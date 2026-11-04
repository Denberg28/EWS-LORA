# EWS LORA

Offline SOS firmware for the LilyGO T3-S3 SX1262 915 MHz. Hold the Rover's SOS switch for two seconds. The Base saves and displays the alert and any external GNSS coordinates. A Rover can reach the Base directly or through one assigned Relay Node. A displayed ACK confirms durable Base receipt. Human response is a separate action.

## Roles and wiring

| Board | Firmware | USB configuration |
| --- | --- | --- |
| Staffed Base | `base` | ID 0, route 0. One Base per network |
| Fixed Relay Node | `node` | Unique node ID 1–8, route 0 |
| Portable SOS Rover | `rover` | Unique rover ID 1–8, route 0 for direct or the selected node ID 1–8 |

Use the **T3-S3 v1.2/v1.3 SX1262 915 MHz** variant for every board. Attach a matching antenna before powering any board. Use a normally open switch from GPIO21 to GND on each Rover. For configuration, temporarily connect a switch or jumper from GPIO21 to GND on the Base and Nodes. GPIO21 is distinct from the built-in BOOT switch. Optional GNSS uses 3.3 V logic NMEA at 9600 baud. Wire GNSS TX to Rover GPIO44 and common ground, power the module at its rated supply and leave its RX disconnected. [LilyGO board documentation](https://wiki.lilygo.cc/products/t3-series/t3-s3/)

The firmware uses 916.0 MHz, 10 dBm conducted power, SF8 and 125 kHz bandwidth. Verify locally authorized operating parameters, antenna and equipment before transmitting. Use an RF-contained bench if those conditions are unverified.

## Flash and configure

1. Download the desired role images and checksum manifest from [v0.2.0-rc.1](https://github.com/Denberg28/EWS-LORA/releases/tag/v0.2.0-rc.1). Check each downloaded file against the corresponding SHA-256 in `SHA256SUMS` with `sha256sum FILE` or `Get-FileHash FILE -Algorithm SHA256`.
2. Install Python 3.12 and run `python -m pip install esptool==4.8.1 pyserial==3.5`. Substitute your USB ports below. If upload mode is needed, hold BOOT while tapping RESET. Use the merged images at 0x0 for **first installation on dedicated boards**. A merged image overwrites NVS sectors that hold configuration and state.

   ```sh
   python -m esptool --chip esp32s3 --port COM5 write_flash 0x0 ews-lora-v0.2.0-rc.1-base.bin
   python -m esptool --chip esp32s3 --port COM6 write_flash 0x0 ews-lora-v0.2.0-rc.1-node.bin
   python -m esptool --chip esp32s3 --port COM7 write_flash 0x0 ews-lora-v0.2.0-rc.1-rover.bin
   ```

3. Download or clone this tagged source for `scripts/provision.py`. Generate one private key for the entire network. Hold GPIO21 low while resetting each blank board, then release it when the screen shows **USB CONFIG MODE**. Close other serial monitors, run the matching command, then reset with GPIO21 released.

   ```sh
   python scripts/provision.py ews-network.key --generate
   python scripts/provision.py ews-network.key --port COM5 --role base --id 0
   python scripts/provision.py ews-network.key --port COM6 --role node --id 1
   python scripts/provision.py ews-network.key --port COM7 --role rover --id 1 --via 1
   ```

   This example routes Rover 1 through Node 1. For a direct Rover, set `--via 0` or omit it. For another Node or Rover, flash the same role image and choose an unused ID within that role. Label each board with its role, ID and route.
4. On a controlled bench with Base and the selected Node powered, hold the Rover switch for 2 seconds and release. **WAIT ACK** means receipt has yet to be confirmed. **RX ACK SEEN** means the Base saved that attempt. Hold for 5 seconds to request CLEAR. Keep the boards powered until Rover returns to **READY**. Base cycles through active rover IDs and shows coordinates when available. The USB serial console at 115200 reports per-rover SOS and CLEAR.

The bench candidate supports up to eight distinct IDs per mobile or relay role. Every Rover uses a fixed route. Capacity, simultaneous alerts, interference tolerance and field range require measurement. Use the test candidate alongside established emergency communications. See [architecture](docs/ARCHITECTURE.md) and [validation procedure](docs/TESTING.md).

## Build, update, and roll back

```sh
python -m pip install -r requirements-build.txt
pio run -e base -e node -e rover
./scripts/check.sh
./scripts/package.sh
```

The host tests use Linux, g++ and libssl-dev. Packaging requires a clean tracked checkout and generates merged images, app-only images and the checksum manifest.

Finish active incidents before maintenance. For a compatible **same-role app update** on an already installed board, flash the corresponding `-app.bin` at **0x10000** to preserve NVS. Avoid duplicate IDs and preserve active state when restoring compatible images. Replacing or erasing a Rover resets its replay counter. To reuse that ID, retire the old configuration and rekey the entire network from clean state. Clear state only after resolving active incidents. An unused ID can be assigned without resetting the existing fleet.

Wire v2 roles require a coordinated upgrade. Wire v1 remains available from [v0.1.0-rc.1](https://github.com/Denberg28/EWS-LORA/releases/tag/v0.1.0-rc.1). For rollback, recover any outstanding alerts with the operator, then restore the matching legacy beacon and receiver images and re-pair those boards if their previous configuration was erased. Re-run the acceptance test after any change.
