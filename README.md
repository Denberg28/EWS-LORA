# EWS LORA

An offline, two-board SOS link for a controlled rescue exercise. Hold the beacon's SOS button for **2 seconds**. A staffed receiver displays the alert and any coordinates supplied by an external GNSS module. The beacon displays **RX ACK SEEN** when the paired receiver has saved the alert. Receipt and dispatch are separate events. Use the test candidate alongside established communications and rescue procedures.

## Hardware

| Item | Connection or requirement |
| --- | --- |
| Beacon and receiver | Two LilyGO T3-S3 v1.2/v1.3, **SX1262 915 MHz** variant, each with a matching antenna attached before power-up |
| SOS switch | Normally open momentary switch from GPIO21 to GND on beacon. Use a large tactile button and short leads. On the receiver temporarily connect the same switch or jumper for key provisioning |
| Optional GNSS | GNSS TX to beacon GPIO44, common GND, 9600 baud NMEA, 3.3 V logic. Power the module at its rated supply. Leave module RX disconnected |
| Board wiring | SX1262 SPI SCK 5, MISO 3, MOSI 6, CS 7, RESET 8, BUSY 34, DIO1 33. OLED SDA 18, SCL 17, I²C 0x3C. LED GPIO37 |
| Power | Start with USB power on both boards. For a portable test, use a suitable board supply and measure endurance under load |

Keep both boards on the same protocol, RF settings and pair key. The SX127x and 2.4 GHz variants and other LoRa stacks are outside this profile. The candidate uses 916.0 MHz, 10 dBm conducted power, SF8 and 125 kHz bandwidth. Verify applicable local limits, approval and antenna configuration before transmitting. Use an RF-contained bench if those conditions are unverified. [LilyGO hardware documentation](https://wiki.lilygo.cc/products/t3-series/t3-s3/)

## Install and pair

1. Download both matching candidate images and `SHA256SUMS` from [Releases](https://github.com/Denberg28/EWS-LORA/releases). Download `BUILD.txt` too and verify all hashes with `sha256sum -c SHA256SUMS` on Linux or compare with `Get-FileHash -Algorithm SHA256` on Windows.
2. Install Python 3.12 and run `python -m pip install pyserial==3.5 esptool==4.8.1`. Connect one board at a time. Substitute its port and role in the command below. The image includes the bootloader, partition table and application. If required, hold BOOT while tapping RESET to enter upload mode. Use RESET alone for a normal reboot.

   ```sh
   python -m esptool --chip esp32s3 --port COM5 write_flash 0x0 ews-lora-v0.1.0-rc.1-beacon.bin
   python -m esptool --chip esp32s3 --port COM6 write_flash 0x0 ews-lora-v0.1.0-rc.1-receiver.bin
   ```

3. Generate **one** key per pair. While each blank board resets, hold its GPIO21 switch down (distinct from BOOT), then release when the screen shows **USB KEY MODE**. Run the port command for each board with the same private key file. Close any open serial monitor first. Then reset each board with GPIO21 released.

   ```sh
   python scripts/provision.py rescue-pair.key --generate
   python scripts/provision.py rescue-pair.key --port COM5
   python scripts/provision.py rescue-pair.key --port COM6
   ```

4. Check **READY** on beacon and **LISTENING** on receiver. Hold SOS for 2 seconds and release. A blinking LED on beacon means an unacknowledged alert. Solid means an ACK for the latest completed attempt. The receiver LED blinks during a saved SOS. A short LED pulse each second signals a fault. OLED and optional USB logs at 115200 provide the detail.
5. Hold beacon SOS for 5 seconds to request clear. Keep both boards powered until beacon returns to **READY** and receiver shows **REMOTE CLEAR**. During **CLEAR PENDING**, a new 2-second hold starts a new SOS. Verify incident closure separately with the receiver operator.

An SOS works without a fix and can add coordinates later. Coordinates are last reported. The receiver displays a saved SOS after reboot. Unacknowledged alerts retry about every 20 seconds. A rapid clear or reactivation may wait for the next transmission slot. No SIM, internet or LoRaWAN gateway is required.

## Build and maintain

```sh
python -m venv .venv
# Activate .venv for your shell, then:
python -m pip install -r requirements-build.txt
pio run -e beacon -e receiver
pio run -e beacon -t upload --upload-port COM5
pio run -e receiver -t upload --upload-port COM6
```

On Ubuntu with g++ and libssl-dev, run `./scripts/check.sh`. Build both roles before running `./scripts/package.sh` from a clean tracked checkout. The packaging command uses PlatformIO's installed esptool, emits merged binaries and verifies hashes. See the [design](docs/ARCHITECTURE.md), [validation procedure](docs/TESTING.md) and [changelog](CHANGELOG.md).

Resolve active incidents before maintenance. Flash the same role on each board and keep NVS intact for compatible upgrades and rollbacks. This first candidate has no prior working build. For rollback to a board's previous application, restore a flash backup made before installing EWS LORA. If wiping or replacing either board, retire the old pair, erase **both** boards with `python -m esptool --chip esp32s3 --port PORT erase_flash`, reflash both images and pair with a fresh key. Erasing deletes saved alert state and keys. Recheck reception after any upgrade, rollback or re-pairing.
