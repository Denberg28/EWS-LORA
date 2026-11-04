#!/usr/bin/env python3
"""Create a network key or configure a blank board in USB CONFIG MODE."""

import argparse
import os
from pathlib import Path
import re
import secrets
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("key", type=Path, help="path to private network key file")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--generate", action="store_true")
    group.add_argument("--port", help="USB serial port, e.g. COM5 or /dev/ttyACM0")
    parser.add_argument("--role", choices=("base", "node", "rover"))
    parser.add_argument("--id", type=int, choices=range(9), default=0, help="base 0, node or rover 1–8")
    parser.add_argument("--via", type=int, choices=range(9), default=0, help="rover route: direct 0, node 1–8")
    args = parser.parse_args()
    if args.port and not args.role:
        parser.error("--port requires --role")
    if args.role == "base" and (args.id or args.via):
        parser.error("base requires ID 0 and route 0")
    if args.role in ("node", "rover") and not args.id:
        parser.error("node and rover IDs are 1–8")
    if args.via and args.role != "rover":
        parser.error("--via applies only to a rover")

    if args.generate:
        with os.fdopen(os.open(args.key, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "w") as f:
            f.write(secrets.token_hex(32) + "\n")
        print("Network key created. Keep it private.")
        return

    from serial import Serial

    key = args.key.read_bytes().strip()
    if not re.fullmatch(rb"[a-f0-9]{64}", key) or key == b"0" * 64:
        parser.error("invalid network key file")
    with Serial(args.port, 115200, timeout=0.25, write_timeout=2) as port:
        time.sleep(0.3)
        port.reset_input_buffer()
        port.write(f"CFG {args.role[0].upper()} {args.id} {args.via} ".encode("ascii") + key + b"\n")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            response = port.readline().strip()
            if response == b"INVALID CONFIG":
                raise SystemExit("Board rejected the configuration. Check its firmware role and ID.")
            if response == b"CONFIG STORED - RESET BOARD":
                print("Configuration stored. Reset the board with the button released.")
                return
    raise SystemExit("No confirmation. Check the port and USB CONFIG MODE before retrying.")


if __name__ == "__main__":
    main()
