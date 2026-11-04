#!/usr/bin/env python3
"""Create a local key or transfer it over USB to a blank board in USB KEY MODE."""

import argparse
import os
from pathlib import Path
import re
import secrets
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("key", type=Path, help="path to private pair key file")
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--generate", action="store_true")
    group.add_argument("--port", help="USB serial port, e.g. COM5 or /dev/ttyACM0")
    args = parser.parse_args()

    if args.generate:
        with os.fdopen(os.open(args.key, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600), "w") as f:
            f.write(secrets.token_hex(32) + "\n")
        print("Pair key created. Keep it private.")
        return

    from serial import Serial

    key = args.key.read_bytes().strip()
    if not re.fullmatch(rb"[a-f0-9]{64}", key) or key == b"0" * 64:
        parser.error("invalid pair key file")
    with Serial(args.port, 115200, timeout=0.25, write_timeout=2) as port:
        time.sleep(0.3)
        port.reset_input_buffer()
        port.write(b"KEY " + key + b"\n")
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if port.readline().strip() == b"KEY STORED - RESET BOARD":
                print("Key stored. Reset the board with the button released.")
                return
    raise SystemExit("No confirmation. Check the port and USB KEY MODE before retrying.")


if __name__ == "__main__":
    main()
