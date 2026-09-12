"""Audit the completed prefix of continuous scene telemetry, not OS cleanup.

An OS close can truncate the final frame. This reports that tail, never counts
it as completed, and requires independent deployment/close evidence.
"""
import argparse
import hashlib
import json
from pathlib import Path


ORDER = ("PS5VK_GRAPHICS_REUSE_BEGIN", "PS5VK_GRAPHICS_PREPARED",
         "PS5VK_GRAPHICS_SUBMIT", "PS5VK_GRAPHICS_SUSPEND_POINT",
         "PS5VK_GRAPHICS_COMPLETED", "PS5VK_GRAPHICS_API_READBACK",
         "PS5VK_TEXTURE_READBACK", "PS5VK_VIDEO_SUBMIT",
         "PS5VK_VIDEO_SUSPEND_POINT", "PS5VK_VIDEO_PRESENTED",
         "PS5VK_GRAPHICS_REUSE_END")


def audit(text):
    stage = completed = 0
    displayed = -1
    serial = None
    first_serial = None
    timestamp = -1
    started = False
    heartbeats = []
    for seq, line in enumerate(text.splitlines()[1:], 1):
        fields = line.split("\t", 3)
        if len(fields) != 4 or fields[0] != str(seq) or int(fields[1]) < timestamp:
            raise ValueError("sequence or timestamp")
        timestamp = int(fields[1])
        if fields[2] not in ("MARK", "INFO"):
            raise ValueError("runtime severity")
        parts = fields[3].split()
        name = parts[0]
        values = dict(p.split("=", 1) for p in parts[1:])
        if name == "PS5VK_SCENE_LOOP":
            if started or values.get("mode") != "continuous" or values.get("pause_us") != "0":
                raise ValueError("continuous identity")
            started = True
        if name == "PS5VK_SCENE_HEARTBEAT":
            count, elapsed, token = (int(values[k]) for k in ("frames", "elapsed_ns", "token"))
            if (not started or stage != 10 or count != completed + 1 or
                    token != count or (count - 1) % 120 or elapsed <= 0):
                raise ValueError("heartbeat frame identity")
            if heartbeats and (count != heartbeats[-1][0] + 120 or elapsed <= heartbeats[-1][1]):
                raise ValueError("heartbeat progression")
            heartbeats.append((count, elapsed))
        if name not in ORDER:
            continue
        if not started or name != ORDER[stage]:
            raise ValueError("frame order: " + name)
        phase, slot, token = completed % 180, completed % 2, completed + 1
        if "rc" in values and values["rc"] != "0":
            raise ValueError("failed native operation")
        if "token" in values and int(values["token"]) != token:
            raise ValueError("presentation token")
        if name == ORDER[0]:
            if int(values["frame"]) != phase or int(values["slot"]) != slot or slot == displayed:
                raise ValueError("write to displayed/wrong slot")
            if int(values["offset"]) != slot * 0x4000000:
                raise ValueError("image offset")
        if name == ORDER[1]:
            serial = int(values["serial"])
            if first_serial is None:
                first_serial = serial
            if serial != first_serial + completed or values["draws"] != "1":
                raise ValueError("graphics serial/draw count")
        if "serial" in values and int(values["serial"]) != serial:
            raise ValueError("graphics completion serial")
        if name == ORDER[5] and any(values[k] != v for k, v in
                (("valid", "1"), ("bad_alpha", "0"), ("bad_sum", "0"))):
            raise ValueError("readback failure")
        if name == ORDER[6] and (int(values["frame"]) != phase or values["unexpected"] != "0"):
            raise ValueError("texture readback")
        if name == ORDER[9]:
            if any(values[k] != v for k, v in
                   (("fence", "0"), ("matching_event", "1"), ("hold_seconds", "0"))):
                raise ValueError("flip completion")
            displayed = slot
        if name == ORDER[10]:
            if any(int(values[k]) != v for k, v in
                   (("frame", phase), ("slot", slot), ("displayed", slot))):
                raise ValueError("reuse retirement")
            completed += 1
        stage = (stage + 1) % len(ORDER)
    if not started or not completed:
        raise ValueError("no completed continuous frames")
    intervals = [(b[0]-a[0])*1e9/(b[1]-a[1]) for a,b in zip(heartbeats, heartbeats[1:])]
    cadence = dict(heartbeat_count=len(heartbeats), interval_count=len(intervals),
                   mean_fps=None, min_interval_fps=None, max_interval_fps=None)
    if intervals:
        cadence.update(mean_fps=(heartbeats[-1][0]-heartbeats[0][0])*1e9/
                       (heartbeats[-1][1]-heartbeats[0][1]),
                       min_interval_fps=min(intervals), max_interval_fps=max(intervals))
    return dict(completed_frames=completed, incomplete_tail_stage=ORDER[stage] if stage else None,
                presentation_loop_cadence=cadence,
                displayed_slot=displayed, graceful_cleanup_proven=False,
                artifact_identity_proven=False, os_close_proven=False)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    args = parser.parse_args()
    meta = json.loads(args.manifest.read_text())
    data = (args.manifest.parent / meta["log_path"]).read_bytes()
    if (hashlib.sha256(data).hexdigest() != meta["sha256"] or meta["gaps"] or
            meta["protocol"] != "ps5log/1" or meta["transport"] != "tcp" or
            meta["identity"]["title"] != "PPSA99994" or meta["identity"]["app"] != "ps5vk"):
        raise ValueError("transport/identity/hash")
    lines = data.decode().splitlines()
    hello = lines[0].split()
    identity = dict(p.split("=",1) for p in hello[2:])
    if (hello[:2] != ["HELLO", "ps5log/1"] or
            any(identity.get(k) != meta["identity"][k] for k in ("title","app","boot")) or
            meta["last_seq"] != len(lines)-1):
        raise ValueError("hello/sequence identity")
    print(json.dumps(audit(data.decode()), indent=2))


if __name__ == "__main__":
    main()
