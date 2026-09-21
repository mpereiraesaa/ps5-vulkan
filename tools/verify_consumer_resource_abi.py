"""Verify the public SDK consumer's compute and graphics resource witnesses.

This validates one bounded resource-ABI workload and its telemetry integrity.
It is not Vulkan conformance and does not infer support beyond the exact
descriptor types, formats and shader exercised by the consumer.
The optional 96-sampler diagnostic exceeds the still-published sampler limits;
its success is backend qualification, not validation of a portable application.
"""
import argparse
import hashlib
import json
from pathlib import Path


TITLE = "PPSA99994"
APP = "ps5vk"

# Shader draw parameters as the promoted contract defines them: BaseVertex,
# BaseInstance and DrawIndex = 0 for direct and single-indirect draws. The
# triples are the low bytes the witness shader encoded into colour, so the case
# with a negative vertex offset pins the two's complement value 254 and every
# case pins DrawIndex 0. Indirect firstInstance stays 0 because
# drawIndirectFirstInstance is not part of this profile.
DRAW_PARAMETER_CASES = (
    ("list_direct", 7, 9, 0),
    ("list_indexed", 5, 3, 0),
    ("strip_indexed_negative", 254, 11, 0),
    ("list_indirect", 11, 0, 0),
    ("strip_indexed_indirect", 17, 0, 0),
    ("strip_direct", 21, 23, 0),
)
DRAW_PARAMETER_COVERED_MINIMUM = 900
# The destination-role witness writes these exact words into the same
# colour-attachment image the draw cases then use: a clear fills everything and
# a buffer-to-image upload overwrites the leading edge. Both words and their
# pixel counts are pins, not samples.
DRAW_PARAMETER_DST_CLEAR_WORD = 0xff604020
DRAW_PARAMETER_DST_UPLOAD_WORD = 0xff1e140a
DRAW_PARAMETER_EXTENT = 64
DRAW_PARAMETER_UPLOAD_EDGE = 8

# Indirect and indexed draw witness (DXVK262-T03). The target is a grid of
# cells; every draw or instance places a triangle in the cell it selects and
# encodes the delivered built-ins (or its 16-bit cell index) in the colour, so
# each case pins how many cells must carry their exact word (matched), that no
# expected cell is wrong and that no other cell holds any coverage (stray).
# (name, cells per row, pinned cells)
INDIRECT_CASES = (
    ("indirect_first_instance", 4, 3),
    ("indexed_indirect_first_instance", 4, 2),
    ("multi_draw", 4, 3),
    ("multi_draw_indexed", 4, 3),
    ("compute_generated_arguments", 4, 4),
    ("max_draw_indirect_count", 256, 65535),
    ("uint32_bit31_indices", 4, 4),
    ("uint32_2pow24_indices", 4, 4),
    ("uint16_control_indices", 4, 4),
    ("uint32_bit31_indexed_indirect", 4, 4),
)
INDIRECT_EXTENT = 256
INDIRECT_CLEAR_WORD = 0xff404040
INDIRECT_MAX_COMMANDS = 65535
# The driver's own expansion records for the four multi-command cases, in
# case order: (commands, commands that draw, emitted draws). The 65535-command
# case must additionally have needed more than one command arena.
INDIRECT_EXPANSIONS = (
    ("multi_draw", 4, 3, 3),
    ("multi_draw_indexed", 4, 3, 3),
    ("compute_generated_arguments", 4, 4, 4),
    ("max_draw_indirect_count", INDIRECT_MAX_COMMANDS, INDIRECT_MAX_COMMANDS, INDIRECT_MAX_COMMANDS),
)
# The rasterization-state witness (DXVK262-T05): case name and the number of
# draws its frame records, in the order the consumer runs them
# (examples/native_consumer/raster_state.h). Every case is a hand-derived
# oracle; the consumer reports valid=1 only when the counted pixels match it.
RASTER_CASES = (
    ("bias_disabled_coplanar", 2),
    ("bias_constant_negative", 2),
    ("bias_constant_positive", 2),
    ("bias_dynamic_constant_negative", 2),
    ("bias_disabled_tilted", 2),
    ("bias_slope_negative", 2),
    ("bias_slope_negative_clamp_negative", 2),
    ("bias_slope_negative_clamp_positive", 2),
    ("bias_slope_positive", 2),
    ("bias_slope_positive_clamp_positive", 2),
    ("bias_dynamic_slope_clamp_negative", 2),
    ("bias_dynamic_two_draws", 3),
    ("clamp_disabled_control", 1),
    ("clamp_enabled", 1),
    ("clamp_enabled_w2", 1),
    ("clamp_disabled_narrow_probe", 2),
    ("clamp_enabled_narrow_probe", 2),
    ("clamp_enabled_reversed_probe", 2),
    ("clamp_disabled_near_probe", 2),
    ("clamp_enabled_near_probe", 2),
    ("polygon_fill", 1),
    ("polygon_line", 1),
    ("polygon_point", 1),
    ("polygon_line_cull_front", 1),
    ("polygon_point_cull_back_front", 1),
    ("polygon_line_cull_back", 1),
    ("polygon_line_coplanar_unbiased", 2),
    ("polygon_line_coplanar_biased", 2),
    ("viewport_static_bank0_of_two", 1),
    ("viewport_dynamic_partial_updates", 2),
    ("viewport_static_scissor_bank0_of_two", 1),
)
RASTER_EXTENT = 64
RASTER_CLEAR_WORD = 0xff000000
RASTER_FLOOR_WORD = 0xff604020
RASTER_TEST_WORD = 0xff9010e0
RASTER_PROBE_WORD = 0xff40ff40

# Exact hashes of the two 64-byte destination buffers of the executable
# secondary scenario, established by two identical hardware runs of the same
# deployed artifact. Only the first 32 bytes of the named buffer are filled;
# its remaining 32 bytes and the whole control buffer of the secondary the
# primary never named stay guard bytes.
SECONDARY_EXECUTED_HASH = "ca327245"
SECONDARY_CONTROL_HASH = "21a49bc5"

# The in-pass oracle renders the same triangle twice into the same 1920x1080
# B8G8R8A8 attachment shape, once through an inherited continuation secondary
# and once inline, so ONE pinned hash and ONE pinned changed-pixel count stand
# for both readbacks: they must agree with each other and with these values.
INPASS_HASH = "77abc830"
INPASS_CHANGED = 471744

# Exact full-image hashes for the shared-color two-subpass oracle. The native
# result must equal the ordered one-subpass control and differ from every
# negative. Reversed equals first-only because its final full-size draw covers
# the smaller draw completely; that equality is expected, not relaxed.
TWO_SUBPASS_HASH = "84cc0cb3"
TWO_SUBPASS_FIRST_HASH = "77abc830"
TWO_SUBPASS_SECOND_HASH = "fa6b3a7a"
TWO_SUBPASS_REVERSED_HASH = "77abc830"
TWO_SUBPASS_CHANGED = 471744

TEXEL_FORMAT_CASES = [
    ("r8_unorm","float",1,"3f800000,00000000,00000000,3f800000"),
    ("r8_snorm","float",1,"bf800000,00000000,00000000,3f800000"),
    ("r8g8_unorm","float",2,"3f800000,3f800000,00000000,3f800000"),
    ("r8g8_snorm","float",2,"bf800000,3f800000,00000000,3f800000"),
    ("r8g8b8a8_unorm","float",4,"00000000,3f800000,00000000,3f800000"),
    ("r8g8b8a8_snorm","float",4,"bf800000,3f800000,bf800000,3f800000"),
    ("a8b8g8r8_unorm","float",4,"00000000,3f800000,00000000,3f800000"),
    ("a8b8g8r8_snorm","float",4,"bf800000,3f800000,bf800000,3f800000"),
    ("b10g11r11_ufloat","float",4,"3f800000,40000000,40800000,3f800000"),
    ("r16_unorm","float",2,"3f800000,00000000,00000000,3f800000"),
    ("r16_snorm","float",2,"bf800000,00000000,00000000,3f800000"),
    ("r16_sfloat","float",2,"3f800000,00000000,00000000,3f800000"),
    ("r16g16_unorm","float",4,"00000000,3f800000,00000000,3f800000"),
    ("r16g16_snorm","float",4,"bf800000,3f800000,00000000,3f800000"),
    ("r16g16_sfloat","float",4,"3f000000,c0000000,00000000,3f800000"),
    ("r16g16b16a16_unorm","float",8,"00000000,3f800000,00000000,3f800000"),
    ("r16g16b16a16_snorm","float",8,"bf800000,3f800000,bf800000,3f800000"),
    ("r16g16b16a16_sfloat","float",8,"3f000000,3f800000,40000000,c0000000"),
    ("r32_sfloat","float",4,"3f000000,00000000,00000000,3f800000"),
    ("r32g32_sfloat","float",8,"3f000000,c0000000,00000000,3f800000"),
    ("r32g32b32a32_sfloat","float",16,"3f000000,3f800000,40000000,c0000000"),
    ("r8_uint","uint",1,"000000ab,00000000,00000000,00000001"),
    ("r8_sint","sint",1,"ffffff81,00000000,00000000,00000001"),
    ("r8g8_uint","uint",2,"00000012,00000034,00000000,00000001"),
    ("r8g8_sint","sint",2,"ffffff81,0000007f,00000000,00000001"),
    ("r8g8b8a8_uint","uint",4,"00000012,00000034,00000056,00000078"),
    ("r8g8b8a8_sint","sint",4,"ffffffff,ffffff81,00000001,0000007f"),
    ("a8b8g8r8_uint","uint",4,"00000012,00000034,00000056,00000078"),
    ("a8b8g8r8_sint","sint",4,"ffffffff,ffffff81,00000001,0000007f"),
    ("r16_uint","uint",2,"00001234,00000000,00000000,00000001"),
    ("r16_sint","sint",2,"ffff8001,00000000,00000000,00000001"),
    ("r16g16_uint","uint",4,"00001234,00005678,00000000,00000001"),
    ("r16g16_sint","sint",4,"ffff8001,00007fff,00000000,00000001"),
    ("r16g16b16a16_uint","uint",8,"00001234,00005678,00009abc,0000def0"),
    ("r16g16b16a16_sint","sint",8,"ffff8001,00007fff,fffffffe,00000002"),
    ("r32_uint","uint",4,"12345678,00000000,00000000,00000001"),
    ("r32_sint","sint",4,"81234567,00000000,00000000,00000001"),
    ("r32g32_uint","uint",8,"12345678,9abcdef0,00000000,00000001"),
    ("r32g32_sint","sint",8,"81234567,12345678,00000000,00000001"),
    ("r32g32b32a32_uint","uint",16,"00000001,00000002,00000003,00000004"),
    ("r32g32b32a32_sint","sint",16,"ffffffff,00000002,fffffffd,00000004"),
]


def validate(log, receipt, artifact, texel_rgba8=False, texel_formats=False):
    def require(condition, label):
        if not condition:
            raise ValueError(label)

    digest = artifact.get("files", {}).get("eboot.bin", "")
    require(artifact.get("title") == TITLE and
            artifact.get("profile") == "public-consumer-resource-abi" and
            artifact.get("submit_enabled") is True, "artifact profile")
    require(not (texel_rgba8 and texel_formats), "one texel witness profile")
    require(len(digest) == 64 and
            all(c in "0123456789abcdef" for c in digest.lower()),
            "artifact identity")
    require(artifact.get("buffer_transfer") == {
        "api": "Vulkan 1.0", "copy_bytes": 7, "update_bytes": 8,
        "fill_bytes": 20, "whole_tail_bytes": 3,
    }, "buffer-transfer artifact contract")
    require(artifact.get("indirect_dispatch") == {
        "api": "Vulkan 1.0", "groups": [1, 1, 1], "offset": 512,
        "result_elements": 64,
    }, "indirect-dispatch artifact contract")
    require(artifact.get("dynamic_descriptors") == {
        "storage_buffers": 2, "uniform_buffers": 1,
        "offsets": [256, 256, 256], "base_plus_dynamic": True,
        "result_elements": 64, "guard_words": 192,
    }, "dynamic-descriptor artifact contract")
    width_artifact = artifact.get("storage_width", {})
    require(width_artifact.get("storageBuffer8BitAccess") is True and
            width_artifact.get("storageBuffer16BitAccess") is True and
            width_artifact.get("shaderInt8") is False and
            width_artifact.get("shaderInt16") is False and
            all(len(width_artifact.get(key, "")) == 64 for key in
                ("storage8_spirv_sha256", "storage16_spirv_sha256")),
            "storage-width artifact contract")
    sync_artifact = artifact.get("synchronization", {})
    require(sync_artifact.get("api") == "Vulkan 1.0" and
            sync_artifact.get("local_size") == 128 and
            sync_artifact.get("wave_size") == 32 and
            sync_artifact.get("binary_semaphore") is True and
            sync_artifact.get("host_event") is True and
            sync_artifact.get("device_event") is True and
            all(len(sync_artifact.get(key, "")) == 64 for key in
                ("sync_producer_spirv_sha256", "sync_consumer_spirv_sha256",
                 "shared_atomic_multiwave_spirv_sha256")),
            "synchronization artifact contract")
    fixed = artifact.get("fixed_function", {})
    require(fixed == {
        "api": "Vulkan 1.0", "width": 1920, "height": 1080,
        "frames": 18, "color_format": "VK_FORMAT_B8G8R8A8_UNORM",
        "depth_format": "VK_FORMAT_D32_SFLOAT", "samples": 1,
        "load_preservation": True, "dynamic_viewport": True,
        "dynamic_scissor": True,
    }, "fixed-function artifact contract")
    sampled = artifact.get("sampled_graphics")
    sampled_profile = None
    if sampled is not None:
        # Archived fragment-only artifacts predate stage_profile. No shared-stage
        # evidence can use that fallback: both its vertex digest and start row differ.
        sampled_profile = sampled.get("stage_profile", "fragment")
        require(sampled_profile in ("fragment", "vertex-fragment", "single-set",
                                    "mixed-resources"),
                "sampled stage profile")
        require(sampled.get("sets") in (1, 4) and sampled.get("descriptors") == 96 and
                sampled.get("rounds") == 4 and
                len(sampled.get("shader_spirv_sha256", "")) == 64 and
                all(c in "0123456789abcdef" for c in sampled["shader_spirv_sha256"]),
                "sampled-graphics artifact contract")
        if sampled_profile == "single-set":
            require(sampled.get("sets") == 1 and sampled.get("elements_per_set") == 96 and
                    sampled.get("descriptors") == 96 and
                    type(sampled.get("visibility_mask")) is int and
                    sampled["visibility_mask"] == 0x10,
                    "single-set artifact contract")
        elif sampled_profile == "mixed-resources":
            require(sampled.get("sets") == 4 and sampled.get("uniform_buffers") == 4 and
                    type(sampled.get("visibility_mask")) is int and
                    sampled["visibility_mask"] == 0x10,
                    "mixed-resource artifact contract")
        elif sampled_profile == "vertex-fragment":
            require("uniform_buffers" not in sampled,
                    "sampler-only artifact must not claim uniform buffers")
            if "visibility_mask" in sampled:
                require(type(sampled["visibility_mask"]) is int and
                        sampled["visibility_mask"] in (0x11, 0x1f, 0x7fffffff), "sampled visibility mask")
            vertex_digest = sampled.get("vertex_spirv_sha256", "")
            require(isinstance(vertex_digest, str) and len(vertex_digest) == 64 and
                    all(c in "0123456789abcdef" for c in vertex_digest), "sampled vertex digest")
        else:
            require("vertex_spirv_sha256" not in sampled, "vertex digest in fragment-only artifact")
            require("visibility_mask" not in sampled, "shared visibility in fragment-only artifact")
            require("uniform_buffers" not in sampled,
                    "sampler-only artifact must not claim uniform buffers")
    require(hashlib.sha256(log).hexdigest() == receipt.get("sha256"),
            "log hash")
    require(receipt.get("protocol") == "ps5log/1" and
            receipt.get("transport") == "tcp" and
            receipt.get("clean") is True and receipt.get("bye") is True and
            receipt.get("gaps") == [] and receipt.get("raw_lines") == 0,
            "complete TCP receipt")

    lines = log.decode().splitlines()
    require(len(lines) > 2 and lines[0].startswith("HELLO ps5log/1 "), "hello")
    identity = dict(item.split("=", 1) for item in lines[0].split()[2:])
    manifest_identity = receipt.get("identity", {})
    require(identity.get("title") == TITLE and identity.get("app") == APP and
            all(identity.get(key) == manifest_identity.get(key)
                for key in ("title", "app", "boot")), "stream identity")

    messages = []
    previous_time = -1
    for seq, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4 and fields[0] == str(seq), "sequence")
        timestamp = int(fields[1])
        require(timestamp >= previous_time, "clock ordering")
        previous_time = timestamp
        require(fields[2] in ("MARK", "INFO") and
                "CHECK failed" not in fields[3] and
                not fields[3].startswith("Compute mismatch"), "runtime failure")
        messages.append(fields[3])

    require(receipt.get("last_seq") == len(messages), "manifest sequence")
    require(lines[-1] ==
            f"BYE seq={len(messages)} reason=consumer-finite-end", "complete BYE")

    def matching(prefix):
        return [(index, message) for index, message in enumerate(messages)
                if message.startswith(prefix)]

    def one(prefix):
        found = matching(prefix)
        require(len(found) == 1, prefix)
        return found[0]

    boot = one("PS5VK_CONSUMER_BOOT ")
    physical = one("PS5VK_CONSUMER_PHYSICAL_DEVICE ")
    physical_queries = one("PS5VK_CONSUMER_PHYSICAL_QUERIES ")
    negotiated = one("PS5VK_CONSUMER_STORAGE_WIDTH_NEGOTIATED ")
    transfer_start = one("PS5VK_CONSUMER_BUFFER_TRANSFER_START")
    transfer_witness = one("PS5VK_CONSUMER_BUFFER_TRANSFER_SUCCESS ")
    # Executable secondary command buffers: the named secondary must have run
    # and the unnamed control must be untouched. Both halves are required, so a
    # run where nothing executed and a run where something unnamed executed are
    # equally failures.
    # Presence-gated rather than flag-gated, because this scenario is compiled
    # unconditionally into the consumer and its START marker is emitted before
    # any Vulkan work: a payload that contains the scenario always logs START,
    # so a missing START means an older payload, not a silent skip. Once START
    # is present the rest is mandatory, so a run that starts the scenario and
    # fails to finish it cannot pass.
    secondary_present = bool(matching("PS5VK_CONSUMER_SECONDARY_EXECUTE_START"))
    secondary_start = one("PS5VK_CONSUMER_SECONDARY_EXECUTE_START") if secondary_present else None
    secondary_witness = one("PS5VK_CONSUMER_SECONDARY_EXECUTE_SUCCESS ") if secondary_present else None
    secondary_retired = one("PS5VK_CONSUMER_SECONDARY_EXECUTE_RETIRED") if secondary_present else None
    if secondary_present:
        secondary_fields = secondary_witness[1].split()[1:]
        for expected in ("filled_bytes=32", "guard_bytes=32", "executed_mismatches=0",
                         "control_mismatches=0", "control_untouched=1"):
            require(expected in secondary_fields,
                    f"secondary execute witness missing {expected}")
        # Both hashes are pinned to their exact measured values rather than
        # merely required to differ. Identical hashes would mean the executed
        # and control buffers ended up the same, which is what a driver that
        # silently ran nothing, or ran the unnamed one too, would produce - but
        # so would a wrong-but-different result, and only the exact pair rules
        # that out as well. The deterministic fill makes both values stable
        # across runs of the same artifact, so this is a pin, not a sample.
        for expected in (f"executed_hash={SECONDARY_EXECUTED_HASH}",
                         f"control_hash={SECONDARY_CONTROL_HASH}"):
            require(expected in secondary_fields,
                    f"secondary execute witness missing {expected}")
    # Secondary execution INSIDE a render pass. The witness is the EQUALITY of
    # two readbacks of the same triangle - one reached through an inherited
    # continuation secondary, one recorded inline - so the discriminator fails
    # in every wrong direction: nothing executed, the deliberately unnamed
    # secondary executed as well, or either drew something else. Presence-gated
    # for the same reason as the transfer scenario above: the START marker is
    # compiled in unconditionally, so a log without it is an older payload
    # rather than a silent skip, and once it is present the rest is mandatory.
    inpass_present = bool(matching("PS5VK_CONSUMER_INPASS_SECONDARY_START"))
    inpass_start = one("PS5VK_CONSUMER_INPASS_SECONDARY_START") if inpass_present else None
    inpass_witness = one("PS5VK_CONSUMER_INPASS_SECONDARY_SUCCESS ") if inpass_present else None
    inpass_retired = one("PS5VK_CONSUMER_INPASS_SECONDARY_RETIRED") if inpass_present else None
    if inpass_present:
        inpass_fields = inpass_witness[1].split()[1:]
        for expected in ("named=1", "unnamed_recorded=1", "bad_alpha=0", "bad_sum=0",
                         f"executed_changed={INPASS_CHANGED}",
                         f"control_changed={INPASS_CHANGED}",
                         f"executed_hash={INPASS_HASH}",
                         f"control_hash={INPASS_HASH}"):
            require(expected in inpass_fields,
                    f"in-pass secondary witness missing {expected}")
        # Stated as its own requirement rather than left implicit in the two
        # pins: the secondary-executed and inline-recorded results must be the
        # SAME image, and the pinned value is what that image is.
        executed = [field for field in inpass_fields if field.startswith("executed_hash=")]
        control = [field for field in inpass_fields if field.startswith("control_hash=")]
        require(len(executed) == 1 and len(control) == 1 and
                executed[0].split("=")[1] == control[0].split("=")[1],
                "secondary-executed and inline draws produced different images")
    # Shader draw parameters. The witness encodes BaseVertex, BaseInstance and
    # DrawIndex into colour and compares the readback in CPU, so every expected
    # triple is a pin rather than a sample. Presence-gated for the same reason
    # as the other scenarios: START is compiled in unconditionally, so a log
    # without it is an older payload rather than a silent skip.
    draw_parameters_present = bool(matching("PS5VK_CONSUMER_DRAW_PARAMETERS_START"))
    draw_parameters_start = one("PS5VK_CONSUMER_DRAW_PARAMETERS_START") \
        if draw_parameters_present else None
    draw_parameter_messages = matching("PS5VK_CONSUMER_DRAW_PARAMETERS case=")
    draw_parameters_result = one("PS5VK_CONSUMER_DRAW_PARAMETERS_RESULT ") \
        if draw_parameters_present else None
    draw_parameters_retired = one("PS5VK_CONSUMER_DRAW_PARAMETERS_RETIRED") \
        if draw_parameters_present else None
    if draw_parameters_present:
        expected_cases = {case[0]: case[1:] for case in DRAW_PARAMETER_CASES}
        destination = one("PS5VK_CONSUMER_DRAW_PARAMETERS_DST ")
        destination_fields = dict(field.split("=", 1)
                                  for field in destination[1].split()[1:])
        require(destination_fields.get("clear_word") ==
                f"{DRAW_PARAMETER_DST_CLEAR_WORD:08x}" and
                destination_fields.get("upload_word") ==
                f"{DRAW_PARAMETER_DST_UPLOAD_WORD:08x}",
                "draw-parameter destination words")
        require(destination_fields.get("clear_matched") ==
                str(DRAW_PARAMETER_EXTENT * DRAW_PARAMETER_EXTENT -
                    DRAW_PARAMETER_UPLOAD_EDGE * DRAW_PARAMETER_UPLOAD_EDGE) and
                destination_fields.get("upload_matched") ==
                str(DRAW_PARAMETER_UPLOAD_EDGE * DRAW_PARAMETER_UPLOAD_EDGE) and
                destination_fields.get("valid") == "1",
                "draw-parameter destination readback")
        manifest = artifact.get("draw_parameters", {})
        require(manifest.get("cases") == [case[0] for case in DRAW_PARAMETER_CASES],
                "artifact draw-parameter case list")
        for field in ("vertex_shader_sha256", "fragment_shader_sha256"):
            digest = manifest.get(field, "")
            require(len(digest) == 64 and
                    all(char in "0123456789abcdef" for char in digest),
                    f"artifact draw-parameter {field}")
        require(len(draw_parameter_messages) == len(DRAW_PARAMETER_CASES),
                "draw-parameter case count")
        observed = {}
        for _, message in draw_parameter_messages:
            fields = dict(field.split("=", 1) for field in message.split()[1:])
            name = fields.get("case", "")
            require(name in expected_cases, f"unexpected draw-parameter case {name!r}")
            require(name not in observed, f"repeated draw-parameter case {name}")
            values = expected_cases[name]
            require(fields.get("base_vertex") == str(values[0]) and
                    fields.get("base_instance") == str(values[1]) and
                    fields.get("draw_index") == str(values[2]),
                    f"draw-parameter values for {name}")
            require(fields.get("uniform") == "1" and fields.get("valid") == "1",
                    f"draw-parameter uniformity for {name}")
            require(fields.get("covered", "").isdigit() and
                    int(fields["covered"]) >= DRAW_PARAMETER_COVERED_MINIMUM,
                    f"draw-parameter coverage for {name}")
            observed[name] = fields
        require(set(observed) == set(expected_cases), "draw-parameter case set")
        require(draw_parameters_result[1].split()[1:] == [
            f"cases={len(DRAW_PARAMETER_CASES)}",
            f"witnessed={len(DRAW_PARAMETER_CASES)}", "valid=1"],
            "draw-parameter result")
        # The pinned upstream readback contract: the same frame copied into the
        # linear staging image and read through vkGetImageSubresourceLayout. The
        # words below come from the staging memory, not the attachment's, so a
        # driver that cannot describe or fill that image cannot pass this.
        staging_messages = matching("PS5VK_CONSUMER_DRAW_PARAMETERS_STAGING case=")
        staging_result = one("PS5VK_CONSUMER_DRAW_PARAMETERS_STAGING_RESULT ")
        require(len(staging_messages) == len(DRAW_PARAMETER_CASES),
                "staging readback case count")
        expected_pitch = (DRAW_PARAMETER_EXTENT * 4 + 255) & ~255
        staged = {}
        for _, message in staging_messages:
            fields = dict(field.split("=", 1) for field in message.split()[1:])
            name = fields.get("case", "")
            require(name in expected_cases,
                    f"unexpected staging readback case {name!r}")
            require(name not in staged, f"repeated staging readback case {name}")
            require(fields.get("row_pitch") == str(expected_pitch) and
                    fields.get("staged_bytes") ==
                    str(expected_pitch * DRAW_PARAMETER_EXTENT),
                    f"staging layout for {name}")
            require(fields.get("layout") == "1",
                    f"staging subresource layout for {name}")
            observed_case = observed[name]
            require(fields.get("covered") == observed_case.get("covered") and
                    fields.get("uniform") == "1" and fields.get("valid") == "1",
                    f"staging readback for {name}")
            staged[name] = fields
        require(set(staged) == set(expected_cases), "staging readback case set")
        # The staging word must be exactly the frame the attachment held: the
        # same encoded triple, read from the linear image.
        for name, fields in staged.items():
            encoded = (0xff << 24) | (int(expected_cases[name][2]) << 16) | \
                (int(expected_cases[name][1]) << 8) | int(expected_cases[name][0])
            require(fields.get("staged") == f"{encoded:08x}",
                    f"staging word for {name}")
        require(staging_result[1].split()[1:] == [
            f"cases={len(DRAW_PARAMETER_CASES)}",
            f"witnessed={len(DRAW_PARAMETER_CASES)}", "valid=1"],
            "staging readback result")
    # Indirect and indexed draw witness. Presence-gated like the other
    # scenarios; once START is present every case, the driver's own expansion
    # records and the negotiation line are mandatory.
    indirect_present = bool(matching("PS5VK_CONSUMER_INDIRECT_START "))
    indirect_start = one("PS5VK_CONSUMER_INDIRECT_START ") if indirect_present else None
    indirect_result = one("PS5VK_CONSUMER_INDIRECT_RESULT ") if indirect_present else None
    indirect_retired = one("PS5VK_CONSUMER_INDIRECT_RETIRED ") if indirect_present else None
    indirect_compute_dispatches = 0
    if indirect_present:
        indirect_negotiated = one("PS5VK_CONSUMER_INDIRECT_NEGOTIATED ")
        require(indirect_negotiated[1].split()[1:] == [
            "multiDrawIndirect=1", "drawIndirectFirstInstance=1",
            "fullDrawIndexUint32=1", "enabled=core_features2"],
            "indirect core feature negotiation")
        indirect_features = one("PS5VK_CONSUMER_INDIRECT_FEATURES ")
        require(indirect_features[1].split()[1:] == [
            "multiDrawIndirect=1", "drawIndirectFirstInstance=1", "fullDrawIndexUint32=1",
            f"maxDrawIndirectCount={INDIRECT_MAX_COMMANDS}",
            "maxDrawIndexedIndexValue=4294967295"],
            "indirect feature and limit report")
        require(indirect_start[1].split()[1:] == [
            f"cases={len(INDIRECT_CASES)}", f"extent={INDIRECT_EXTENT}",
            f"clear_word={INDIRECT_CLEAR_WORD:08x}"], "indirect witness start")
        require(one("PS5VK_CONSUMER_INDIRECT_PIPELINE ")[1].split()[1:] == [
            "topology=triangle_list", "push_bytes=16", "created=1"], "indirect witness pipeline")
        require(one("PS5VK_CONSUMER_INDIRECT_TARGET ")[1].split()[1:] == [
            "layout=general", f"clear_word={INDIRECT_CLEAR_WORD:08x}", "prelude=1"],
            "indirect witness target prelude")
        require(one("PS5VK_CONSUMER_INDIRECT_ARGUMENTS_GENERATED ")[1].split()[1:] == [
            "commands=4", "vertices=3", "first_instance_base=20",
            "barrier=compute_shader_write_to_indirect_command_read"],
            "compute-generated argument dispatch")
        require(one("PS5VK_CONSUMER_INDIRECT_ARGUMENTS_READBACK ")[1].split()[1:] == [
            "first=3,1,0,20", "last=3,1,0,23"], "compute-generated argument readback")
        indirect_manifest = artifact.get("indirect_draws", {})
        require(indirect_manifest.get("cases") == [case[0] for case in INDIRECT_CASES] and
                indirect_manifest.get("extent") == INDIRECT_EXTENT and
                indirect_manifest.get("max_commands") == INDIRECT_MAX_COMMANDS and
                indirect_manifest.get("features") == [
                    "drawIndirectFirstInstance", "fullDrawIndexUint32", "multiDrawIndirect"],
                "artifact indirect witness manifest")
        for field in ("vertex_shader_sha256", "compute_shader_sha256", "fragment_shader_sha256"):
            digest = indirect_manifest.get(field, "")
            require(len(digest) == 64 and
                    all(char in "0123456789abcdef" for char in digest),
                    f"artifact indirect {field}")
        indirect_messages = matching("PS5VK_CONSUMER_INDIRECT case=")
        require(len(indirect_messages) == len(INDIRECT_CASES), "indirect case count")
        expected_cells = {case[0]: (case[1], case[2]) for case in INDIRECT_CASES}
        observed_indirect = []
        for _, message in indirect_messages:
            fields = dict(field.split("=", 1) for field in message.split()[1:])
            name = fields.get("case", "")
            require(name in expected_cells, f"unexpected indirect case {name!r}")
            require(name not in observed_indirect, f"repeated indirect case {name}")
            cells, pinned = expected_cells[name]
            require(fields.get("cells") == str(cells) and
                    fields.get("expected") == str(pinned) and
                    fields.get("matched") == str(pinned) and
                    fields.get("wrong") == "0" and fields.get("stray") == "0" and
                    fields.get("valid") == "1",
                    f"indirect cells for {name}")
            require(fields.get("covered", "").isdigit() and
                    int(fields["covered"]) >= pinned,
                    f"indirect coverage for {name}")
            observed_indirect.append(name)
        require(observed_indirect == [case[0] for case in INDIRECT_CASES],
                "indirect case order")
        # The driver's expansion records between START and RESULT, in case
        # order: exactly the four multi-command cases, each with its command
        # count, the commands that drew and the draws emitted; the 65535-command
        # case needed a chain of arenas and reported it.
        expansions = [(index, message) for index, message in
                      matching("PS5VK_MULTI_DRAW_EXPANDED ")
                      if indirect_start[0] < index < indirect_result[0]]
        require(len(expansions) == len(INDIRECT_EXPANSIONS), "multi-draw expansion records")
        arenas_for_max = 0
        for (_, message), (name, commands, drawing, draws) in zip(expansions, INDIRECT_EXPANSIONS):
            fields = dict(field.split("=", 1) for field in message.split()[1:])
            require(fields.get("body") == "0" and fields.get("commands") == str(commands) and
                    fields.get("drawing") == str(drawing) and fields.get("draws") == str(draws) and
                    fields.get("arenas", "").isdigit() and int(fields["arenas"]) >= 1,
                    f"multi-draw expansion for {name}")
            if name == "max_draw_indirect_count":
                arenas_for_max = int(fields["arenas"])
        batches = [(index, message) for index, message in matching("PS5VK_GRAPHICS_BATCHES ")
                   if indirect_start[0] < index < indirect_result[0]]
        require(len(batches) == 1 and arenas_for_max > 1, "arena chain for the 65535-command case")
        batch_fields = dict(field.split("=", 1) for field in batches[0][1].split()[1:])
        require(batch_fields.get("arenas") == str(arenas_for_max) and
                batch_fields.get("words", "").isdigit() and
                int(batch_fields["words"]) > (arenas_for_max - 1) * 16384,
                "arena chain record")
        batch_submits = [(index, message) for index, message in matching("PS5VK_GRAPHICS_BATCH_SUBMIT ")
                         if indirect_start[0] < index < indirect_result[0]]
        batch_completions = [(index, message) for index, message in
                             matching("PS5VK_GRAPHICS_BATCH_COMPLETED ")
                             if indirect_start[0] < index < indirect_result[0]]
        require(len(batch_submits) == arenas_for_max - 1 and
                len(batch_completions) == arenas_for_max - 1 and
                all(message.endswith("rc=0") for _, message in batch_submits),
                "arena chain launches and completions")
        require(indirect_result[1].split()[1:] == [
            f"cases={len(INDIRECT_CASES)}", f"witnessed={len(INDIRECT_CASES)}", "valid=1"],
            "indirect witness result")
        require(indirect_retired[1].split()[1:] == [
            f"cases={len(INDIRECT_CASES)}", f"witnessed={len(INDIRECT_CASES)}"],
            "indirect witness retirement")
        require(indirect_start[0] < indirect_messages[0][0] < indirect_result[0] < indirect_retired[0],
                "indirect witness ordering")
        indirect_compute_dispatches = 1
    # Rasterization-state witness (DXVK262-T05). Presence-gated on its START
    # marker like the other scenarios: a device that does not report the four
    # features logs the feature line and a SKIPPED line and nothing else; once
    # START is present every case, in order, must be valid, and the feature
    # line must show all four features with the multiViewport floor.
    raster_features = matching("PS5VK_CONSUMER_RASTER_FEATURES ")
    raster_present = bool(matching("PS5VK_CONSUMER_RASTER_START "))
    raster_start = one("PS5VK_CONSUMER_RASTER_START ") if raster_present else None
    raster_result = one("PS5VK_CONSUMER_RASTER_RESULT ") if raster_present else None
    raster_retired = one("PS5VK_CONSUMER_RASTER_RETIRED ") if raster_present else None
    if raster_features and not raster_present:
        require(len(raster_features) == 1 and
                one("PS5VK_CONSUMER_RASTER_SKIPPED ")[1].endswith("reason=features_not_reported"),
                "raster witness skipped only when the features are not reported")
    if raster_present:
        require(len(raster_features) == 1, "raster feature report")
        feature_fields = dict(item.split("=", 1) for item in raster_features[0][1].split()[1:])
        require(feature_fields.get("depthBiasClamp") == "1" and
                feature_fields.get("depthClamp") == "1" and
                feature_fields.get("fillModeNonSolid") == "1" and
                feature_fields.get("multiViewport") == "1" and
                feature_fields.get("maxViewports", "").isdigit() and
                int(feature_fields["maxViewports"]) >= 16,
                "raster feature and viewport floor report")
        require(raster_start[1].split()[1:] == [
            f"cases={len(RASTER_CASES)}", f"extent={RASTER_EXTENT}",
            f"clear_word={RASTER_CLEAR_WORD:08x}", f"floor_word={RASTER_FLOOR_WORD:08x}",
            f"test_word={RASTER_TEST_WORD:08x}", f"probe_word={RASTER_PROBE_WORD:08x}"],
            "raster witness start")
        require(one("PS5VK_CONSUMER_RASTER_TARGET ")[1].split()[1:] == [
            "layout=general", f"clear_word={RASTER_CLEAR_WORD:08x}", "prelude=1"],
            "raster witness target prelude")
        raster_pipelines = matching("PS5VK_CONSUMER_RASTER_PIPELINE ")
        require([row[1].split()[1:] for row in raster_pipelines] == [
            ["role=floor", "depth=less_write", "created=1"],
            ["role=two_viewports", "created=1"]], "raster witness pipelines")
        raster_manifest = artifact.get("raster_state", {})
        require(raster_manifest.get("cases") == [case[0] for case in RASTER_CASES] and
                raster_manifest.get("extent") == RASTER_EXTENT and
                raster_manifest.get("features") == [
                    "depthBiasClamp", "depthClamp", "fillModeNonSolid", "multiViewport"],
                "artifact raster witness manifest")
        for field in ("vertex_shader_sha256", "fragment_shader_sha256"):
            digest = raster_manifest.get(field, "")
            require(len(digest) == 64 and
                    all(char in "0123456789abcdef" for char in digest),
                    f"artifact raster {field}")
        raster_messages = matching("PS5VK_CONSUMER_RASTER case=")
        require(len(raster_messages) == len(RASTER_CASES), "raster case count")
        observed_raster = []
        for _, message in raster_messages:
            fields = dict(field.split("=", 1) for field in message.split()[1:])
            name = fields.get("case", "")
            require(name in dict(RASTER_CASES), f"unexpected raster case {name!r}")
            require(name not in observed_raster, f"repeated raster case {name}")
            counts = [fields.get(key, "") for key in
                      ("floor", "test", "probe", "clear", "other", "test_left", "test_top")]
            require(all(value.isdigit() for value in counts) and
                    fields.get("other") == "0" and fields.get("valid") == "1" and
                    sum(int(value) for value in counts[:5]) == RASTER_EXTENT * RASTER_EXTENT and
                    fields.get("expected", ""),
                    f"raster oracle for {name}")
            observed_raster.append(name)
        require(observed_raster == [case[0] for case in RASTER_CASES], "raster case order")
        require(raster_result[1].split()[1:] == [
            f"cases={len(RASTER_CASES)}", f"witnessed={len(RASTER_CASES)}", "valid=1"],
            "raster witness result")
        require(raster_retired[1].split()[1:] == [
            f"cases={len(RASTER_CASES)}", f"witnessed={len(RASTER_CASES)}"],
            "raster witness retirement")
        require(raster_start[0] < raster_messages[0][0] < raster_result[0] < raster_retired[0],
                "raster witness ordering")
    # multiViewport end to end through a geometry stage (DXVK262-T05). Gated on
    # its own START marker: a device without geometryShader logs the feature
    # line and SKIPPED; once present the single routing case must be valid.
    raster_gs_features = matching("PS5VK_CONSUMER_RASTER_GS_FEATURES ")
    raster_gs_present = bool(matching("PS5VK_CONSUMER_RASTER_GS_START "))
    raster_gs_start = one("PS5VK_CONSUMER_RASTER_GS_START ") if raster_gs_present else None
    raster_gs_result = one("PS5VK_CONSUMER_RASTER_GS_RESULT ") if raster_gs_present else None
    raster_gs_retired = one("PS5VK_CONSUMER_RASTER_GS_RETIRED ") if raster_gs_present else None
    if raster_gs_features and not raster_gs_present:
        require(len(raster_gs_features) == 1 and
                one("PS5VK_CONSUMER_RASTER_GS_SKIPPED ")[1].endswith("reason=features_not_reported"),
                "viewport-index witness skipped only when the features are not reported")
    if raster_gs_present:
        require(len(raster_gs_features) == 1, "viewport-index feature report")
        gs_fields = dict(item.split("=", 1) for item in raster_gs_features[0][1].split()[1:])
        require(gs_fields.get("geometryShader") == "1" and gs_fields.get("multiViewport") == "1" and
                gs_fields.get("maxViewports", "").isdigit() and int(gs_fields["maxViewports"]) >= 16,
                "viewport-index feature and floor report")
        require(raster_gs_start[1].split()[1:] == [
            "cases=1", f"extent={RASTER_EXTENT}", "tiles=16", f"clear_word={RASTER_CLEAR_WORD:08x}"],
            "viewport-index witness start")
        require(one("PS5VK_CONSUMER_RASTER_GS_TARGET ")[1].split()[1:] == [
            "layout=general", f"clear_word={RASTER_CLEAR_WORD:08x}", "prelude=1"],
            "viewport-index witness target prelude")
        require(one("PS5VK_CONSUMER_RASTER_GS_PIPELINE ")[1].split()[1:] == [
            "stages=3", "viewports=16", "created=1"], "viewport-index witness pipeline")
        gs_case = one("PS5VK_CONSUMER_RASTER_GS case=")
        require(gs_case[1].split()[1:] == [
            "case=viewport_index_routing", "tiles=16", f"matched={RASTER_EXTENT * RASTER_EXTENT}",
            "foreign=0", "clear=0", "other=0", "valid=1"], "viewport-index routing oracle")
        require(raster_gs_result[1].split()[1:] == ["cases=1", "witnessed=1", "valid=1"],
                "viewport-index witness result")
        require(raster_gs_retired[1].split()[1:] == ["cases=1", "witnessed=1"],
                "viewport-index witness retirement")
        require(raster_gs_start[0] < gs_case[0] < raster_gs_result[0] < raster_gs_retired[0],
                "viewport-index witness ordering")
        gs_manifest = artifact.get("raster_viewport_index", {})
        require(gs_manifest.get("cases") == ["viewport_index_routing"] and
                gs_manifest.get("extent") == RASTER_EXTENT and gs_manifest.get("tiles") == 16 and
                gs_manifest.get("features") == ["geometryShader", "multiViewport"],
                "artifact viewport-index witness manifest")
        for field in ("vertex_shader_sha256", "geometry_shader_sha256", "fragment_shader_sha256"):
            digest = gs_manifest.get(field, "")
            require(len(digest) == 64 and all(char in "0123456789abcdef" for char in digest),
                    f"artifact viewport-index {field}")
    two_subpass_present = bool(matching("PS5VK_CONSUMER_TWO_SUBPASS_START"))
    two_subpass_start = one("PS5VK_CONSUMER_TWO_SUBPASS_START") \
        if two_subpass_present else None
    two_subpass_witness = one("PS5VK_CONSUMER_TWO_SUBPASS_SUCCESS ") \
        if two_subpass_present else None
    two_subpass_retired = one("PS5VK_CONSUMER_TWO_SUBPASS_RETIRED") \
        if two_subpass_present else None
    subpass_boundaries = matching("PS5VK_SUBPASS_BOUNDARY ")
    if two_subpass_present:
        expected = {
            "multi": TWO_SUBPASS_HASH,
            "ordered": TWO_SUBPASS_HASH,
            "first": TWO_SUBPASS_FIRST_HASH,
            "second": TWO_SUBPASS_SECOND_HASH,
            "reversed": TWO_SUBPASS_REVERSED_HASH,
            "changed": str(TWO_SUBPASS_CHANGED),
            "negative_distinct": "1",
            "bad_alpha": "0",
            "bad_sum": "0",
        }
        observed = dict(item.split("=", 1)
                        for item in two_subpass_witness[1].split()[1:])
        require(observed == expected, "two-subpass exact pixel oracle")
        require(observed["multi"] == observed["ordered"] and
                all(observed["multi"] != observed[name]
                    for name in ("first", "second", "reversed")),
                "two-subpass discriminator relations")
        require(len(subpass_boundaries) == 1 and
                "subpass=1 words=10" in subpass_boundaries[0][1],
                "one exact native subpass boundary")
    else:
        require(not subpass_boundaries,
                "subpass boundary without two-subpass witness")
    transfer_retired = one("PS5VK_CONSUMER_BUFFER_TRANSFER_RETIRED")
    start = one("PS5VK_CONSUMER_COMPUTE_START")
    pipeline = one("PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED")
    indirect = one("PS5VK_CONSUMER_DISPATCH_INDIRECT_RECORDED ")
    prepared = matching("PS5VK_QUEUE_PREPARED ")
    submitted = matching("PS5VK_QUEUE_SUBMIT ")
    suspended = matching("PS5VK_QUEUE_SUSPEND_POINT ")
    completed = matching("PS5VK_QUEUE_COMPLETED ")
    witness = one("PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS ")
    texel_format = one("PS5VK_CONSUMER_TEXEL_RGBA8_FORMAT ") if texel_rgba8 else None
    texel_witness = one("PS5VK_CONSUMER_TEXEL_RGBA8_SUCCESS ") if texel_rgba8 else None
    texel_formats_start = one("PS5VK_CONSUMER_TEXEL_FORMATS_START ") if texel_formats else None
    texel_format_rows = matching("PS5VK_CONSUMER_TEXEL_FORMAT_CASE ") if texel_formats else []
    texel_formats_success = one("PS5VK_CONSUMER_TEXEL_FORMATS_SUCCESS ") if texel_formats else None
    texel_formats_retired = one("PS5VK_CONSUMER_TEXEL_FORMATS_RETIRED") if texel_formats else None
    width_start = one("PS5VK_CONSUMER_STORAGE_WIDTH_START")
    width_pipelines = one("PS5VK_CONSUMER_STORAGE_WIDTH_PIPELINES_CREATED ")
    width_witness = one("PS5VK_CONSUMER_STORAGE_WIDTH_SUCCESS ")
    width_retired = one("PS5VK_CONSUMER_STORAGE_WIDTH_RETIRED")
    sync_start = one("PS5VK_CONSUMER_SYNC_START")
    sync_objects = one("PS5VK_CONSUMER_SYNC_OBJECTS_SUCCESS ")
    sync_witness = one("PS5VK_CONSUMER_SYNC_SUCCESS ")
    sync_retired = one("PS5VK_CONSUMER_SYNC_RETIRED")
    graphics_start = one("PS5VK_CONSUMER_GRAPHICS_START ")
    graphics_cold = one("PS5VK_CONSUMER_GRAPHICS_PIPELINE_COLD_CREATED")
    graphics_warm = one("PS5VK_CONSUMER_GRAPHICS_PIPELINE_WARM_CREATED")
    graphics_cache_release = one("PS5VK_CONSUMER_GRAPHICS_PIPELINE_DESTROYED ")
    dynamic_pipeline = one("PS5VK_CONSUMER_DYNAMIC_PIPELINE_CREATED ")
    present_created = one("PS5VK_CONSUMER_PRESENT_SURFACE_CREATED ")
    graphics_prepared = matching("PS5VK_GRAPHICS_PREPARED ")
    graphics_submitted = matching("PS5VK_GRAPHICS_SUBMIT ")
    graphics_suspended = matching("PS5VK_GRAPHICS_SUSPEND_POINT ")
    graphics_completed = matching("PS5VK_GRAPHICS_COMPLETED ")
    depth_reject = matching("PS5VK_CONSUMER_DEPTH_REJECT ")
    readbacks = matching("PS5VK_CONSUMER_READBACK ")
    present_destroyed = one("PS5VK_CONSUMER_PRESENT_SURFACE_DESTROYED")
    success = one("PS5VK_CONSUMER_TEST_SUCCESS")
    retired = one("PS5VK_CONSUMER_RESOURCES_RETIRED ")
    ready = one("PS5VK_READY_FOR_SHELL_CLOSE ")

    extra_texel_dispatches = len(TEXEL_FORMAT_CASES) if texel_formats else 0
    # Four compute submissions, plus the draw-parameter witness's one
    # resource-less prelude submission when that scenario ran; its staging
    # readback is frontend work and adds none.
    extra_witness_prelude = ((1 if draw_parameters_present else 0) + (1 if indirect_present else 0) +
                             (1 if raster_present else 0) + (1 if raster_gs_present else 0))
    # The indirect witness adds its own transfer prelude (the GENERAL transition
    # and clear of its target, prepared like the draw-parameter one) and
    # generates one command set on the GPU: one compute submission with one
    # dispatch, prepared, submitted and completed between its START and RESULT
    # markers.
    require(len(prepared) == 4 + extra_witness_prelude + (1 if texel_formats else 0) +
            indirect_compute_dispatches and
            all(len(rows) == 6 + extra_texel_dispatches + indirect_compute_dispatches
                for rows in (submitted, suspended, completed)),
            "resource, narrow and synchronization submit records")
    if indirect_present:
        generated = [row for row in prepared if indirect_start[0] < row[0] < indirect_result[0]]
        require(len(generated) == 2 and generated[0][1].endswith("dispatches=0") and
                generated[1][1].endswith("dispatches=1"),
                "indirect target prelude and compute-generated argument submissions")
    ordered = [boot, physical, physical_queries, negotiated,
               transfer_start, transfer_witness, transfer_retired,
               start]
    if secondary_present:
        ordered[-1:-1] = [secondary_start, secondary_witness, secondary_retired]
    if texel_format is not None:
        ordered.append(texel_format)
    ordered += [pipeline, indirect,
               prepared[0], submitted[0], suspended[0], completed[0], witness,
               ]
    if texel_witness is not None:
        ordered.append(texel_witness)
    ordered += [width_start, width_pipelines,
               prepared[1], submitted[1], suspended[1], completed[1],
               submitted[2], suspended[2], completed[2],
               width_witness, width_retired, sync_start, prepared[2],
               submitted[3], suspended[3], completed[3],
               submitted[4], suspended[4], completed[4],
               submitted[5], suspended[5], completed[5],
               prepared[3],
               sync_objects, sync_witness, sync_retired]
    if texel_formats:
        ordered += [texel_formats_start, prepared[4]]
        for index in range(len(TEXEL_FORMAT_CASES)):
            ordered += [submitted[6 + index], suspended[6 + index],
                        completed[6 + index]]
        ordered += [*texel_format_rows, texel_formats_success,
                    texel_formats_retired]
    ordered += [success, retired, ready]
    require([row[0] for row in ordered] == sorted({row[0] for row in ordered}),
            "resource witness ordering")
    if texel_rgba8:
        texel_contract = artifact.get("texel_rgba8", {})
        require(texel_contract.get("format") == "VK_FORMAT_R8G8B8A8_UNORM" and
                texel_contract.get("texels") == 64, "texel artifact contract")
        spirv_digest = str(texel_contract.get("shader_spirv_sha256", ""))
        require(len(spirv_digest) == 64 and
                all(c in "0123456789abcdef" for c in spirv_digest.lower()),
                "texel shader identity")
        require("format=r8g8b8a8_unorm" in texel_format[1] and
                "uniform_texel_reported=1" in texel_format[1],
                "the device must report the uniform texel buffer bit for RGBA8")
        require("format=r8g8b8a8_unorm" in texel_witness[1] and
                "channels=4" in texel_witness[1] and
                "packed_rgba_order=1" in texel_witness[1] and
                "mismatches=0" in texel_witness[1] and
                "guard_mismatches=0" in texel_witness[1],
                "RGBA8 texel fetch witness")
    if texel_formats:
        contract = artifact.get("texel_formats", {})
        hashes = contract.get("shader_spirv_sha256", {})
        require(contract.get("case_count") == len(TEXEL_FORMAT_CASES) and
                contract.get("components_per_case") == 4 and
                set(hashes) == {"float", "uint", "sint"} and
                all(isinstance(value, str) and len(value) == 64 and
                    all(c in "0123456789abcdef" for c in value.lower())
                    for value in hashes.values()), "texel-format artifact contract")
        require(texel_formats_start[1] ==
                f"PS5VK_CONSUMER_TEXEL_FORMATS_START cases={len(TEXEL_FORMAT_CASES)}",
                "texel-format start")
        require(len(texel_format_rows) == len(TEXEL_FORMAT_CASES),
                "texel-format case count")
        require(prepared[4][1].endswith(
                f"serial=57 dispatches={len(TEXEL_FORMAT_CASES)}"),
                "texel-format prepared dispatch count")
        for index in range(len(TEXEL_FORMAT_CASES)):
            require(submitted[6 + index][1].endswith(
                    f"serial=57 index={index} rc=0") and
                    suspended[6 + index][1].endswith(
                    f"serial=57 index={index} rc=0") and
                    f"serial=57 index={index} token=" in completed[6 + index][1],
                    f"texel-format dispatch lifecycle {index}")
        for index, ((name, shader_class, byte_count, expected), row) in enumerate(
                zip(TEXEL_FORMAT_CASES, texel_format_rows)):
            fields = dict(item.split("=", 1) for item in row[1].split()[1:])
            require(fields == {
                "index": str(index), "name": name, "class": shader_class,
                "bytes": str(byte_count), "expected": expected,
                "actual": expected, "mismatches": "0",
            }, f"texel-format case {index}")
        require(texel_formats_success[1] ==
                f"PS5VK_CONSUMER_TEXEL_FORMATS_SUCCESS cases={len(TEXEL_FORMAT_CASES)} "
                f"components={len(TEXEL_FORMAT_CASES)*4} mismatches=0 guard_mismatches=0",
                "texel-format success")
    if inpass_present:
        # The scenario runs after the last finite frame and before the surface
        # is destroyed, so it cannot be mistaken for one of the frames.
        require(readbacks[-1][0] < inpass_start[0] < inpass_witness[0] <
                inpass_retired[0] < present_destroyed[0],
                "in-pass secondary witness ordering")
    if two_subpass_present:
        require(inpass_retired and
                inpass_retired[0] < two_subpass_start[0] <
                subpass_boundaries[0][0] < two_subpass_witness[0] <
                two_subpass_retired[0] < present_destroyed[0],
                "two-subpass witness ordering")
    require(graphics_start[0] > sync_retired[0] and
            graphics_cold[0] < graphics_warm[0] < graphics_cache_release[0] <
            dynamic_pipeline[0] < present_created[0] and
            present_destroyed[0] < success[0], "graphics witness ordering")
    require("mode=finite" in boot[1], "finite mode")
    require(physical[1] ==
        "PS5VK_CONSUMER_PHYSICAL_DEVICE api=00400000 vendor=1002 device=0000 "
        "heap=268435456 heap_flags=00000001 type_flags=00000003 "
        "queue_flags=00000003 storage=268435456 uniform=65536 texel=65536 "
        "push=256 allocations=2048 granularity=131072 map_align=64 "
        "texel_align=4 ubo_align=256 ssbo_align=256 atom=64 shared=65536 "
        "invocations=1024 hash=be169e1b",
        "deterministic physical-device report")
    require(physical_queries[1].split()[1:] == [
        "devices=1", "queues=1", "two_call=1", "tail_preserved=1",
        "pnext_preserved=1", "formats=5", "image_supported=1",
        "image_rejected=1"],
        "physical-device query witnesses")
    require(negotiated[1].split()[1:] == [
        "instance_ext=1", "device_exts=4", "storageBuffer8BitAccess=1",
        "storageBuffer16BitAccess=1", "narrow_arithmetic=0",
        "robustBufferAccess=1", "shaderDrawParameters=1"],
        "narrow storage negotiation")
    require(transfer_witness[1].split()[1:] == [
        "copy_bytes=7", "update_bytes=8", "fill_bytes=20",
        "whole_tail_bytes=3", "guard_mismatches=0", "hash=9a158222"],
        "buffer-transfer oracle")
    # The serial numbering depends on whether the payload contains the
    # executable-secondary scenario, because executing one named secondary
    # produces exactly TWO extra segments: one for the child and one for the
    # primary that names it. Both numberings are pinned exactly rather than
    # relaxed, and the offset is derived from the scenario's own presence, so
    # an unexpected extra submission still fails either way. Recorded evidence
    # from payloads without the scenario keeps its original serials.
    shift = 2 if secondary_present else 0
    def serial(base):
        return base + shift
    require(prepared[0][1].endswith(f"serial={serial(5)} dispatches=1"),
            "one resource dispatch")
    require(indirect[1].split()[1:] == ["groups=1,1,1", "offset=512"],
            "indirect dispatch recording")
    require(prepared[1][1].endswith(f"serial={serial(6)} dispatches=2"),
            "two narrow dispatches")
    require(prepared[2][1].endswith(f"serial={serial(8)} dispatches=3"),
            "three synchronization dispatches")
    require(prepared[3][1].endswith(f"serial={serial(10)} dispatches=0"),
            "event dependency segment")
    require([row[1].rsplit(" ", 1)[0] for row in submitted[:6]] == [
                f"PS5VK_QUEUE_SUBMIT serial={serial(5)} index=0",
                f"PS5VK_QUEUE_SUBMIT serial={serial(6)} index=0",
                f"PS5VK_QUEUE_SUBMIT serial={serial(6)} index=1",
                f"PS5VK_QUEUE_SUBMIT serial={serial(8)} index=0",
                f"PS5VK_QUEUE_SUBMIT serial={serial(8)} index=1",
                f"PS5VK_QUEUE_SUBMIT serial={serial(8)} index=2"] and
            all(row[1].endswith("rc=0") for row in submitted), "submits")
    require([row[1].rsplit(" ", 1)[0] for row in suspended[:6]] == [
                f"PS5VK_QUEUE_SUSPEND_POINT serial={serial(5)} index=0",
                f"PS5VK_QUEUE_SUSPEND_POINT serial={serial(6)} index=0",
                f"PS5VK_QUEUE_SUSPEND_POINT serial={serial(6)} index=1",
                f"PS5VK_QUEUE_SUSPEND_POINT serial={serial(8)} index=0",
                f"PS5VK_QUEUE_SUSPEND_POINT serial={serial(8)} index=1",
                f"PS5VK_QUEUE_SUSPEND_POINT serial={serial(8)} index=2"] and
            all(row[1].endswith("rc=0") for row in suspended), "suspend points")
    expected_completion = ((serial(5), 0), (serial(6), 0), (serial(6), 1),
                           (serial(8), 0), (serial(8), 1), (serial(8), 2))
    require(all(f"serial={s} index={i}" in row[1]
                for row, (s, i) in zip(completed[:6], expected_completion)),
            "completion identities")
    require(f"serial={serial(5)} index=0 token={serial(5)}00000001 gcr=0070f528"
            in completed[0][1], "completion")
    require(witness[1].split()[1:] == [
        "sets=3", "storage=2", "uniform=1", "texel=1",
        "dynamic_ssbo=2", "dynamic_ubo=1", "offsets=256,256,256",
        "base_plus_dynamic=1",
        "push_bytes=4", "spec_constants=2", "multiplier=5",
        "extra_bias=11", "addend=19", "elements=64", "mismatches=0",
        "guard_words=192", "guard_mismatches=0"],
        "resource oracle")
    require(width_pipelines[1].endswith("count=2"), "narrow pipelines")
    require(width_witness[1].split()[1:] == [
        "storage8=1", "storage16=1", "elements8=64", "elements16=64",
        "checksum8=9575e8c5", "checksum16=603ddade", "mismatches8=0",
        "mismatches16=0", "guard_bytes8=4032", "guard_bytes16=3968",
        "guard_mismatches8=0", "guard_mismatches16=0"],
        "narrow storage oracle")
    sync_fields = sync_witness[1].split()[1:]
    require(sync_objects[1].split()[1:] == [
        "host_set_reset=1", "device_set_wait_reset=1",
        "binary_signal_wait=1", "semaphore_consumed=1"],
        "binary semaphore and event oracle")
    require(sync_fields[:8] == [
        "producer_consumer=1", "host_compute_host=1", "local_size=128",
        "waves32=4", "lds_atomic=1", "permutation=1", "counter=128",
        "sync_hash=467e2acd"] and len(sync_fields) == 11 and
        sync_fields[8].startswith("atomic_hash=") and
        len(sync_fields[8].split("=", 1)[1]) == 8 and
        sync_fields[9:] == ["mismatches=0", "guard_mismatches=0"],
        "synchronization oracle")
    require(retired[1].endswith("zero_tracked_allocations=1") and
            ready[1].endswith("resources_retired=1"), "resource retirement")
    require(graphics_start[1].endswith("mode=finite") and
            dynamic_pipeline[1].endswith("viewport=1 scissor=1"),
            "dynamic fixed-function setup")
    # Two extra graphics submissions when the in-pass scenario is present: one
    # for the secondary-executed pass and one for the inline control.
    graphics_count = ((40 if sampled is not None else 36) +
                      (2 if inpass_present else 0) +
                      (5 if two_subpass_present else 0) +
                      (len(DRAW_PARAMETER_CASES) if draw_parameters_present else 0) +
                      (len(INDIRECT_CASES) if indirect_present else 0) +
                      (len(RASTER_CASES) if raster_present else 0) +
                      (1 if raster_gs_present else 0))
    # The draw-parameter witness also records one transfer prelude (its colour
    # transition), which submits and completes but is never prepared by the
    # graphics backend. Every other submission is a graphics one.
    prelude_submissions = ((1 if draw_parameters_present else 0) + (1 if indirect_present else 0) +
                           (1 if raster_present else 0) + (1 if raster_gs_present else 0))
    require(len(graphics_prepared) == graphics_count and
            all(len(rows) == graphics_count + prelude_submissions for rows in
                (graphics_submitted, graphics_suspended, graphics_completed)),
            "two graphics submissions per finite frame")
    require(len(depth_reject) == 18 and len(readbacks) == 18,
            "fixed-function frame witnesses")

    def fields(message):
        return dict(item.split("=", 1) for item in message.split()[1:])

    for frame, (blocked, readback) in enumerate(zip(depth_reject, readbacks)):
        blocked_fields = fields(blocked[1])
        readback_fields = fields(readback[1])
        require(blocked_fields == {"frame": str(frame), "nonblack": "0", "valid": "1"},
                "depth reject oracle")
        require(readback_fields.get("frame") == str(frame) and
                readback_fields.get("slot") == str(frame & 1) and
                readback_fields.get("bad_alpha") == "0" and
                readback_fields.get("bad_sum") == "0" and
                readback_fields.get("valid") == "1" and
                int(readback_fields.get("changed", "0")) > 0,
                "load/depth/dynamic draw oracle")
    serials = [int(fields(row[1])["serial"]) for row in graphics_prepared]
    # The draw-parameter witness's colour transition is a transfer prelude: it
    # submits and completes, but the graphics backend never prepares it, so the
    # pairing and status checks below are over the graphics submissions only.
    graphics_serials = set(serials)
    prelude_serials = sorted({int(fields(row[1])["serial"]) for row in graphics_submitted} -
                             graphics_serials)
    require(len(prelude_serials) == prelude_submissions,
            "draw-parameter transfer prelude submissions")
    graphics_submitted = [row for row in graphics_submitted
                          if int(fields(row[1])["serial"]) in graphics_serials]
    graphics_suspended = [row for row in graphics_suspended
                          if int(fields(row[1])["serial"]) in graphics_serials]
    graphics_completed = [row for row in graphics_completed
                          if int(fields(row[1])["serial"]) in graphics_serials]
    expected_draws = ["1"] * graphics_count
    if two_subpass_present:
        expected_draws[-5:] = ["2", "2", "1", "1", "2"]
    if raster_present:
        # The raster witness frames record one floor draw plus the case's own
        # draws; they are the graphics submissions prepared between its START
        # and RESULT markers, in case order.
        raster_frames = [index for index, row in enumerate(graphics_prepared)
                         if raster_start[0] < row[0] < raster_result[0]]
        require(len(raster_frames) == len(RASTER_CASES), "raster witness frame count")
        for index, (_, draws) in zip(raster_frames, RASTER_CASES):
            expected_draws[index] = str(draws)
    # The graphics submissions must retire in increasing serial order. Their
    # serials are not contiguous: the payload's non-graphics submissions (the
    # compute blocks, the draw-parameter preludes and the per-case staging
    # readback) take serials in between, which is exactly what the counts above
    # and below bound.
    require(all(later > earlier for earlier, later in zip(serials, serials[1:])) and
            [fields(row[1]).get("draws") for row in graphics_prepared] == expected_draws and
            all(fields(row[1]).get("rc") == "0" for row in graphics_submitted) and
            all(fields(row[1]).get("rc") == "0" for row in graphics_suspended),
            "graphics serials and submit status")
    for index in range(graphics_count):
        sequence = [rows[index] for rows in (graphics_prepared, graphics_submitted,
                                            graphics_suspended, graphics_completed)]
        require([row[0] for row in sequence] == sorted({row[0] for row in sequence}) and
                all(int(fields(row[1])["serial"]) == serials[index] for row in sequence),
                "graphics completion pairing and ordering")
    sampled_rows = matching("PS5VK_CONSUMER_SAMPLED_SETS_")
    single_rows = matching("PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_")
    mixed_rows = matching("PS5VK_CONSUMER_MIXED_SETS_")
    mixed_profile = sampled_profile == "mixed-resources"
    if sampled is None:
        require(not sampled_rows and not single_rows and not mixed_rows,
                "sampled graphics without artifact contract")
    elif sampled_profile == "single-set":
        # One artifact profile only: the single-set run may not also claim the
        # four-set scope, and the per-set element count is part of the claim.
        require(not sampled_rows, "single-set profile must not report four-set markers")
        start = ("PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_START sets=1 descriptors=96"
                 " elements=96 rounds=4"
                 f" visibility={sampled['visibility_mask']:08x}"
                 f" fs_sha256={sampled['shader_spirv_sha256']}")
        require(len(single_rows) == 6 and single_rows[0][1] == start and
                single_rows[-1][1] == "PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RETIRED" and
                sync_retired[0] < single_rows[0][0] < single_rows[-1][0] < graphics_start[0],
                "single-set scope and shader identity")
        # The same ninety-six weighted elements as the four-set profile, placed in
        # one set. Element i keeps the source and weight of the four-set element
        # with the same global index, so the frozen reference words are identical;
        # matching them here is the proof that the per-set capacity, not a
        # different workload, produced them.
        expected_words = ("914c503b", "914b4d4f", "914a4643", "913a5449")
        previous = single_rows[0][0]
        for round_index, expected_word in enumerate(expected_words):
            row = single_rows[round_index + 1]
            require(row[1] == f"PS5VK_CONSUMER_SINGLE_SET_SAMPLERS_RESULT round={round_index} "
                    f"expected={expected_word} changed=471744 bad=0",
                    "single-set pixel oracle")
            require(previous < graphics_prepared[round_index][0] <
                    graphics_completed[round_index][0] < row[0] < single_rows[-1][0],
                    "single-set round completion")
            previous = row[0]
    elif mixed_profile:
        # One artifact profile only: a mixed run may not also claim the
        # sampler-only scope, and the uniform-buffer count is part of the claim.
        require(not sampled_rows, "mixed profile must not report sampler-only markers")
        require(sampled.get("uniform_buffers") == 4, "mixed-resource uniform-buffer count")
        start = (f"PS5VK_CONSUMER_MIXED_SETS_START sets=4 descriptors=96"
                 f" uniform_buffers=4 rounds=4"
                 f" visibility={sampled['visibility_mask']:08x}"
                 f" fs_sha256={sampled['shader_spirv_sha256']}")
        require(len(mixed_rows) == 6 and mixed_rows[0][1] == start and
                mixed_rows[-1][1] == "PS5VK_CONSUMER_MIXED_SETS_RETIRED" and
                sync_retired[0] < mixed_rows[0][0] < mixed_rows[-1][0] < graphics_start[0],
                "mixed-resource scope and shader identity")
        # Literal independent reference values for the mixed workload: the same
        # weighted 96-element sampler sum as the sampler-only profile, scaled by
        # the integer uniform weight 1 + set + round of that round. This is not a
        # pass flag from the application.
        expected_words = ("381e1f17", "4b29272a", "5d2d2c2c", "6f2c4038")
        previous = mixed_rows[0][0]
        for round_index, expected_word in enumerate(expected_words):
            row = mixed_rows[round_index + 1]
            require(row[1] == f"PS5VK_CONSUMER_MIXED_SETS_RESULT round={round_index} "
                    f"expected={expected_word} changed=471744 bad=0",
                    "mixed-resource pixel oracle")
            require(previous < graphics_prepared[round_index][0] <
                    graphics_completed[round_index][0] < row[0] < mixed_rows[-1][0],
                    "mixed-resource round completion")
            previous = row[0]
    else:
        require(not single_rows, "four-set profile must not report single-set markers")
        require(not mixed_rows, "sampler-only profile must not report mixed markers")
        start = "PS5VK_CONSUMER_SAMPLED_SETS_START sets=4 descriptors=96 rounds=4"
        if sampled_profile == "vertex-fragment":
            start += (f" stages=vertex-fragment vs_sha256={sampled['vertex_spirv_sha256']}"
                      f" fs_sha256={sampled['shader_spirv_sha256']}")
            # Legacy shared artifacts used explicit VS|FS visibility without
            # a mask field. New logs cannot silently fall back to that format.
            if "visibility_mask" in sampled:
                start += f" visibility={sampled['visibility_mask']:08x}"
        require(len(sampled_rows) == 6 and sampled_rows[0][1] ==
                start and
                sampled_rows[-1][1] == "PS5VK_CONSUMER_SAMPLED_SETS_RETIRED" and
                sync_retired[0] < sampled_rows[0][0] < sampled_rows[-1][0] < graphics_start[0],
                "sampled-graphics scope")
        # Literal independent reference values from the owned four-palette,
        # weighted 96-element shader. This is not a pass flag from the app.
        expected_words = ("914c503b", "914b4d4f", "914a4643", "913a5449")
        if sampled_profile == "vertex-fragment":
            # (FS forward weights + 2 * VS reverse weights) / 32768.
            expected_words = ("6d3a3b2f", "6d373d35", "6d42392a", "6d2e4135")
        previous = sampled_rows[0][0]
        for round_index, expected_word in enumerate(expected_words):
            row = sampled_rows[round_index + 1]
            require(row[1] == f"PS5VK_CONSUMER_SAMPLED_SETS_RESULT round={round_index} "
                    f"expected={expected_word} changed=471744 bad=0",
                    "sampled-graphics pixel oracle")
            require(previous < graphics_prepared[round_index][0] <
                    graphics_completed[round_index][0] < row[0] < sampled_rows[-1][0],
                    "sampled-graphics round completion")
            previous = row[0]

    result = {
        "run_id": receipt.get("run_id"),
        "deployment_self_sha256": digest,
        "log_sha256": receipt["sha256"],
        "descriptor_sets": 3,
        "mixed_resource_uniform_buffers": 4 if mixed_profile else 0,
        "storage_buffers": 2,
        "uniform_buffers": 1,
        "dynamic_storage_buffers": 2,
        "dynamic_uniform_buffers": 1,
        "dynamic_offsets": [256, 256, 256],
        "uniform_texel_buffers": 1,
        "push_constant_bytes": 4,
        "specialization_constants": 2,
        "elements_checked": 64,
        "guard_words_checked": 192,
        "buffer_transfer_bytes_checked": 67,
        "buffer_transfer_hash_fnv1a32": "9a158222",
        "secondary_execute_witnessed": secondary_present,
        "secondary_executed_hash_fnv1a32":
            SECONDARY_EXECUTED_HASH if secondary_present else None,
        "secondary_control_hash_fnv1a32":
            SECONDARY_CONTROL_HASH if secondary_present else None,
        "inpass_secondary_witnessed": inpass_present,
        "inpass_secondary_hash_fnv1a32": INPASS_HASH if inpass_present else None,
        "inpass_secondary_changed_pixels": INPASS_CHANGED if inpass_present else 0,
        "two_subpass_witnessed": two_subpass_present,
        "two_subpass_hash_fnv1a32": TWO_SUBPASS_HASH if two_subpass_present else None,
        "two_subpass_negative_hashes_fnv1a32": ([TWO_SUBPASS_FIRST_HASH,
            TWO_SUBPASS_SECOND_HASH, TWO_SUBPASS_REVERSED_HASH]
            if two_subpass_present else []),
        "indirect_dispatches_checked": 1,
        "storage8_elements_checked": 64,
        "storage16_elements_checked": 64,
        "narrow_guard_bytes_checked": 8000,
        "storage8_checksum_fnv1a32": "9575e8c5",
        "storage16_checksum_fnv1a32": "603ddade",
        "physical_device_report_fnv1a32": "be169e1b",
        "reported_heap_bytes": 268435456,
        "reported_memory_type_flags": "DEVICE_LOCAL|HOST_VISIBLE",
        "reported_host_coherent": False,
        "synchronization_words_checked": 64,
        "multiwave_atomic_lanes_checked": 128,
        "wave32_count": 4,
        "fixed_function_frames_checked": 18,
        "graphics_submissions_checked": graphics_count,
        "sampled_graphics_sets_checked": 4 if sampled is not None else 0,
        "sampled_graphics_descriptors_per_round": 96 if sampled is not None else 0,
        "sampled_graphics_rounds_checked": 4 if sampled is not None else 0,
        "sampled_graphics_stage_profile": sampled_profile,
        "single_set_sampler_elements": (96 if sampled_profile == "single-set" else 0),
        "uniform_texel_formats_checked": len(TEXEL_FORMAT_CASES) if texel_formats else 0,
        "indirect_draw_cases": (len(INDIRECT_CASES) if indirect_present else 0),
        "raster_cases": (len(RASTER_CASES) if raster_present else 0),
        "raster_viewport_index_cases": (1 if raster_gs_present else 0),
        # True when the staged SDK reported the four features through the
        # PS5VK_RASTER_DIAGNOSTIC measurement gate rather than the shipping
        # platform mask: such a run is measurement evidence, not a claim that
        # the shipping profile advertises them.
        "raster_diagnostic_features": bool(artifact.get("raster_state", {}).get("diagnostic_features")),
        "indirect_max_commands_arenas": (arenas_for_max if indirect_present else 0),
        "draw_parameter_cases": (len(DRAW_PARAMETER_CASES)
                                 if draw_parameters_present else 0),
        "sampled_graphics_visibility_mask": (sampled.get("visibility_mask", 0x11)
            if sampled_profile == "vertex-fragment" else None),
        "sampled_graphics_exceeds_advertised_limits": sampled is not None,
        "dynamic_viewport_scissor": True,
        "attachment_load_preservation": True,
        "depth_reject_then_accept": True,
        "clean_tcp": True,
        "os_close": "requires independent lifecycle evidence",
    }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    parser.add_argument("--texel-rgba8", action="store_true")
    parser.add_argument("--texel-formats", action="store_true")
    args = parser.parse_args()
    receipt = json.loads(args.log.with_suffix(".json").read_text())
    artifact = json.loads(args.artifact.read_text())
    print(json.dumps(validate(args.log.read_bytes(), receipt, artifact,
                              texel_rgba8=args.texel_rgba8,
                              texel_formats=args.texel_formats), indent=2))


if __name__ == "__main__":
    main()
