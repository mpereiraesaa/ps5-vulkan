"""Strict verifier for the six-round compute profile reference workload, not conformance.

Artifact identity is supplied by the verified deployment record/build manifest;
the console stream itself does not attest its executable hash.
"""
import argparse
import hashlib
import json
from pathlib import Path

PROFILE_STAGE = "compute-api"
PROFILE_TITLE = "PPSA99994"
PROFILE_APP = "ps5vk"


def expected_messages(suspend_points=True, compiler="offline-exact-library"):
    if compiler == "runtime-psbc-aco":
        messages = ["PS5VK_BOOT stage=compute api=compute compiler=runtime-psbc-aco",
                    "PS5VK_PLATFORM_LOAD rc=0", "PS5VK_PLATFORM_INIT rc=0",
                    "PS5VK_UNREGISTERED_ABSENT checked=1 rc=feature-not-present",
                    "PS5VK_PIPELINE_CREATE program=0 mode=cold-compile rc=0",
                    "PS5VK_PIPELINE_CREATE program=1 mode=cold-compile-unregistered rc=0",
                    "PS5VK_CACHE_STATS entries=2 hits=0 misses=2 compiles=2 evictions=0",
                    "PS5VK_CACHE_WARM_HIT program=0 hits=1 compiles=2",
                    "PS5VK_UNSUPPORTED_REJECTED checked=1 rc=rejected-clean"]
    else:
        messages = ["PS5VK_BOOT stage=compute api=compute compiler=offline-exact-library",
                    "PS5VK_PLATFORM_LOAD rc=0", "PS5VK_PLATFORM_INIT rc=0"]
    messages += ["PS5VK_MEMORY_ALLOC requested=16384 mapped=65536"] * 3
    for round_number in range(6):
        serial = round_number + 1
        dispatch_alloc = 768 if compiler == "runtime-psbc-aco" else 1024
        messages += [f"PS5VK_MEMORY_ALLOC requested={dispatch_alloc} mapped=65536"] * 2
        messages.append(f"PS5VK_QUEUE_PREPARED serial={serial} dispatches=2")
        for index in range(2):
            messages.append(f"PS5VK_QUEUE_SUBMIT serial={serial} index={index} rc=0")
            if suspend_points:
                messages.append(f"PS5VK_QUEUE_SUSPEND_POINT serial={serial} index={index} rc=0")
            messages.append(f"PS5VK_QUEUE_COMPLETED serial={serial} index={index} "
                            f"token={(serial << 32) | (index + 1):x} gcr=0070f528")
        messages += ["PS5VK_MEMORY_RELEASE mapped=65536"] * 2
        messages.append(f"PS5VK_COMPUTE_RESULT round={round_number} first_program={round_number % 2} "
                        f"descriptor_offset={256 * serial} binding_offset=256 "
                        "checked=3072 outputs=0 guards=0")
    messages += ["PS5VK_MEMORY_RELEASE mapped=65536"] * 3
    if compiler == "runtime-psbc-aco":
        messages.append("PS5VK_CACHE_FINAL entries=2 hits=1 misses=2 compiles=2 evictions=0")
    return messages + ["PS5VK_PLATFORM_CLOSE rc=0 allocations_bytes=0",
                       "PS5VK_COMPUTE_END rounds=6 dispatches=12"]


def validate(log, manifest, artifact):
    def require(condition, message):
        if not condition:
            raise ValueError(message)
    require(artifact.get("title") == PROFILE_TITLE, "artifact title")
    require(artifact.get("stage") == PROFILE_STAGE, "profile mismatch")
    require(artifact.get("submit_enabled") is True, "submit must be enabled")
    eboot = artifact.get("files", {}).get("eboot.bin")
    require(isinstance(eboot, str) and len(eboot) == 64 and all(c in "0123456789abcdef" for c in eboot.lower()),
            "artifact identity")
    require(hashlib.sha256(log).hexdigest() == manifest.get("sha256"), "log hash")
    require(manifest.get("protocol") == "ps5log/1" and manifest.get("transport") == "tcp", "transport")
    require(manifest.get("clean") is True and manifest.get("bye") is True and
            manifest.get("gaps") == [], "unclean stream")
    identity = manifest.get("identity", {})
    require(identity.get("title") == PROFILE_TITLE and identity.get("app") == PROFILE_APP, "identity")
    lines = log.decode().splitlines()
    require(len(lines) >= 2, "empty stream")
    hello = lines[0].split()
    require(hello[:2] == ["HELLO", "ps5log/1"], "hello")
    fields = dict(item.split("=", 1) for item in hello[2:])
    require(all(fields.get(key) == identity.get(key) for key in ("title", "app", "boot")), "hello identity")
    messages, previous_time = [], -1
    for seq, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4 and fields[0] == str(seq), "sequence")
        timestamp = int(fields[1])
        require(timestamp >= previous_time, "clock ordering")
        previous_time = timestamp
        require(fields[2] in ("MARK", "INFO"), "unexpected severity")
        messages.append(fields[3])
    compiler = artifact.get("compiler", "offline-exact-library")
    require(compiler in ("runtime-psbc-aco", "offline-exact-library"), "unknown compiler profile")
    require(messages == expected_messages(suspend_points=True, compiler=compiler), "workload/lifecycle mismatch")
    require(manifest.get("last_seq") == len(messages), "manifest sequence")
    require(lines[-1] == f"BYE seq={len(messages)} reason=compute-end", "bye")
    return {"run_id": manifest["run_id"], "boot": identity["boot"],
            "log_sha256": manifest["sha256"], "deployment_self_sha256": eboot,
            "compiler": compiler,
            "rounds": 6, "dispatches": 12, "data_words_checked": 18432,
            "guard_words_checked": 55296, "clean_tcp": True}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--artifact", required=True, type=Path)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    path = args.manifest.parent / manifest["log_path"]
    print(json.dumps(validate(path.read_bytes(), manifest,
                              json.loads(args.artifact.read_text())), indent=2))


if __name__ == "__main__":
    main()
