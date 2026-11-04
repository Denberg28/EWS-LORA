#!/usr/bin/env python3
"""Feed a synthetic NMEA fix through a 3.3 V USB-UART adapter for bench testing."""

import argparse
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB-UART adapter serial port")
    args = parser.parse_args()

    from serial import Serial

    body = b"GPGGA,120000,1230.0000,N,12330.0000,E,1,08,0.9,10.0,M,0.0,M,,"
    checksum = 0
    for byte in body:
        checksum ^= byte
    with Serial(args.port, 9600, write_timeout=2) as serial:
        try:
            while True:
                serial.write(b"$" + body + f"*{checksum:02X}\r\n".encode())
                time.sleep(1)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
