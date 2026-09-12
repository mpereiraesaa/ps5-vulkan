"""Bounded, unfiltered host-side klog capture for lifecycle diagnosis.

No console writes or lifecycle actions. This supplementary private diagnostic
does not replace ps5log/1 or certify GPU completion. Klog includes system-wide
messages; never publish the raw capture.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import socket
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--seconds", type=int, default=45)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 60:
        parser.error("seconds must be 1..60")
    root = Path(__file__).resolve().parents[1] / "private-captures/exit-klog"
    root.mkdir(parents=True, exist_ok=True)
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S%fZ")
    path = root / (stamp + ".log")
    digest = hashlib.sha256()
    count = 0
    limit = 8 * 1024 * 1024
    reason = "timeout"
    with socket.create_connection((args.host, 3232), timeout=3) as stream:
        stream.settimeout(.5)
        with path.open("xb") as output:
            print(json.dumps(dict(event="connected", path=str(path))), flush=True)
            deadline = time.monotonic() + args.seconds
            while time.monotonic() < deadline and count < limit:
                try:
                    chunk = stream.recv(min(65536, limit-count))
                except socket.timeout:
                    continue
                if not chunk:
                    reason = "eof"
                    break
                output.write(chunk)
                output.flush()
                digest.update(chunk)
                count += len(chunk)
            if count == limit:
                reason = "byte-limit"
    record = dict(path=str(path), bytes=count, sha256=digest.hexdigest(),
                  end_reason=reason, requested_seconds=args.seconds,
                  transport="klog-tcp", filtered=False, private=True)
    with path.with_suffix(".json").open("x") as metadata:
        json.dump(record, metadata, indent=2)
    print(json.dumps(record), flush=True)


if __name__ == "__main__":
    main()
