# Changelog

## v0.5.0-rc.1

- Collect same-attempt direct RF readings from the Base and relay nodes when an SOS has no GPS fix.
- Add a calibrated RSSI search-area estimate, anchor CSV import and a no-GPS demo scenario.
- Keep RF estimates separate from GPS tracks and incident acknowledgement.


## v0.4.0-rc.1

- Add a read-only Windows x64 monitor with rover contact, SOS state, relay heartbeats, offline movement trails, demo mode and GeoJSON export.
- Add jittered authenticated rover positions and relay heartbeats, separate from durable SOS/CLEAR state.
- Add a versioned Base USB snapshot feed and preserve v0.3 incident interoperability.
- Desktop and firmware are controlled bench candidates pending physical fleet acceptance.


## v0.3.0-rc.1

- Adds debounced BOOT tap-and-hold SOS and cancellation, external red/green outputs, and named GPS alert screens.
- Adds passive peer alerts, durable confirmed clear handling and typed Base acknowledgements.
- Introduces wire v3 and reads stored Base records from v2.

## v0.2.0-rc.1

- Adds separate Base, Relay Node and Rover images with runtime IDs and fixed direct or single-node routes.
- Stores independent Base alert records for eight Rovers and rotates active SOS records on the OLED.
- Adds authenticated one-hop forwarding, retry jitter and USB fleet provisioning. Uses wire v2.
- Packages merged first-install and same-role app-only update images for all three roles.

## v0.1.0-rc.1

- Initial test candidate for a single LilyGO T3-S3 SX1262 beacon and staffed receiver.
- Offline SOS and clear, optional GNSS coordinates, authenticated acknowledgement, retry and reboot recovery.
- Adds private USB pair provisioning, pinned PlatformIO builds, candidate packaging and a bench validation procedure.
- Physical RF, GNSS, button, OLED, power, coverage and endurance checks must pass before stable release.
