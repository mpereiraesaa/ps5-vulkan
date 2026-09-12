"""Validate bootstrap compute profile TCP evidence; acceptance is deliberately narrower than Vulkan."""
import argparse
import hashlib
import json
from pathlib import Path

SHADER = "4ce5d1dc537e4e0d4a9eee302a087e882a66e541ee53ea209e2ad65f53b0f356"

def validate(log, manifest, require_suspend=False):
    def require(condition, message):
        if not condition:
            raise ValueError(message)
    require(hashlib.sha256(log).hexdigest() == manifest["sha256"], "log hash")
    require(manifest["transport"] == "tcp" and manifest["protocol"] == "ps5log/1", "transport")
    require(manifest["clean"] and manifest["bye"] and not manifest["gaps"], "unclean stream")
    require(manifest["identity"]["title"] == "PPSA99994" and
            manifest["identity"]["app"] == "ps5vk", "identity")
    lines = log.decode().splitlines()
    require(lines[0].startswith("HELLO ps5log/1 title=PPSA99994 app=ps5vk "), "hello")
    require("boot=" + manifest["identity"]["boot"] in lines[0], "boot")
    messages = []
    for seq, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4 and fields[0] == str(seq), "sequence")
        require(fields[2] != "ERR", "runtime error")
        messages.append(fields[3])
    require(lines[-1] == f"BYE seq={len(messages)} reason=native-end", "bye")
    require(manifest["last_seq"] == len(messages), "manifest sequence")
    required = [
        "agc_load=0", "agc_init=0",
        f"PS5VK_PREPARED shader={SHADER} dwords=82 submit_enabled=1 dma_only=0",
        "PS5VK_SUBMIT rc=0", "PS5VK_COMPLETION observed=13579bdf2468ace0",
        "PS5VK_VISIBILITY control_before_flush=00000000",
        "PS5VK_RESULT completion=token outputs=0 inputs=0 guards=0 first=18446744073709551615",
        "PS5VK_CLEANUP_BEGIN result=0", "PS5VK_CLEANUP_END result=0 allocations_live=0",
        "compute_run=0", "agc_unload=0", "PS5VK_NATIVE_END",
    ]
    positions = []
    if require_suspend:
        required.insert(required.index("PS5VK_SUBMIT rc=0")+1,"PS5VK_SUSPEND_POINT rc=0")
    suspend_records=[m for m in messages if m.startswith("PS5VK_SUSPEND_POINT ")]
    require(suspend_records == (["PS5VK_SUSPEND_POINT rc=0"] if require_suspend else []),
            "suspend protocol mismatch")
    for message in required:
        require(messages.count(message) == 1, "missing/duplicate " + message)
        positions.append(messages.index(message))
    require(positions == sorted(positions), "lifecycle order")
    require(not any("VISIBILITY_SAMPLE" in m or "PS5VK_INSPECT" in m for m in messages),
            "diagnostic delay not accepted")
    return {"run_id": manifest["run_id"], "boot": manifest["identity"]["boot"],
            "log_sha256": manifest["sha256"], "results": 1024, "guards": 64,
            "completion": "13579bdf2468ace0", "clean_tcp": True}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--require-suspend", action="store_true",
                        help="validate current submit/suspend/completion ordering; omit only for historical bootstrap compute profile")
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    print(json.dumps(validate((args.manifest.parent / manifest["log_path"]).read_bytes(), manifest,
                              args.require_suspend), indent=2))

if __name__ == "__main__":
    main()
