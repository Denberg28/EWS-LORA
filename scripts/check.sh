#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
binary=$(mktemp)
trap 'rm -f "$binary"' EXIT
"${CXX:-g++}" -std=c++11 -Wall -Wextra -Werror -pedantic -g \
  -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -Iinclude \
  src/protocol.cpp tests/protocol_test.cpp -lcrypto -o "$binary"
"$binary"
python3 -m py_compile scripts/provision.py scripts/nmea_fixture.py
