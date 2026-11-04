# EWS LORA

Offline SOS firmware for the LilyGO T3-S3 SX1262 915 MHz. Tap the Rover BOOT button, release, then press and hold for two seconds. The Base saves and displays the named alert and any external GNSS coordinates. Other Rovers can display authenticated alerts they hear. A Rover can reach the Base directly or through one assigned Relay Node. A displayed ACK confirms durable Base receipt. Human response is a separate action.

## Roles and wiring

| Board | Firmware | USB configuration |
| --- | --- | --- |
| Staffed Base | `base` | ID 0, route 0. One Base per network |
| Fixed Relay Node | `node` | Unique node ID 1–8, route 0 |
| Portable SOS Rover | `rover` | Unique rover ID 1–8, route 0 for direct or the selected node ID 1–8 |

Use the **T3-S3 v1.2/v1.3 SX1262 915 MHz** variant for every board. Attach a matching antenna before powering any board. Use the onboard **BOOT** button for SOS and CLEAR only after the Rover starts. Holding BOOT during reset enters the ESP32-S3 bootloader. For USB provisioning or renaming, temporarily hold **GPIO21 to GND** during reset. Release the service jumper after startup. For two-color indication, use an external common-cathode red/green LED, with common to GND, red anode through a 330 Ω resistor to GPIO10 and green anode through a separate 330 Ω resistor to GPIO43. The onboard GPIO37 LED mirrors the active SOS flash and gives a short pulse for faults. Leave GPIO43 free of other peripherals when using the green LED. Optional GNSS uses 3.3 V logic NMEA at 9600 baud. Wire GNSS TX to Rover GPIO44 and common ground, power the module at its rated supply and leave its RX disconnected. [LilyGO board documentation](https://wiki.lilygo.cc/products/t3-series/t3-s3/)

The firmware uses 916.0 MHz, 10 dBm conducted power, SF8 and 125 kHz bandwidth. Verify locally authorized operating parameters, antenna and equipment before transmitting. Use an RF-contained bench if those conditions are unverified.

## Flash and configure

1. Download the desired role images and checksum manifest from [v0.3.0-rc.1](https://github.com/Denberg28/EWS-LORA/releases/tag/v0.3.0-rc.1). Check each downloaded file against the corresponding SHA-256 in `SHA256SUMS` with `sha256sum FILE` or `Get-FileHash FILE -Algorithm SHA256`.
2. Install Python 3.12 and run `python -m pip install esptool==4.8.1 pyserial==3.5`. Substitute your USB ports below. If upload mode is needed, hold BOOT while tapping RESET. Use the merged images at 0x0 for **first installation on dedicated boards**. A merged image overwrites NVS sectors that hold configuration and state.

   ```sh
   python -m esptool --chip esp32s3 --port COM5 write_flash 0x0 ews-lora-v0.3.0-rc.1-base.bin
   python -m esptool --chip esp32s3 --port COM6 write_flash 0x0 ews-lora-v0.3.0-rc.1-node.bin
   python -m esptool --chip esp32s3 --port COM7 write_flash 0x0 ews-lora-v0.3.0-rc.1-rover.bin
   ```

3. Download or clone this tagged source for `scripts/provision.py`. Generate one private key for the entire network. Hold GPIO21 low while resetting each blank board, then release it when the screen shows **USB CONFIG MODE**. Close other serial monitors, run the matching command, then reset with GPIO21 released.

   ```sh
   python scripts/provision.py ews-network.key --generate
   python scripts/provision.py ews-network.key --port COM5 --role base --id 0
   python scripts/provision.py ews-network.key --port COM6 --role node --id 1
   python scripts/provision.py ews-network.key --port COM7 --role rover --id 1 --via 1
   ```

   This example routes Rover 1 through Node 1. For a direct Rover, set `--via 0` or omit it. For another Node or Rover, flash the same role image and choose an unused ID within that role. Label each board with its role, ID and route.
4. To name an idle Rover, reset it with GPIO21 low again, release the jumper and run `python scripts/provision.py --port COM7 --name ALPHA`. Reset normally. Names use 1–8 letters, digits or hyphens and are carried in authenticated alerts. Unnamed units display their Rover ID.
5. On the bench, tap BOOT for less than 450 ms, release, then press again within one second and hold for **two seconds**. The Rover latches SOS and flashes red quickly. Its OLED shows its name, coordinates when available and whether the Base has acknowledged. While SOS is active, use the same gesture with a **three-second** hold to cancel. **CANCEL PENDING** and red flashing continue until the Base saves CLEAR and returns a matching ACK. Then the Rover shows **SOS CLEARED** and the external green LED stays on while the unit is ready.
6. Base and peer Rovers show active alert names and coordinates, or **NO GPS FIX**. Peer notices are passive RF reception. A peer clears its record only if it hears the Base's confirmed CLEAR directly or via the selected Node. A peer that missed confirmation can retain the alert. Confirm the incident state at the Base.

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

Wire v3 requires a coordinated Base, Node and Rover upgrade. For existing v0.2 boards, use the same-role app-only images to retain the network key, IDs, Rover sequence and previous Base records. The new firmware reads v2 stored Base records and rewrites them in v3 format as updates arrive. Rolling back after that requires clean reprovisioning of the whole network with a fresh key, or restoring a coordinated offline backup. Resolve outstanding incidents first and re-run bench acceptance after any change. [Previous v0.2 release](https://github.com/Denberg28/EWS-LORA/releases/tag/v0.2.0-rc.1)
