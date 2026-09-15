import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

from tools.verify_consumer_resource_abi import (
    APP, INPASS_CHANGED, INPASS_HASH, SECONDARY_CONTROL_HASH,
    SECONDARY_EXECUTED_HASH, TEXEL_FORMAT_CASES, TITLE,
    DRAW_PARAMETER_CASES, DRAW_PARAMETER_COVERED_MINIMUM,
    TWO_SUBPASS_CHANGED, TWO_SUBPASS_FIRST_HASH, TWO_SUBPASS_HASH,
    TWO_SUBPASS_REVERSED_HASH, TWO_SUBPASS_SECOND_HASH, validate)


DRAW_PARAMETER_VERT_SHA256 = "1" * 64
DRAW_PARAMETER_FRAG_SHA256 = "2" * 64


def draw_parameter_messages(first_serial=13,
                            covered=DRAW_PARAMETER_COVERED_MINIMUM + 300):
    """The witness rows as the hardware emits them, from the verifier's table."""
    rows = ["PS5VK_CONSUMER_DRAW_PARAMETERS_START cases=6 extent=64"]
    serial = first_serial
    for name, base_vertex, base_instance, draw_index in DRAW_PARAMETER_CASES:
        rows.extend([
            f"PS5VK_GRAPHICS_PREPARED serial={serial} draws=1 words=256",
            f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
            f"PS5VK_GRAPHICS_SUSPEND_POINT serial={serial} rc=0",
            f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=131072",
            f"PS5VK_CONSUMER_DRAW_PARAMETERS case={name} "
            f"base_vertex={base_vertex} base_instance={base_instance} "
            f"draw_index={draw_index} covered={covered} uniform=1 valid=1",
        ])
        serial += 1
    rows.append("PS5VK_CONSUMER_DRAW_PARAMETERS_RESULT cases=6 witnessed=6 valid=1")
    rows.append("PS5VK_CONSUMER_DRAW_PARAMETERS_RETIRED cases=6 witnessed=6")
    return rows


# Secondary execution INSIDE a render pass, as the hardware emitted it. The
# same triangle reaches the same attachment twice - once through an inherited
# continuation secondary named by a primary that records no draw of its own,
# once recorded inline - so the two readbacks are the same image. A second
# continuation secondary is recorded against the same scope and deliberately
# never named.
INPASS_MESSAGES = [
    "PS5VK_CONSUMER_INPASS_SECONDARY_START",
    "PS5VK_CONSUMER_INPASS_SECONDARY_SUCCESS named=1 unnamed_recorded=1 "
    f"executed_changed={INPASS_CHANGED} control_changed={INPASS_CHANGED} "
    f"bad_alpha=0 bad_sum=0 executed_hash={INPASS_HASH} "
    f"control_hash={INPASS_HASH}",
    "PS5VK_CONSUMER_INPASS_SECONDARY_RETIRED",
]

TWO_SUBPASS_MESSAGES = [
    "PS5VK_CONSUMER_TWO_SUBPASS_START",
    "PS5VK_CONSUMER_TWO_SUBPASS_SUCCESS "
    f"multi={TWO_SUBPASS_HASH} ordered={TWO_SUBPASS_HASH} "
    f"first={TWO_SUBPASS_FIRST_HASH} second={TWO_SUBPASS_SECOND_HASH} "
    f"reversed={TWO_SUBPASS_REVERSED_HASH} changed={TWO_SUBPASS_CHANGED} "
    "negative_distinct=1 bad_alpha=0 bad_sum=0",
    "PS5VK_CONSUMER_TWO_SUBPASS_RETIRED",
]


# The executable-secondary scenario as the hardware emitted it. The payload
# allocates two 64-byte destination buffers of guard bytes, records the same
# 32-byte fill into two secondaries and names only the first, so the executed
# buffer carries the pattern in its first half and the control buffer keeps
# all of its guard bytes.
SECONDARY_MESSAGES = [
    "PS5VK_CONSUMER_SECONDARY_EXECUTE_START",
    "PS5VK_CONSUMER_SECONDARY_EXECUTE_SUCCESS filled_bytes=32 guard_bytes=32 "
    "executed_mismatches=0 control_mismatches=0 control_untouched=1 "
    f"executed_hash={SECONDARY_EXECUTED_HASH} "
    f"control_hash={SECONDARY_CONTROL_HASH}",
    "PS5VK_CONSUMER_SECONDARY_EXECUTE_RETIRED",
]


MESSAGES = [
    "PS5VK_CONSUMER_BOOT mode=finite sdk_version=0.1",
    "PS5VK_CONSUMER_PHYSICAL_DEVICE api=00400000 vendor=1002 device=0000 "
    "heap=268435456 heap_flags=00000001 type_flags=00000003 "
    "queue_flags=00000003 storage=268435456 uniform=65536 texel=65536 "
    "push=256 allocations=2048 granularity=131072 map_align=64 "
    "texel_align=4 ubo_align=256 ssbo_align=256 atom=64 shared=65536 "
    "invocations=1024 hash=be169e1b",
    "PS5VK_CONSUMER_PHYSICAL_QUERIES devices=1 queues=1 two_call=1 "
    "tail_preserved=1 pnext_preserved=1 formats=5 image_supported=1 "
    "image_rejected=1",
    "PS5VK_CONSUMER_STORAGE_WIDTH_NEGOTIATED instance_ext=1 device_exts=4 "
    "storageBuffer8BitAccess=1 storageBuffer16BitAccess=1 narrow_arithmetic=0 "
    "robustBufferAccess=1 shaderDrawParameters=1",
    "PS5VK_CONSUMER_BUFFER_TRANSFER_START",
    "PS5VK_CONSUMER_BUFFER_TRANSFER_SUCCESS copy_bytes=7 update_bytes=8 "
    "fill_bytes=20 whole_tail_bytes=3 guard_mismatches=0 hash=9a158222",
    "PS5VK_CONSUMER_BUFFER_TRANSFER_RETIRED",
    "PS5VK_CONSUMER_COMPUTE_START",
    "PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED",
    "PS5VK_CONSUMER_DISPATCH_INDIRECT_RECORDED groups=1,1,1 offset=512",
    "PS5VK_QUEUE_PREPARED serial=5 dispatches=1",
    "PS5VK_QUEUE_SUBMIT serial=5 index=0 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=5 index=0 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=5 index=0 token=500000001 gcr=0070f528",
    "PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS sets=3 storage=2 uniform=1 texel=1 "
    "dynamic_ssbo=2 dynamic_ubo=1 offsets=256,256,256 base_plus_dynamic=1 "
    "push_bytes=4 spec_constants=2 multiplier=5 extra_bias=11 addend=19 "
    "elements=64 mismatches=0 guard_words=192 guard_mismatches=0",
    "PS5VK_CONSUMER_STORAGE_WIDTH_START",
    "PS5VK_CONSUMER_STORAGE_WIDTH_PIPELINES_CREATED count=2",
    "PS5VK_QUEUE_PREPARED serial=6 dispatches=2",
    "PS5VK_QUEUE_SUBMIT serial=6 index=0 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=6 index=0 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=6 index=0 token=100000002 gcr=0070f528",
    "PS5VK_QUEUE_SUBMIT serial=6 index=1 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=6 index=1 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=6 index=1 token=100000003 gcr=0070f528",
    "PS5VK_CONSUMER_STORAGE_WIDTH_SUCCESS storage8=1 storage16=1 "
    "elements8=64 elements16=64 checksum8=9575e8c5 checksum16=603ddade "
    "mismatches8=0 mismatches16=0 guard_bytes8=4032 guard_bytes16=3968 "
    "guard_mismatches8=0 guard_mismatches16=0",
    "PS5VK_CONSUMER_STORAGE_WIDTH_RETIRED",
    "PS5VK_CONSUMER_SYNC_START",
    "PS5VK_QUEUE_PREPARED serial=8 dispatches=3",
    "PS5VK_QUEUE_SUBMIT serial=8 index=0 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=8 index=0 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=8 index=0 token=400000001 gcr=0070f528",
    "PS5VK_QUEUE_SUBMIT serial=8 index=1 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=8 index=1 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=8 index=1 token=400000002 gcr=0070f528",
    "PS5VK_QUEUE_SUBMIT serial=8 index=2 rc=0",
    "PS5VK_QUEUE_SUSPEND_POINT serial=8 index=2 rc=0",
    "PS5VK_QUEUE_COMPLETED serial=8 index=2 token=400000003 gcr=0070f528",
    "PS5VK_QUEUE_PREPARED serial=10 dispatches=0",
    "PS5VK_CONSUMER_SYNC_OBJECTS_SUCCESS host_set_reset=1 "
    "device_set_wait_reset=1 binary_signal_wait=1 semaphore_consumed=1",
    "PS5VK_CONSUMER_SYNC_SUCCESS producer_consumer=1 host_compute_host=1 "
    "local_size=128 waves32=4 lds_atomic=1 permutation=1 counter=128 "
    "sync_hash=467e2acd atomic_hash=1234abcd mismatches=0 guard_mismatches=0",
    "PS5VK_CONSUMER_SYNC_RETIRED",
    "PS5VK_CONSUMER_TEST_SUCCESS",
    "PS5VK_CONSUMER_RESOURCES_RETIRED zero_tracked_allocations=1",
    "PS5VK_READY_FOR_SHELL_CLOSE resources_retired=1",
]

FIXED_FUNCTION_MESSAGES = [
    "PS5VK_CONSUMER_GRAPHICS_START mode=finite",
    "PS5VK_CONSUMER_GRAPHICS_PIPELINE_COLD_CREATED",
    "PS5VK_CONSUMER_GRAPHICS_PIPELINE_WARM_CREATED",
    "PS5VK_CONSUMER_GRAPHICS_PIPELINE_DESTROYED refcount_verified=1",
    "PS5VK_CONSUMER_DYNAMIC_PIPELINE_CREATED viewport=1 scissor=1",
    "PS5VK_CONSUMER_PRESENT_SURFACE_CREATED buffers=2",
]
for frame in range(18):
    for phase in range(2):
        serial = 13 + frame * 2 + phase
        FIXED_FUNCTION_MESSAGES.extend([
            f"PS5VK_GRAPHICS_PREPARED serial={serial} draws=1 words=256",
            f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
            f"PS5VK_GRAPHICS_SUSPEND_POINT serial={serial} rc=0",
            f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=8388608",
        ])
        if phase == 0:
            FIXED_FUNCTION_MESSAGES.append(
                f"PS5VK_CONSUMER_DEPTH_REJECT frame={frame} nonblack=0 valid=1")
        else:
            FIXED_FUNCTION_MESSAGES.append(
                f"PS5VK_CONSUMER_READBACK frame={frame} slot={frame & 1} "
                "changed=471744 bad_alpha=0 bad_sum=0 valid=1")
FIXED_FUNCTION_MESSAGES.append("PS5VK_CONSUMER_PRESENT_SURFACE_DESTROYED")
MESSAGES[-3:-3] = FIXED_FUNCTION_MESSAGES


class ConsumerResourceAbiTests(unittest.TestCase):
    def fixture(self, edit=None, sampled=False, shared=False, visibility=None,
                single=False, mixed=False, secondary=False, inpass=False,
                texel_formats=False, two_subpass=False, draw_parameters=False):
        sampled = sampled or shared or single or mixed
        messages = list(MESSAGES)
        if draw_parameters:
            # The witness runs after the last offscreen sampled case and before
            # the presentation graphics block, exactly like the payload. Its
            # submissions take the first serials of that block, so the
            # presentation rows shift by the number of cases.
            for index, message in enumerate(messages):
                if message.startswith("PS5VK_GRAPHICS_") and " serial=" in message:
                    prefix, rest = message.split(" serial=")
                    serial, tail = rest.split(" ", 1)
                    messages[index] = (f"{prefix} "
                                       f"serial={int(serial) + len(DRAW_PARAMETER_CASES)} "
                                       f"{tail}")
            at = messages.index("PS5VK_CONSUMER_GRAPHICS_START mode=finite")
            messages[at:at] = draw_parameter_messages()
        if inpass:
            # The scenario runs after the last finite frame and before the
            # surface is destroyed, and costs exactly two extra graphics
            # submissions: the secondary-executed pass and the inline control.
            last = max(index for index, message in enumerate(messages)
                       if message.startswith("PS5VK_CONSUMER_READBACK "))
            serial = 13 + 36
            extra = []
            for _ in range(2):
                extra.extend([
                    f"PS5VK_GRAPHICS_PREPARED serial={serial} draws=1 words=256",
                    f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
                    f"PS5VK_GRAPHICS_SUSPEND_POINT serial={serial} rc=0",
                    f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=8388608",
                ])
                serial += 1
            messages[last + 1:last + 1] = [INPASS_MESSAGES[0]] + extra + \
                INPASS_MESSAGES[1:]
        if two_subpass:
            serials = [int(message.split(" serial=", 1)[1].split()[0])
                       for message in messages
                       if message.startswith("PS5VK_GRAPHICS_PREPARED ")]
            serial = max(serials) + 1
            rows = [TWO_SUBPASS_MESSAGES[0]]
            for scenario, draws in enumerate((2, 2, 1, 1, 2)):
                if scenario == 0:
                    rows.append(
                        f"PS5VK_SUBPASS_BOUNDARY serial={serial} subpass=1 words=10")
                rows.extend([
                    f"PS5VK_GRAPHICS_PREPARED serial={serial} draws={draws} words=256",
                    f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
                    f"PS5VK_GRAPHICS_SUSPEND_POINT serial={serial} rc=0",
                    f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=8388608",
                ])
                serial += 1
            rows.extend(TWO_SUBPASS_MESSAGES[1:])
            at = messages.index("PS5VK_CONSUMER_PRESENT_SURFACE_DESTROYED")
            messages[at:at] = rows
        if secondary:
            # Executing one named secondary costs exactly two extra compute
            # segments, one for the child and one for the parent that names it,
            # so every later submission serial - and the serial-derived first
            # completion token - moves up by two.
            for index, message in enumerate(messages):
                if message.startswith("PS5VK_QUEUE_") and " serial=" in message:
                    prefix, rest = message.split(" serial=")
                    value, tail = rest.split(" ", 1)
                    tail = tail.replace(f"token={value}00000001",
                                        f"token={int(value) + 2}00000001")
                    messages[index] = f"{prefix} serial={int(value) + 2} {tail}"
            at = messages.index("PS5VK_CONSUMER_COMPUTE_START")
            messages[at:at] = list(SECONDARY_MESSAGES)
        if texel_formats:
            rows = [f"PS5VK_CONSUMER_TEXEL_FORMATS_START cases={len(TEXEL_FORMAT_CASES)}",
                    f"PS5VK_QUEUE_PREPARED serial=57 dispatches={len(TEXEL_FORMAT_CASES)}"]
            for index in range(len(TEXEL_FORMAT_CASES)):
                rows.extend([
                    f"PS5VK_QUEUE_SUBMIT serial=57 index={index} rc=0",
                    f"PS5VK_QUEUE_SUSPEND_POINT serial=57 index={index} rc=0",
                    f"PS5VK_QUEUE_COMPLETED serial=57 index={index} token={index + 1:09x} gcr=0070f528",
                ])
            for index, (name, shader_class, byte_count, expected) in enumerate(TEXEL_FORMAT_CASES):
                rows.append(
                    f"PS5VK_CONSUMER_TEXEL_FORMAT_CASE index={index} name={name} "
                    f"class={shader_class} bytes={byte_count} expected={expected} "
                    f"actual={expected} mismatches=0")
            rows.extend([
                f"PS5VK_CONSUMER_TEXEL_FORMATS_SUCCESS cases={len(TEXEL_FORMAT_CASES)} "
                f"components={len(TEXEL_FORMAT_CASES) * 4} mismatches=0 guard_mismatches=0",
                "PS5VK_CONSUMER_TEXEL_FORMATS_RETIRED",
            ])
            at = messages.index("PS5VK_CONSUMER_TEST_SUCCESS")
            messages[at:at] = rows
        if sampled:
            for index,message in enumerate(messages):
                if message.startswith("PS5VK_GRAPHICS_") and " serial=" in message:
                    prefix,rest=message.split(" serial=")
                    serial,tail=rest.split(" ",1)
                    messages[index]=f"{prefix} serial={int(serial)+4} {tail}"
            if single:
                rows=["PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_START sets=1 descriptors=96"
                      " elements=96 rounds=4 visibility=00000010 fs_sha256="+"f"*64]
                marker="PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RESULT"
                retired="PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RETIRED"
                words=("914c503b","914b4d4f","914a4643","913a5449")
            elif mixed:
                rows=["PS5VK_CONSUMER_MIXED_SETS_START sets=4 descriptors=96"
                      " uniform_buffers=4 rounds=4 visibility=00000010"
                      " fs_sha256="+"f"*64]
                words=("381e1f17","4b29272a","5d2d2c2c","6f2c4038")
                marker="PS5VK_CONSUMER_MIXED_SETS_RESULT"
                retired="PS5VK_CONSUMER_MIXED_SETS_RETIRED"
            else:
                rows=["PS5VK_CONSUMER_SAMPLED_SETS_START sets=4 descriptors=96 rounds=4"]
                marker="PS5VK_CONSUMER_SAMPLED_SETS_RESULT"
                retired="PS5VK_CONSUMER_SAMPLED_SETS_RETIRED"
                words=("914c503b","914b4d4f","914a4643","913a5449")
                if shared:
                    rows[0]+=" stages=vertex-fragment vs_sha256="+"e"*64+" fs_sha256="+"f"*64
                    if visibility is not None:rows[0]+=f" visibility={visibility:08x}"
                    words=("6d3a3b2f","6d373d35","6d42392a","6d2e4135")
            for round_index,word in enumerate(words):
                serial=13+round_index
                rows.extend([
                    f"PS5VK_GRAPHICS_PREPARED serial={serial} draws=1 words=256",
                    f"PS5VK_GRAPHICS_SUBMIT serial={serial} rc=0",
                    f"PS5VK_GRAPHICS_SUSPEND_POINT serial={serial} rc=0",
                    f"PS5VK_GRAPHICS_COMPLETED serial={serial} image_bytes=8388608",
                    f"{marker} round={round_index} expected={word} changed=471744 bad=0",
                ])
            rows.append(retired)
            at=messages.index("PS5VK_CONSUMER_GRAPHICS_START mode=finite")
            messages[at:at]=rows
        if edit:
            edit(messages)
        log = (f"HELLO ps5log/1 title={TITLE} app={APP} boot=test\n" +
               "".join(f"{i}\t{i}\tMARK\t{message}\n"
                       for i, message in enumerate(messages, 1)) +
               f"BYE seq={len(messages)} reason=consumer-finite-end\n").encode()
        receipt = {
            "sha256": hashlib.sha256(log).hexdigest(), "protocol": "ps5log/1",
            "transport": "tcp", "clean": True, "bye": True, "gaps": [],
            "raw_lines": 0, "last_seq": len(messages), "run_id": "synthetic",
            "identity": {"title": TITLE, "app": APP, "boot": "test"},
        }
        artifact = {
            "title": TITLE, "profile": "public-consumer-resource-abi",
            "submit_enabled": True, "files": {"eboot.bin": "a" * 64},
            "draw_parameters": {
                "cases": [case[0] for case in DRAW_PARAMETER_CASES],
                "vertex_shader_sha256": DRAW_PARAMETER_VERT_SHA256,
                "fragment_shader_sha256": DRAW_PARAMETER_FRAG_SHA256,
            },
            "buffer_transfer": {
                "api": "Vulkan 1.0", "copy_bytes": 7,
                "update_bytes": 8, "fill_bytes": 20,
                "whole_tail_bytes": 3,
            },
            "indirect_dispatch": {
                "api": "Vulkan 1.0", "groups": [1, 1, 1],
                "offset": 512, "result_elements": 64,
            },
            "dynamic_descriptors": {
                "storage_buffers": 2, "uniform_buffers": 1,
                "offsets": [256, 256, 256], "base_plus_dynamic": True,
                "result_elements": 64, "guard_words": 192,
            },
            "storage_width": {
                "storageBuffer8BitAccess": True,
                "storageBuffer16BitAccess": True,
                "shaderInt8": False,
                "shaderInt16": False,
                "storage8_spirv_sha256": "b" * 64,
                "storage16_spirv_sha256": "c" * 64,
            },
            "synchronization": {
                "api": "Vulkan 1.0", "local_size": 128, "wave_size": 32,
                "binary_semaphore": True, "host_event": True,
                "device_event": True,
                "sync_producer_spirv_sha256": "d" * 64,
                "sync_consumer_spirv_sha256": "e" * 64,
                "shared_atomic_multiwave_spirv_sha256": "f" * 64,
            },
            "fixed_function": {
                "api": "Vulkan 1.0", "width": 1920, "height": 1080,
                "frames": 18, "color_format": "VK_FORMAT_B8G8R8A8_UNORM",
                "depth_format": "VK_FORMAT_D32_SFLOAT", "samples": 1,
                "load_preservation": True, "dynamic_viewport": True,
                "dynamic_scissor": True,
            },
        }
        if sampled:
            artifact["sampled_graphics"]={"sets":4,"descriptors":96,"rounds":4,
                                         "shader_spirv_sha256":"f"*64}
            if single:
                artifact["sampled_graphics"].update(stage_profile="single-set",sets=1,
                                                    elements_per_set=96,
                                                    visibility_mask=0x10)
            elif mixed:
                artifact["sampled_graphics"].update(stage_profile="mixed-resources",
                                                    uniform_buffers=4,
                                                    visibility_mask=0x10)
            elif shared:
                artifact["sampled_graphics"].update(stage_profile="vertex-fragment", vertex_spirv_sha256="e"*64)
                if visibility is not None:artifact["sampled_graphics"]["visibility_mask"]=visibility
        if texel_formats:
            artifact["texel_formats"] = {
                "case_count": len(TEXEL_FORMAT_CASES),
                "components_per_case": 4,
                "shader_spirv_sha256": {
                    "float": "1" * 64, "uint": "2" * 64, "sint": "3" * 64,
                },
            }
        return log, receipt, artifact

    def test_uniform_texel_format_matrix_is_strict(self):
        result = validate(*self.fixture(texel_formats=True), texel_formats=True)
        self.assertEqual(result["uniform_texel_formats_checked"], 41)

        def rejected(edit):
            log, receipt, artifact = self.fixture(edit=edit, texel_formats=True)
            with self.assertRaises(ValueError):
                validate(log, receipt, artifact, texel_formats=True)

        def corrupt_case(messages):
            index = next(i for i, message in enumerate(messages)
                         if "TEXEL_FORMAT_CASE index=8 " in message)
            messages[index] = messages[index].replace("actual=3f800000", "actual=00000000")

        def drop_completion(messages):
            messages.remove(next(message for message in messages
                            if message.startswith("PS5VK_QUEUE_COMPLETED serial=57 index=40 ")))

        def corrupt_guard(messages):
            index = messages.index(next(message for message in messages
                if message.startswith("PS5VK_CONSUMER_TEXEL_FORMATS_SUCCESS ")))
            messages[index] = messages[index].replace("guard_mismatches=0", "guard_mismatches=1")

        for edit in (corrupt_case, drop_completion, corrupt_guard):
            with self.subTest(edit=edit.__name__):
                rejected(edit)

    def test_shared_visibility_is_exact_and_cannot_be_downgraded(self):
        for mask in (0x11,0x1f,0x7fffffff):
            log,receipt,artifact=self.fixture(shared=True,visibility=mask)
            self.assertEqual(validate(log,receipt,artifact)["sampled_graphics_visibility_mask"],mask)
            for wrong in (0,0x40000000,0x10,"all",None,True,0x11 if mask!=0x11 else 0x1f):
                artifact["sampled_graphics"]["visibility_mask"]=wrong
                with self.subTest(mask=mask,wrong=wrong),self.assertRaises(ValueError):validate(log,receipt,artifact)

    def test_mixed_profile_is_strict_and_cannot_be_confused_with_sampler_only(self):
        log,receipt,artifact=self.fixture(mixed=True)
        result=validate(log,receipt,artifact)
        self.assertEqual(result["mixed_resource_uniform_buffers"],4)

        def mutated(edit):
            log,receipt,artifact=self.fixture(mixed=True,edit=edit)
            with self.assertRaises(ValueError):
                validate(log,receipt,artifact)

        def drop_round(messages):
            messages.remove(next(m for m in messages
                if m.startswith("PS5VK_CONSUMER_MIXED_SETS_RESULT round=2")))
        def duplicate_round(messages):
            row=next(m for m in messages
                     if m.startswith("PS5VK_CONSUMER_MIXED_SETS_RESULT round=1"))
            messages.insert(messages.index(row),row)
        def wrong_pixel(messages):
            index=messages.index(next(m for m in messages
                if m.startswith("PS5VK_CONSUMER_MIXED_SETS_RESULT round=3")))
            messages[index]=messages[index].replace("expected=6f2c4038","expected=6f2c4039")
        def leaked_sampler_markers(messages):
            messages.insert(messages.index("PS5VK_CONSUMER_MIXED_SETS_RETIRED"),
                            "PS5VK_CONSUMER_SAMPLED_SETS_RESULT round=0 "
                            "expected=914c503b changed=471744 bad=0")
        def missing_uniform_marker(messages):
            index=messages.index(next(m for m in messages
                if m.startswith("PS5VK_CONSUMER_MIXED_SETS_START")))
            messages[index]=messages[index].replace(" uniform_buffers=4","")

        for edit in (drop_round,duplicate_round,wrong_pixel,leaked_sampler_markers,
                     missing_uniform_marker):
            with self.subTest(edit=edit.__name__):
                mutated(edit)

        # Artifact-side claims must match the run: a stale shader hash, a wrong
        # uniform count, a downgraded visibility mask or a sampler-only artifact
        # presented with mixed markers are all rejected.
        for key,value in (("shader_spirv_sha256","0"*64),
                          ("uniform_buffers",3),
                          ("visibility_mask",0x11),
                          ("stage_profile","fragment")):
            log,receipt,artifact=self.fixture(mixed=True)
            artifact["sampled_graphics"][key]=value
            with self.subTest(key=key),self.assertRaises(ValueError):
                validate(log,receipt,artifact)
        # A sampler-only artifact cannot claim the mixed profile.
        log,receipt,artifact=self.fixture(sampled=True)
        artifact["sampled_graphics"]["uniform_buffers"]=4
        with self.assertRaises(ValueError):
            validate(log,receipt,artifact)

    def test_shared_visibility_default_and_absence_are_exact(self):
        log,receipt,artifact=self.fixture(shared=True)
        self.assertEqual(validate(log,receipt,artifact)["sampled_graphics_visibility_mask"],0x11)
        artifact["sampled_graphics"]["visibility_mask"]=0x7fffffff
        with self.assertRaises(ValueError):validate(log,receipt,artifact)
        log,receipt,artifact=self.fixture(sampled=True)
        artifact["sampled_graphics"]["visibility_mask"]=0x10
        with self.assertRaises(ValueError):validate(log,receipt,artifact)
        # An explicit mask in the log cannot be dropped from the artifact.
        log,receipt,artifact=self.fixture(shared=True,visibility=0x1f)
        del artifact["sampled_graphics"]["visibility_mask"]
        with self.assertRaises(ValueError):validate(log,receipt,artifact)
        artifact["sampled_graphics"]["visibility_mask"]=0x10
        with self.assertRaises(ValueError):validate(log,receipt,artifact)

    def test_single_set_capacity_is_strict_and_bound_to_one_set(self):
        log,receipt,artifact=self.fixture(single=True)
        result=validate(log,receipt,artifact)
        self.assertEqual(result["single_set_sampler_elements"],96)
        self.assertEqual(result["sampled_graphics_stage_profile"],"single-set")

        def mutated(edit):
            with self.assertRaises(ValueError):
                validate(*self.fixture(edit,single=True))

        def drop_round(messages):
            messages.remove(next(m for m in messages
                if m.startswith("PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RESULT round=2")))
        def duplicate_round(messages):
            row=next(m for m in messages
                     if m.startswith("PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RESULT round=1"))
            messages.insert(messages.index(row),row)
        def wrong_pixel(messages):
            index=messages.index(next(m for m in messages
                if m.startswith("PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RESULT round=0")))
            messages[index]=messages[index].replace("expected=914c503b","expected=914c503c")
        def missing_elements(messages):
            index=messages.index(next(m for m in messages
                if m.startswith("PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_START")))
            messages[index]=messages[index].replace(" elements=96","")
        def leaked_four_set_marker(messages):
            messages.insert(messages.index("PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RETIRED"),
                            "PS5VK_CONSUMER_SAMPLED_SETS_RESULT round=0 "
                            "expected=914c503b changed=471744 bad=0")

        for edit in (drop_round,duplicate_round,wrong_pixel,missing_elements,
                     leaked_four_set_marker):
            with self.subTest(edit=edit.__name__):
                mutated(edit)

        # The artifact must claim exactly the one-set ninety-six-element profile
        # the run proves; a four-set shape or any wrong count is rejected.
        for key,value in (("sets",4),("elements_per_set",95),("elements_per_set",97),
                          ("descriptors",95),("visibility_mask",0x11),
                          ("shader_spirv_sha256","0"*64)):
            log,receipt,artifact=self.fixture(single=True)
            artifact["sampled_graphics"][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                validate(log,receipt,artifact)
        log,receipt,artifact=self.fixture(single=True)
        artifact["sampled_graphics"]={"sets":4,"descriptors":96,"rounds":4,
                                      "shader_spirv_sha256":"f"*64}
        with self.assertRaises(ValueError):
            validate(log,receipt,artifact)
        # And a single-set artifact cannot carry four-set markers.
        log,receipt,artifact=self.fixture(sampled=True)
        artifact["sampled_graphics"].update(stage_profile="single-set",sets=1,
                                            elements_per_set=96,visibility_mask=0x10)
        with self.assertRaises(ValueError):
            validate(log,receipt,artifact)

    def test_visibility_cli_rejects_unknown_or_unexecuted_modes(self):
        builder=Path(__file__).resolve().parents[1]/"tools/build_consumer.py"
        for args in (("--sampler-visibility","unknown"),("--sampler-visibility","all"),
                     ("--continuous","--shared-stage-samplers","--sampler-visibility","all"),
                     ("--continuous","--single-set-samplers"),
                     ("--shared-stage-samplers","--single-set-samplers")):
            result=subprocess.run([sys.executable,str(builder),*args],capture_output=True,text=True)
            self.assertEqual(result.returncode,2,result.stdout+result.stderr)

    def test_shared_sampler_profile_and_hashes_are_bound_to_evidence(self):
        result=validate(*self.fixture(shared=True))
        self.assertEqual(result["sampled_graphics_stage_profile"],"vertex-fragment")
        self.assertTrue(result["sampled_graphics_exceeds_advertised_limits"])
        for key,value in (("stage_profile","fragment"),("stage_profile","unknown"),
                          ("vertex_spirv_sha256","d"*64),("shader_spirv_sha256","c"*64),
                          ("vertex_spirv_sha256","bad")):
            log,receipt,artifact=self.fixture(shared=True)
            artifact["sampled_graphics"][key]=value
            with self.subTest(key=key,value=value),self.assertRaises(ValueError):
                validate(log,receipt,artifact)
        for key in ("stage_profile","vertex_spirv_sha256","shader_spirv_sha256"):
            log,receipt,artifact=self.fixture(shared=True)
            del artifact["sampled_graphics"][key]
            with self.subTest(missing=key),self.assertRaises(ValueError):validate(log,receipt,artifact)

    def test_shared_sampler_rejects_missing_duplicate_or_wrong_stage_contribution(self):
        for old,new in (("expected=6d3a3b2f","expected=914c503b"),
                        ("expected=6d3a3b2f","expected=00000000"),
                        ("round=3","round=2"),("changed=471744","changed=0"),
                        ("bad=0","bad=1")):
            def edit(messages):
                index=next(i for i,m in enumerate(messages) if old in m)
                messages[index]=messages[index].replace(old,new)
            with self.subTest(old=old),self.assertRaises(ValueError):validate(*self.fixture(edit,shared=True))
        for prefix in ("PS5VK_CONSUMER_SAMPLED_SETS_START","PS5VK_CONSUMER_SAMPLED_SETS_RESULT",
                       "PS5VK_CONSUMER_SAMPLED_SETS_RETIRED","PS5VK_GRAPHICS_COMPLETED serial=13"):
            def edit(messages):
                messages.pop(next(i for i,m in enumerate(messages) if m.startswith(prefix)))
            with self.subTest(prefix=prefix),self.assertRaises(ValueError):validate(*self.fixture(edit,shared=True))

    def test_both_c_pixel_oracles_match_independent_literals(self):
        # Compile the actual consumer's oracle prefix, not a Python reimplementation.
        source=(Path(__file__).resolve().parents[1]/"examples/native_consumer/sampled_sets.h").read_text()
        source=source.split("static void run_sampled_sets",1)[0].replace(
            '#include "sampled_set_shaders.h"','#include <stdint.h>\n#include <stdio.h>')
        source+='\nint main(void){for(unsigned i=0;i<4;++i)printf("%08x\\n",sampled_expected(i));}\n'
        with tempfile.TemporaryDirectory() as directory:
            binary=Path(directory)/"oracle"
            for shared,expected in ((False,["914c503b","914b4d4f","914a4643","913a5449"]),
                                    (True,["6d3a3b2f","6d373d35","6d42392a","6d2e4135"])):
                flags=["-DCONSUMER_SHARED_STAGE_SAMPLERS=1"] if shared else []
                subprocess.run(["cc","-std=c11","-Wall","-Werror",*flags,"-x","c","-","-o",str(binary)],
                               input=source,text=True,check=True,capture_output=True)
                self.assertEqual(subprocess.check_output([str(binary)],text=True).splitlines(),expected)

    def test_sampled_sets_reference_and_mutations(self):
        result=validate(*self.fixture(sampled=True))
        self.assertTrue(result["sampled_graphics_exceeds_advertised_limits"])
        self.assertEqual(result["graphics_submissions_checked"],40)
        self.assertEqual(result["sampled_graphics_descriptors_per_round"],96)
        for old,new in (("bad=0","bad=1"),("changed=471744","changed=1"),
                        ("expected=914c503b","expected=ffffffff"),
                        ("round=3","round=2"),("serial=13 image_bytes","serial=14 image_bytes")):
            def edit(messages):
                index=next(i for i,m in enumerate(messages) if old in m)
                messages[index]=messages[index].replace(old,new)
            with self.subTest(old=old),self.assertRaises(ValueError):
                validate(*self.fixture(edit,sampled=True))

    def test_sampled_sets_evidence_cannot_be_omitted(self):
        for prefix in ("PS5VK_CONSUMER_SAMPLED_SETS_START", "PS5VK_CONSUMER_SAMPLED_SETS_RESULT",
                       "PS5VK_CONSUMER_SAMPLED_SETS_RETIRED", "PS5VK_GRAPHICS_COMPLETED serial=13"):
            def edit(messages):
                messages.pop(next(i for i,m in enumerate(messages) if m.startswith(prefix)))
            with self.subTest(prefix=prefix),self.assertRaises(ValueError):
                validate(*self.fixture(edit,sampled=True))
        log,receipt,artifact=self.fixture(sampled=True)
        del artifact["sampled_graphics"]
        with self.assertRaises(ValueError):validate(log,receipt,artifact)
        log,receipt,artifact=self.fixture()
        artifact["sampled_graphics"]={"sets":4,"descriptors":96,"rounds":4,"shader_spirv_sha256":"f"*64}
        with self.assertRaises(ValueError):validate(log,receipt,artifact)

    def test_reference(self):
        result = validate(*self.fixture())
        self.assertFalse(result["sampled_graphics_exceeds_advertised_limits"])
        self.assertEqual(result["descriptor_sets"], 3)
        self.assertEqual(result["guard_words_checked"], 192)
        self.assertEqual(result["dynamic_storage_buffers"], 2)
        self.assertEqual(result["dynamic_uniform_buffers"], 1)
        self.assertEqual(result["push_constant_bytes"], 4)
        self.assertEqual(result["specialization_constants"], 2)
        self.assertEqual(result["indirect_dispatches_checked"], 1)
        self.assertEqual(result["storage8_elements_checked"], 64)
        self.assertEqual(result["storage16_elements_checked"], 64)
        self.assertEqual(result["narrow_guard_bytes_checked"], 8000)
        self.assertEqual(result["physical_device_report_fnv1a32"], "be169e1b")
        self.assertFalse(result["reported_host_coherent"])
        self.assertEqual(result["multiwave_atomic_lanes_checked"], 128)
        self.assertEqual(result["wave32_count"], 4)

    def test_every_witness_is_required(self):
        for index in range(len(MESSAGES)):
            with self.subTest(index=index), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.pop(index)))

    def test_corrupt_or_false_success_is_rejected(self):
        replacements = (("mismatches=0", "mismatches=1"),
                        ("guard_mismatches=0", "guard_mismatches=1"),
                        ("sets=3", "sets=1"), ("rc=0", "rc=-1"),
                        ("serial=5 dispatches=1", "serial=5 dispatches=3"),
                        ("hash=9a158222", "hash=00000000"),
                        ("checksum8=9575e8c5", "checksum8=00000000"),
                        ("checksum16=603ddade", "checksum16=00000000"),
                        ("sync_hash=467e2acd", "sync_hash=00000000"),
                        ("permutation=1", "permutation=0"),
                        ("heap=268435456", "heap=268435455"),
                        ("hash=be169e1b", "hash=00000000"),
                        ("pnext_preserved=1", "pnext_preserved=0"),
                        ("allocations=1", "allocations=0"))
        for old, new in replacements:
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(lambda messages: messages.__setitem__(
                    slice(None), [message.replace(old, new) for message in messages])))

    def test_transport_identity_and_bye_are_fail_closed(self):
        for mutate in (
            lambda log, receipt, artifact: receipt.update(gaps=[{"expected": 2, "got": 3}]),
            lambda log, receipt, artifact: receipt.update(clean=False),
            lambda log, receipt, artifact: artifact.update(profile="other"),
            lambda log, receipt, artifact: artifact["files"].update({"eboot.bin": "bad"}),
            lambda log, receipt, artifact: artifact["storage_width"].update(shaderInt8=True),
            lambda log, receipt, artifact: artifact["storage_width"].update(storage8_spirv_sha256="bad"),
            lambda log, receipt, artifact: artifact["dynamic_descriptors"].update(offsets=[0, 0, 0]),
        ):
            args = list(self.fixture())
            mutate(*args)
            with self.assertRaises(ValueError):
                validate(*args)

    def test_executable_secondary_witness_is_accepted_with_shifted_serials(self):
        result = validate(*self.fixture(secondary=True))
        self.assertTrue(result["secondary_execute_witnessed"])
        self.assertEqual(result["secondary_executed_hash_fnv1a32"],
                         SECONDARY_EXECUTED_HASH)
        self.assertEqual(result["secondary_control_hash_fnv1a32"],
                         SECONDARY_CONTROL_HASH)
        # Payloads without the scenario keep their recorded serials and stay
        # valid; the shift is derived from the scenario's own presence.
        older = validate(*self.fixture())
        self.assertFalse(older["secondary_execute_witnessed"])
        self.assertIsNone(older["secondary_executed_hash_fnv1a32"])
        self.assertIsNone(older["secondary_control_hash_fnv1a32"])

    def test_secondary_hashes_match_an_independent_recomputation(self):
        # The pins are not transcribed from the log: the oracle is fully
        # declared - two 64-byte buffers of 0x5a guard, with the first 32 bytes
        # of the executed one overwritten by the little-endian 0xa1b2c3d4 fill
        # - so both FNV-1a values are recomputed here from that description.
        def fnv1a32(data):
            digest = 0x811c9dc5
            for byte in data:
                digest = ((digest ^ byte) * 0x01000193) & 0xffffffff
            return f"{digest:08x}"

        guard = bytes([0x5a]) * 64
        executed = (0xa1b2c3d4).to_bytes(4, "little") * 8 + guard[32:]
        self.assertEqual(fnv1a32(executed), SECONDARY_EXECUTED_HASH)
        self.assertEqual(fnv1a32(guard), SECONDARY_CONTROL_HASH)
        self.assertNotEqual(SECONDARY_EXECUTED_HASH, SECONDARY_CONTROL_HASH)

    def test_draw_parameter_witness_is_accepted_and_reported(self):
        result = validate(*self.fixture(draw_parameters=True))
        self.assertEqual(result["draw_parameter_cases"], len(DRAW_PARAMETER_CASES))

    def test_draw_parameter_values_are_pinned_per_case(self):
        rows = [row for row in draw_parameter_messages() if " case=" in row]
        self.assertEqual(len(rows), len(DRAW_PARAMETER_CASES))
        for row in rows:
            for field in ("base_vertex", "base_instance", "draw_index"):
                def edit(messages, row=row, field=field):
                    index = messages.index(row)
                    messages[index] = " ".join(
                        f"{field}=99" if part.startswith(f"{field}=") else part
                        for part in messages[index].split())
                with self.subTest(case=row.split()[1], field=field), \
                        self.assertRaises(ValueError):
                    validate(*self.fixture(draw_parameters=True, edit=edit))

    def test_draw_parameter_witness_is_fail_closed(self):
        rows = draw_parameter_messages()
        case_row = next(row for row in rows if " case=" in row)

        def rewrite(row, field, value):
            def edit(messages):
                index = messages.index(row)
                messages[index] = " ".join(
                    f"{field}={value}" if part.startswith(f"{field}=") else part
                    for part in messages[index].split())
            return edit

        for row, field, value in (
                (case_row, "uniform", "0"),
                (case_row, "valid", "0"),
                (case_row, "covered", str(DRAW_PARAMETER_COVERED_MINIMUM - 1)),
                (rows[-2], "witnessed", "5"),
                (rows[-2], "valid", "0")):
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                validate(*self.fixture(draw_parameters=True,
                                       edit=rewrite(row, field, value)))
        # A case the payload did not report is a missing witness, not a
        # smaller run, so dropping one row must fail.
        def drop(messages):
            messages.remove(case_row)
        with self.assertRaises(ValueError):
            validate(*self.fixture(draw_parameters=True, edit=drop))
        # The artifact must declare exactly the same cases and pin both shaders.
        def drop_cases(artifact):
            artifact["draw_parameters"].pop("cases")

        def drop_vertex_hash(artifact):
            artifact["draw_parameters"].pop("vertex_shader_sha256")

        def shrink_cases(artifact):
            artifact["draw_parameters"]["cases"] = ["list_direct"]

        for mutate in (drop_cases, drop_vertex_hash, shrink_cases):
            log, receipt, artifact = self.fixture(draw_parameters=True)
            mutate(artifact)
            with self.subTest(mutate=mutate.__name__), self.assertRaises(ValueError):
                validate(log, receipt, artifact)

    def test_secondary_execute_scenario_cannot_be_half_reported(self):
        for dropped in SECONDARY_MESSAGES:
            def drop(messages, dropped=dropped):
                messages.remove(dropped)
            with self.subTest(dropped=dropped.split()[0]), \
                    self.assertRaises(ValueError):
                validate(*self.fixture(secondary=True, edit=drop))

    def test_secondary_execute_rejects_equal_hashes(self):
        # Identical hashes are what a driver that executed nothing, or that
        # executed the unnamed secondary too, would report.
        for old, new in ((f"control_hash={SECONDARY_CONTROL_HASH}",
                          f"control_hash={SECONDARY_EXECUTED_HASH}"),
                         (f"executed_hash={SECONDARY_EXECUTED_HASH}",
                          f"executed_hash={SECONDARY_CONTROL_HASH}")):
            def edit(messages, old=old, new=new):
                index = messages.index(SECONDARY_MESSAGES[1])
                messages[index] = messages[index].replace(old, new)
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(secondary=True, edit=edit))

    def test_secondary_execute_hashes_are_pinned_not_merely_different(self):
        # A wrong-but-different pair - a bad fill pattern, or a control buffer
        # that was touched - passes a difference check and must still fail.
        for old, new in ((f"executed_hash={SECONDARY_EXECUTED_HASH}",
                          "executed_hash=ca327246"),
                         (f"control_hash={SECONDARY_CONTROL_HASH}",
                          "control_hash=21a49bc6"),
                         ("control_untouched=1", "control_untouched=0"),
                         ("filled_bytes=32", "filled_bytes=16"),
                         ("executed_mismatches=0", "executed_mismatches=1"),
                         ("control_mismatches=0", "control_mismatches=1")):
            def edit(messages, old=old, new=new):
                index = messages.index(SECONDARY_MESSAGES[1])
                messages[index] = messages[index].replace(old, new)
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(secondary=True, edit=edit))


    def test_inpass_secondary_witness_is_accepted_and_reported(self):
        result = validate(*self.fixture(inpass=True))
        self.assertTrue(result["inpass_secondary_witnessed"])
        self.assertEqual(result["inpass_secondary_hash_fnv1a32"], INPASS_HASH)
        self.assertEqual(result["inpass_secondary_changed_pixels"], INPASS_CHANGED)
        self.assertEqual(result["graphics_submissions_checked"], 38)
        # A payload without the scenario keeps its own submission count and
        # reports nothing, so recorded evidence still validates unchanged.
        older = validate(*self.fixture())
        self.assertFalse(older["inpass_secondary_witnessed"])
        self.assertIsNone(older["inpass_secondary_hash_fnv1a32"])
        self.assertEqual(older["graphics_submissions_checked"], 36)

    def test_inpass_secondary_scenario_cannot_be_half_reported(self):
        for dropped in INPASS_MESSAGES:
            def drop(messages, dropped=dropped):
                messages.remove(dropped)
            with self.subTest(dropped=dropped.split()[0]), \
                    self.assertRaises(ValueError):
                validate(*self.fixture(inpass=True, edit=drop))

    def test_inpass_secondary_requires_the_two_images_to_match(self):
        # Unequal hashes are what a driver that executed nothing, or that also
        # executed the deliberately unnamed secondary, would report.
        for old, new in ((f"executed_hash={INPASS_HASH}", "executed_hash=77abc831"),
                         (f"control_hash={INPASS_HASH}", "control_hash=77abc831"),
                         (f"executed_changed={INPASS_CHANGED}", "executed_changed=0"),
                         (f"control_changed={INPASS_CHANGED}", "control_changed=0")):
            def edit(messages, old=old, new=new):
                index = messages.index(INPASS_MESSAGES[1])
                messages[index] = messages[index].replace(old, new)
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(inpass=True, edit=edit))

    def test_inpass_secondary_hashes_are_pinned_not_merely_equal(self):
        # Both sides wrong in the same way still agree with each other; only
        # the pinned image rules that out. The remaining fields are pinned too.
        def both(messages):
            index = messages.index(INPASS_MESSAGES[1])
            messages[index] = messages[index].replace(INPASS_HASH, "77abc831")
        with self.assertRaises(ValueError):
            validate(*self.fixture(inpass=True, edit=both))

        def both_changed(messages):
            index = messages.index(INPASS_MESSAGES[1])
            messages[index] = messages[index].replace(
                str(INPASS_CHANGED), str(INPASS_CHANGED - 1))
        with self.assertRaises(ValueError):
            validate(*self.fixture(inpass=True, edit=both_changed))

        for old, new in (("named=1", "named=0"),
                         ("unnamed_recorded=1", "unnamed_recorded=0"),
                         ("bad_alpha=0", "bad_alpha=1"),
                         ("bad_sum=0", "bad_sum=1")):
            def edit(messages, old=old, new=new):
                index = messages.index(INPASS_MESSAGES[1])
                messages[index] = messages[index].replace(old, new)
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(inpass=True, edit=edit))

    def test_inpass_secondary_witness_must_follow_the_frames(self):
        # Moved before the last frame readback it would be indistinguishable
        # from one of the eighteen frames.
        def hoist(messages):
            for message in INPASS_MESSAGES:
                messages.remove(message)
            at = messages.index("PS5VK_CONSUMER_GRAPHICS_START mode=finite")
            messages[at:at] = INPASS_MESSAGES
        with self.assertRaises(ValueError):
            validate(*self.fixture(inpass=True, edit=hoist))

    def test_two_subpass_witness_is_accepted_and_reported(self):
        result = validate(*self.fixture(inpass=True, two_subpass=True))
        self.assertTrue(result["two_subpass_witnessed"])
        self.assertEqual(result["two_subpass_hash_fnv1a32"], TWO_SUBPASS_HASH)
        self.assertEqual(result["graphics_submissions_checked"], 43)

    def test_two_subpass_witness_is_fail_closed(self):
        for dropped in TWO_SUBPASS_MESSAGES:
            def drop(messages, dropped=dropped):
                messages.remove(dropped)
            with self.subTest(dropped=dropped.split()[0]), \
                    self.assertRaises(ValueError):
                validate(*self.fixture(inpass=True, two_subpass=True, edit=drop))

        mutations = ((f"multi={TWO_SUBPASS_HASH}", "multi=84cc0cb4"),
                     (f"ordered={TWO_SUBPASS_HASH}", "ordered=84cc0cb4"),
                     (f"first={TWO_SUBPASS_FIRST_HASH}", "first=84cc0cb3"),
                     (f"second={TWO_SUBPASS_SECOND_HASH}", "second=84cc0cb3"),
                     ("negative_distinct=1", "negative_distinct=0"))
        for old, new in mutations:
            def mutate(messages, old=old, new=new):
                index = messages.index(TWO_SUBPASS_MESSAGES[1])
                messages[index] = messages[index].replace(old, new, 1)
            with self.subTest(old=old), self.assertRaises(ValueError):
                validate(*self.fixture(inpass=True, two_subpass=True, edit=mutate))

    def test_two_subpass_boundary_is_required_and_ordered(self):
        def drop(messages):
            messages[:] = [m for m in messages
                           if not m.startswith("PS5VK_SUBPASS_BOUNDARY ")]
        with self.assertRaises(ValueError):
            validate(*self.fixture(inpass=True, two_subpass=True, edit=drop))

        def move_after_result(messages):
            index = next(i for i, message in enumerate(messages)
                         if message.startswith("PS5VK_SUBPASS_BOUNDARY "))
            boundary = messages.pop(index)
            result = messages.index(TWO_SUBPASS_MESSAGES[1])
            messages.insert(result + 1, boundary)
        with self.assertRaises(ValueError):
            validate(*self.fixture(inpass=True, two_subpass=True,
                                   edit=move_after_result))


if __name__ == "__main__":
    unittest.main()
