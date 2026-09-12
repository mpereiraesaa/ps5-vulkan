"""Send only project-specific lifecycle helpers to the owned console."""
import argparse
from pathlib import Path
import socket

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("action", choices=("status", "launch", "close"))
    parser.add_argument("--host", required=True)
    args = parser.parse_args()
    path = Path(__file__).resolve().parents[1] / "build/control" / (args.action + ".elf")
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise SystemExit("not ELF")
    with socket.create_connection((args.host, 9021), timeout=5) as sock:
        sock.settimeout(15)
        sock.sendall(data)
        sock.shutdown(socket.SHUT_WR)
        while True:
            try:
                data = sock.recv(4096)
            except socket.timeout:
                raise SystemExit("Helper response timed out; inspect status before retrying")
            if not data:
                break
            print(data.decode("utf-8", "replace"), end="", flush=True)

if __name__ == "__main__":
    main()
