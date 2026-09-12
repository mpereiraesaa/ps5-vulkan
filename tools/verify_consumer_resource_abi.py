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

    def one(prefix):
        found = [(index, message) for index, message in enumerate(messages)
                 if message.startswith(prefix)]
        require(len(found) == 1, prefix)
        return found[0]

    boot = one("PS5VK_CONSUMER_BOOT ")
    start = one("PS5VK_CONSUMER_COMPUTE_START")
    pipeline = one("PS5VK_CONSUMER_COMPUTE_PIPELINE_CREATED")
    prepared = one("PS5VK_QUEUE_PREPARED ")
    submitted = one("PS5VK_QUEUE_SUBMIT ")
    suspended = one("PS5VK_QUEUE_SUSPEND_POINT ")
    completed = one("PS5VK_QUEUE_COMPLETED ")
    witness = one("PS5VK_CONSUMER_RESOURCE_ABI_SUCCESS ")
    success = one("PS5VK_CONSUMER_TEST_SUCCESS")
    retired = one("PS5VK_CONSUMER_RESOURCES_RETIRED ")
    ready = one("PS5VK_READY_FOR_SHELL_CLOSE ")

    require([row[0] for row in (boot, start, pipeline, prepared, submitted,
                                suspended, completed, witness, success,
                                retired, ready)] == sorted({row[0] for row in
                                (boot, start, pipeline, prepared, submitted,
                                 suspended, completed, witness, success,
                                 retired, ready)}), "resource witness ordering")
    require("mode=finite" in boot[1], "finite mode")
    require(prepared[1].endswith("serial=1 dispatches=1"), "one dispatch")
    require(submitted[1].endswith("serial=1 index=0 rc=0"), "submit")
    require(suspended[1].endswith("serial=1 index=0 rc=0"), "suspend point")
    require("serial=1 index=0 token=100000001 gcr=0070f528" in completed[1],
            "completion")
    require(witness[1].split()[1:] == [
        "sets=3", "storage=2", "uniform=1", "texel=1", "elements=64",
        "mismatches=0", "guard_words=128", "guard_mismatches=0"],
        "resource oracle")
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
        "elements_checked": 64,
        "guard_words_checked": 128,
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
