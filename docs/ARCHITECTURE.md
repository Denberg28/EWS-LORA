# EWS LORA 0.1 design

Baseline: public Denberg28/EWS-LORA, default branch main, empty before the initial AGENTS.md commit 305c39a0b28ecc382a930654983769278a8fcbb5. There is no prior firmware, package identity, signing key, or known-good device image.

## Scope

Two LilyGO T3-S3 ESP32-S3 boards with SX1262 915 MHz hardware. One portable beacon and one staffed receiver form a private, direct LoRa pair. A tactile switch activates the beacon. An optional external 3.3 V logic NMEA GNSS receiver supplies coordinates. The OLED and onboard LED work offline. There is no phone, cellular, cloud, GPS-by-LoRa triangulation, relay, mesh, multi-beacon dispatch, or integration with public rescue services. The receiver should be staffed for the entire controlled test.

## Operation and recovery

| Event | Beacon | Receiver |
| --- | --- | --- |
| Hold 2 s | Latch SOS in flash, send now even with no fix | Authenticate and persist alert before sending ACK |
| No ACK | Show WAIT ACK, repeat every 20 s | Continue listening |
| ACK for current transmission | Show RX ACK SEEN, send every 30 s while ACKs continue | Keep alert latched. Show latest reported coordinates or NO GPS FIX |
| ACK goes stale | Show WAIT ACK after 75 s without an ACK. A missed 3 s response window already restores 20 s retries | Keep last stored report |
| Fix becomes valid or stale | Update at next report. Expire fixes older than 10 s | Display the state sent in the report |
| Hold 5 s on an active beacon | Persist CLEAR and retry every 20 s until ACK | Persist and show REMOTE CLEAR. Operator verifies incident separately |
| Reboot | Resume pending SOS or CLEAR from NVS with a new sequence | Restore last stored record and mark RESTORED until a newer report |
| Radio init/I/O fault | Show radio fault and retry setup every 2 s, retain stored alert | Same, retain stored record |
| Storage fault | Show STORE ERROR and inhibit radio. Recover after fault repair/reboot | Same. Never ACK a write that failed |

Local users control activation and clearing. A 2 s hold during pending CLEAR starts a new SOS. A local mode change respects the 20 s minimum between transmissions during uptime, and the receiver applies an 18 s ACK rate limit during uptime. The paired receiver can acknowledge only. There are no actuators, remote commands, handover, or motion outputs. Hold-at-boot is reserved for provisioning a blank board. The button must be released after startup before it can trigger an action. An 8 s ESP task watchdog reboots a stalled main loop. RadioLib's synchronous TX is bounded by its packet timeout. Each TX opens a 3 s ACK window; signed ACKs must match the exact 64-bit transmission nonce and 32-bit sequence. An ACK confirms durable receiver receipt. Dispatch and human response require separate action.

## Interfaces

- P2P LoRa, configurable center frequency, SF8, 125 kHz BW, CR 4/5, preamble 8, explicit header, PHY CRC, private sync word 0x12. The candidate profile is 916.0 MHz and 10 dBm conducted. Verify authorized operating parameters before transmitting. One antenna and one receiver per pair.
- 41-byte version-1 datagram, little-endian. Bytes 0–1 magic EW, 2 version, 3 type (SOS=1, CLEAR=2, ACK=3), 4 fix flag (0/1), 5–8 nonzero monotonic report sequence, 9–12 signed latitude and 13–16 signed longitude in degrees × 100,000 (zero when absent), 17–24 nonzero fresh random nonce, 25–40 first 16 bytes of HMAC-SHA256 over bytes 0–24. ACK and CLEAR carry no coordinates. ACK copies report sequence and nonce.
- Each pair uses its own random 256-bit key, stored in NVS via a physically enabled USB-only provisioning session. Payload coordinates are readable to RF listeners. There is no key export, RF provisioning, or encryption. Protect access to hardware, local logs and the provisioning key. ESP32 flash encryption is outside this MVP.
- The beacon reserves sequence values in blocks of 256 via NVS before use and skips unused values after reset. Its active mode and reservation high-water mark live in one record. One report version is reused for identical retransmissions. Changes in fix availability or rounded coordinates get a new sequence. The receiver durably stores the most recent changed report before ACK. It rejects older sequences and conflicting payloads at the current sequence. Authenticated exact duplicates may receive ACK at a bounded rate, but never renew stored state or operator freshness. Power loss between save and ACK is handled by retransmission.
- Coordinates are last reported and have no authenticated wall-clock timestamp. The receiver never implies a live fix or range guarantee. Duplicate RF replay can consume airtime. Interference, jamming, physical key extraction, traffic observation and lost receivers require operational controls. The USB receiver log contains coordinates and is intended for local test use.
