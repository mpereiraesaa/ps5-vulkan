"""Strict evidence for the native sixteen/sparse vertex-binding diagnostic.

Artifact hashes must additionally be tied to a verified deployment by the run
operator. This parser does not prove which executable the OS launched, nor OS
process exit, and never promotes generic Vulkan conformance.
"""
import argparse
import hashlib
import json
from pathlib import Path

MASKS = ("ffff", "8000", "8008", "ffff")
COPY_BYTES = ("7552", "457", "938", "7552")


def require(ok, label):
    if not ok:
        raise ValueError(label)


def validate(log, receipt, artifact):
    digest = artifact.get("files", {}).get("eboot.bin", "")
    require(len(digest) == 64 and all(c in "0123456789abcdef" for c in digest),
            "artifact identity")
    require(artifact.get("scissor_probe") == 13 and
            artifact.get("stage") == "graphics-api-native-presentation-reuse" and
            artifact.get("compiler") == "runtime-psbc-aco" and
            artifact.get("graphics_shader_source") == "owned-runtime-vertex-bindings" and
            artifact.get("geometry_fixture") == "sixteen-and-sparse-vertex-bindings" and
            artifact.get("termination") == "shell-close-after-cleanup",
            "artifact profile")
    require(hashlib.sha256(log).hexdigest() == receipt.get("sha256"), "log hash")
    require(receipt.get("clean") is True and receipt.get("bye") is True and
            receipt.get("gaps") == [] and receipt.get("raw_lines") == 0 and
            receipt.get("transport") == "tcp" and receipt.get("protocol") == "ps5log/1",
            "complete TCP receipt")
    lines = log.decode().splitlines()
    require(len(lines) > 2 and lines[0].startswith("HELLO ps5log/1 "), "hello")
    hello = dict(word.split("=", 1) for word in lines[0].split()[2:])
    require(hello.get("title") == "PPSA99994" and hello.get("app") == "ps5vk" and
            all(hello.get(k) == receipt.get("identity", {}).get(k)
                for k in ("title", "app", "boot")), "stream identity")
    records = []
    stamp = -1
    for seq, line in enumerate(lines[1:-1], 1):
        fields = line.split("\t", 3)
        require(len(fields) == 4 and fields[0] == str(seq) and
                int(fields[1]) >= stamp and fields[2] in ("INFO", "MARK"),
                "record integrity")
        stamp = int(fields[1])
        words = fields[3].split()
        require(words and all("=" in w for w in words[1:]), "record fields")
        records.append((words[0], dict(w.split("=", 1) for w in words[1:])))
    require(receipt.get("records") == receipt.get("last_seq") == len(records) and
            lines[-1] == f"BYE seq={len(records)} reason=graphics-api-end",
            "complete BYE")

    def matching(name):
        return [(i, fields) for i, (tag, fields) in enumerate(records) if tag == name]

    limits = matching("PS5VK_GRAPHICS_LIMITS")
    require(len(limits) == 1 and limits[0][1].get("bindings") == "16",
            "reported binding capacity")
    names = ("PS5VK_RUNTIME_GRAPHICS_CACHE", "PS5VK_BINDINGS_INPUT",
             "PS5VK_BINDINGS_PREPARED", "PS5VK_GRAPHICS_SUBMIT",
             "PS5VK_GRAPHICS_COMPLETED", "PS5VK_BINDINGS_READBACK",
             "PS5VK_VIDEO_PRESENTED", "PS5VK_GRAPHICS_REUSE_END")
    groups = [matching(name) for name in names]
    require(all(len(g) == 4 for g in groups), "four complete GPU cases")
    computes, compute_ends = matching("PS5VK_COMPUTE_RESULT"), matching("PS5VK_COMPUTE_END")
    require(len(computes) == 48 and len(compute_ends) == 8 and
            all(f.get("outputs") == f.get("guards") == "0" and f.get("checked") == "3072"
                for _, f in computes) and
            all(f.get("rounds") == "6" and f.get("dispatches") == "12"
                for _, f in compute_ends), "compute controls")
    previous = -1
    for case, mask in enumerate(MASKS):
        order = [g[case][0] for g in groups]
        require(previous < compute_ends[2*case][0] < order[0] and
                order == sorted(set(order)) and order[-1] < compute_ends[2*case+1][0],
                "case ordering")
        for phase in range(2):
            end = compute_ends[2*case+phase][0]
            lower = previous if phase == 0 else order[-1]
            chunk = computes[(2*case+phase)*6:(2*case+phase+1)*6]
            require([f.get("round") for _, f in chunk] == list(map(str, range(6))) and
                    all(lower < i < end for i, _ in chunk), "compute phase ordering")
        previous = compute_ends[2*case+1][0]
        cache, source, prepared, submit, complete, result, present, end = [g[case][1] for g in groups]
        require(cache.get("rc") == "0" and cache.get("hit") == ("1" if case == 3 else "0") and
                cache.get("compiled_pairs") == cache.get("misses") == str(min(case+1, 3)) and
                cache.get("hits") == str(case//3), "runtime cache")
        require(source.get("case") == result.get("case") == str(case) and
                source.get("mask") == prepared.get("mask") == result.get("mask") == mask and
                source.get("declared") == "16" and
                all(source.get(k) == "1" for k in
                    ("reversed_locations", "distinct_buffers", "odd_offsets")) and
                prepared.get("copied_bytes") == COPY_BYTES[case], "binding identity")
        require(prepared.get("serial") == submit.get("serial") == complete.get("serial") and
                submit.get("rc") == "0" and result.get("expected_white") == "471744" and
                result.get("other") == "0" and result.get("valid") == "1" and
                present.get("fence") == "0" and present.get("matching_event") == "1" and
                end.get("displayed") == "0", "GPU oracle")
    platform, cleanup = matching("PS5VK_PLATFORM_CLOSE"), matching("PS5VK_GRAPHICS_API_CLEANUP_COMPLETE")
    require(len(platform) == len(cleanup) == 1 and
            previous < platform[0][0] < cleanup[0][0] and
            platform[0][1].get("rc") == platform[0][1].get("allocations_bytes") == "0",
            "resource cleanup")
    return {"self_sha256": digest, "cases": 4, "gpu_readback": True,
            "masks": MASKS, "shader_optimization": True, "warm_cache": True,
            "process_exit_verified": False, "deployment_identity_verified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path)
    parser.add_argument("receipt", type=Path)
    parser.add_argument("artifact", type=Path)
    args = parser.parse_args()
    print(json.dumps(validate(args.log.read_bytes(), json.loads(args.receipt.read_text()),
                              json.loads(args.artifact.read_text())), indent=2))
