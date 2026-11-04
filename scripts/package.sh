#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
version=$(cat VERSION)
[[ $version =~ ^v[0-9]+\.[0-9]+\.[0-9]+-rc\.[0-9]+$ ]]
sha=$(git rev-parse HEAD)
git diff --quiet HEAD -- .
core_dir=${PLATFORMIO_CORE_DIR:-"$HOME/.platformio"}
esptool="$core_dir/packages/tool-esptoolpy/esptool.py"
boot_app="$core_dir/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin"
mkdir -p dist

for role in base node rover; do
  build=".pio/build/$role"
  python3 "$esptool" --chip esp32s3 image_info "$build/firmware.bin"
  cp "$build/firmware.bin" "dist/ews-lora-$version-$role-app.bin"
  python3 "$esptool" --chip esp32s3 merge_bin --flash_mode dio --flash_size 4MB \
    --flash_freq 80m -o "dist/ews-lora-$version-$role.bin" \
    0x0 "$build/bootloader.bin" 0x8000 "$build/partitions.bin" \
    0xe000 "$boot_app" 0x10000 "$build/firmware.bin"
  test -s "dist/ews-lora-$version-$role.bin"
  python3 "$esptool" --chip esp32s3 image_info "dist/ews-lora-$version-$role.bin"
done

{
  printf 'Version: %s\nCommit: %s\nTier: HIL test candidate\n' "$version" "$sha"
  pio --version
  pio pkg list
  cat platformio.ini
} > dist/BUILD.txt

(
  cd dist
  sha256sum ews-lora-"$version"-{base,node,rover}{,-app}.bin BUILD.txt > SHA256SUMS
  sha256sum --check SHA256SUMS
)
