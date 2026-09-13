"""Verify the public SDK consumer's multi-set compute witness.

This validates one bounded resource-ABI workload and its telemetry integrity.
It is not Vulkan conformance and does not infer support beyond the exact
descriptor types, formats and shader exercised by the consumer.
"""
import argparse
import hashlib
import json
from pathlib import Path


TITLE = "PPSA99994"
APP = "ps5vk"


def validate(log, receipt, artifact):
    def require(condition, label):
        if not condition:
            raise ValueError(label)

    digest = artifact.get("files", {}).get("eboot.bin", "")
    require(artifact.get("title") == TITLE and
            artifact.get("profile") == "public-consumer-resource-abi" and
            artifact.get("submit_enabled") is True, "artifact profile")
    require(len(digest) == 64 and
            all(c in "0123456789abcdef" for c in digest.lower()),
            "artifact identity")
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
            all(len(sync_artifact.get(key, "")) == 64 for key in
                ("sync_producer_spirv_sha256", "sync_consumer_spirv_sha256",
                 "shared_atomic_multiwave_spirv_sha256")),
            "synchronization artifact contract")
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
    start = one("PS5VK_CONSUMER_COMPUTE_START")
    pipeline = one("PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED")
    prepared = matching("PS5VK_QUEUE_PREPARED ")
    submitted = matching("PS5VK_QUEUE_SUBMIT ")
    suspended = matching("PS5VK_QUEUE_SUSPEND_POINT ")
    completed = matching("PS5VK_QUEUE_COMPLETED ")
    witness = one("PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS ")
    width_start = one("PS5VK_CONSUMER_STORAGE_WIDTH_START")
    width_pipelines = one("PS5VK_CONSUMER_STORAGE_WIDTH_PIPELINES_CREATED ")
    width_witness = one("PS5VK_CONSUMER_STORAGE_WIDTH_SUCCESS ")
    width_retired = one("PS5VK_CONSUMER_STORAGE_WIDTH_RETIRED")
    sync_start = one("PS5VK_CONSUMER_SYNC_START")
    sync_witness = one("PS5VK_CONSUMER_SYNC_SUCCESS ")
    sync_retired = one("PS5VK_CONSUMER_SYNC_RETIRED")
    success = one("PS5VK_CONSUMER_TEST_SUCCESS")
    retired = one("PS5VK_CONSUMER_RESOURCES_RETIRED ")
    ready = one("PS5VK_READY_FOR_SHELL_CLOSE ")

    require(len(prepared) == 3 and
            all(len(rows) == 6 for rows in (submitted, suspended, completed)),
            "resource, narrow and synchronization submit records")
    ordered = [boot, physical, physical_queries, negotiated, start, pipeline,
               prepared[0], submitted[0], suspended[0], completed[0], witness,
               width_start, width_pipelines,
               prepared[1], submitted[1], suspended[1], completed[1],
               submitted[2], suspended[2], completed[2],
               width_witness, width_retired, sync_start, prepared[2],
               submitted[3], suspended[3], completed[3],
               submitted[4], suspended[4], completed[4],
               submitted[5], suspended[5], completed[5],
               sync_witness, sync_retired, success, retired, ready]
    require([row[0] for row in ordered] == sorted({row[0] for row in ordered}),
            "resource witness ordering")
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
        "instance_ext=1", "device_exts=3", "storageBuffer8BitAccess=1",
        "storageBuffer16BitAccess=1", "narrow_arithmetic=0"],
        "narrow storage negotiation")
    require(prepared[0][1].endswith("serial=1 dispatches=1"), "one resource dispatch")
    require(prepared[1][1].endswith("serial=2 dispatches=2"), "two narrow dispatches")
    require(prepared[2][1].endswith("serial=3 dispatches=3"), "three synchronization dispatches")
    require([row[1].rsplit(" ", 1)[0] for row in submitted] == [
                "PS5VK_QUEUE_SUBMIT serial=1 index=0",
                "PS5VK_QUEUE_SUBMIT serial=2 index=0",
                "PS5VK_QUEUE_SUBMIT serial=2 index=1",
                "PS5VK_QUEUE_SUBMIT serial=3 index=0",
                "PS5VK_QUEUE_SUBMIT serial=3 index=1",
                "PS5VK_QUEUE_SUBMIT serial=3 index=2"] and
            all(row[1].endswith("rc=0") for row in submitted), "submits")
    require([row[1].rsplit(" ", 1)[0] for row in suspended] == [
                "PS5VK_QUEUE_SUSPEND_POINT serial=1 index=0",
                "PS5VK_QUEUE_SUSPEND_POINT serial=2 index=0",
                "PS5VK_QUEUE_SUSPEND_POINT serial=2 index=1",
                "PS5VK_QUEUE_SUSPEND_POINT serial=3 index=0",
                "PS5VK_QUEUE_SUSPEND_POINT serial=3 index=1",
                "PS5VK_QUEUE_SUSPEND_POINT serial=3 index=2"] and
            all(row[1].endswith("rc=0") for row in suspended), "suspend points")
    expected_completion = ((1, 0), (2, 0), (2, 1),
                           (3, 0), (3, 1), (3, 2))
    require(all(f"serial={serial} index={index}" in row[1]
                for row, (serial, index) in zip(completed, expected_completion)),
            "completion identities")
    require("serial=1 index=0 token=100000001 gcr=0070f528" in completed[0][1],
            "completion")
    require(witness[1].split()[1:] == [
        "sets=3", "storage=2", "uniform=1", "texel=1",
        "push_bytes=4", "spec_constants=2", "multiplier=5",
        "extra_bias=11", "addend=19", "elements=64", "mismatches=0",
        "guard_words=128", "guard_mismatches=0"],
        "resource oracle")
    require(width_pipelines[1].endswith("count=2"), "narrow pipelines")
    require(width_witness[1].split()[1:] == [
        "storage8=1", "storage16=1", "elements8=64", "elements16=64",
        "checksum8=9575e8c5", "checksum16=603ddade", "mismatches8=0",
        "mismatches16=0", "guard_bytes8=4032", "guard_bytes16=3968",
        "guard_mismatches8=0", "guard_mismatches16=0"],
        "narrow storage oracle")
    sync_fields = sync_witness[1].split()[1:]
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

    return {
        "run_id": receipt.get("run_id"),
        "deployment_self_sha256": digest,
        "log_sha256": receipt["sha256"],
        "descriptor_sets": 3,
        "storage_buffers": 2,
        "uniform_buffers": 1,
        "uniform_texel_buffers": 1,
        "push_constant_bytes": 4,
        "specialization_constants": 2,
        "elements_checked": 64,
        "guard_words_checked": 128,
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
        "clean_tcp": True,
        "os_close": "requires independent lifecycle evidence",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    args = parser.parse_args()
    receipt = json.loads(args.log.with_suffix(".json").read_text())
    artifact = json.loads(args.artifact.read_text())
    print(json.dumps(validate(args.log.read_bytes(), receipt, artifact), indent=2))


if __name__ == "__main__":
    main()
