# EWS LORA fleet candidate design

Baseline: v0.2.0-rc.1 at ee7d7d8ba2b85b2ee5624606c0cfcc853fd51c02, a software-verified three-role candidate. It has no physical acceptance evidence. This update retains that tagged baseline and introduces wire v3 for one staffed Base, up to eight numbered SOS Rovers and up to eight numbered Relay Nodes. Roles are provisioned locally. A Rover selects a direct link or exactly one Node. There is no automatic failover, multi-hop mesh, internet gateway, vehicle control, or public rescue-service integration.

| Event | Rover | Node | Base |
| --- | --- | --- | --- |
| Tap and hold 2 s | Persist SOS and transmit at the next eligible slot even without GPS | Forward only for its own node ID | Persist by rover ID before ACK |
| Tap and hold 3 s during SOS | Persist CLEAR and retry until ACK | Same path as SOS | Clear only the named rover and persist replay state |
| No current ACK | Show WAIT ACK and retry | Expire the transaction window | Retain last saved event |
| New or expired fix | Update on next report; fixes expire after 10 s | Forward validated report | Show last reported coordinates or NO GPS FIX |
| Reboot | Resume active SOS/CLEAR and reserve a higher sequence block | Listen again and await Rover retry | Restore per-rover records and mark them restored |
| I/O fault | Retain alert and retry radio setup | Retry radio setup | Retain saved events and retry radio setup |
| Storage fault | Show STORE ERROR and disable RF | Disable RF if config storage fails | Fail closed and avoid ACKing failed writes |

A fresh tap-and-hold for two seconds during pending CLEAR starts a new SOS. The debounced gesture requires a short tap, then a second press within one second. Each gesture fires once per release. GPIO0 held at reset enters the ROM bootloader. An already-held runtime input must be released before arming. GPIO21 is reserved for USB configuration. Local users control SOS and clear. Only the Base issues an ACK. The firmware has no actuators or motion authority. The loop task uses an 8-second watchdog, bounded RadioLib packet TX, and debounced button handling.

## Routing and timing

- Direct Rover sends route 0. Base accepts the unforwarded report and returns an unforwarded ACK. Routed Rover sends a nonzero route. Only the matching Node forwards that request with the hop bit set. Base accepts the routed report only with the hop bit set. The same Node then forwards the corresponding Base ACK with the hop bit set. Peers accept authenticated SOS overheard on either hop and remove it only after a newer confirmed CLEAR ACK. Passive peer reception gives no delivery guarantee. A peer that misses confirmation retains its record, labeled as last reported or restored. Rover accepts an ACK only on the configured path.
- Each ACK must match report kind, Rover ID, route, report sequence and a fresh 64-bit per-transmission nonce. Each Node holds one in-flight transaction for up to 4 seconds. It forwards the matching ACK once and rejects already-forwarded frames. A route contains at most one relay in each direction.
- Direct retry slots are 20–28 seconds, routed retry slots 40–48 seconds. After a successful ACK, reporting slots are respectively 30–38 and 50–58 seconds. Independent random jitter reduces repeated collisions. Local mode changes respect the prior transmission slot. The Base limits ACK starts to one per 18 seconds and each Node limits uplink transaction starts to one per 38 seconds. A busy Base can save a report without ACK. Simultaneous alerts compete for airtime. ID limits describe storage and addressing capacity. Per-route service capacity and multi-alert latency require measurement.
- RF parameters are 916.0 MHz by default, 10 dBm conducted, SF8, 125 kHz BW, CR 4/5, preamble 8, explicit header, CRC, private sync word 0x12. Limits are design defaults during uptime, and resetting a board clears its airtime timers. Verify jurisdictional and antenna requirements before RF operation.

## Wire and persistence

52-byte datagram. Bytes 0–1 magic EW, 2 version 3, 3 kind (SOS 1, CLEAR 2, SOS ACK 3, CLEAR ACK 4), 4 fix flag, 5–8 nonzero monotonic report sequence, 9–12 signed latitude and 13–16 signed longitude in degrees × 100,000, 17 Rover ID, 18 route ID, 19–26 uppercase Rover name (zero-padded), 27 hop bit, 28–35 nonzero nonce, and 36–51 first 16 bytes of HMAC-SHA256 over bytes 0–35. Multi-byte values are little-endian. Coordinates are zero when absent. CLEAR may carry the last incident fix captured before cancellation. ACKs carry no coordinates.

Network devices share one random 256-bit key and trust other provisioned devices. Anyone with that key can impersonate network roles. Use distinct keys for distinct networks and protect access to boards, keys and USB logs. RF coordinates are authenticated and readable. USB configuration requires a physical boot condition and a blank configuration. Firmware has no key export and does not enable flash encryption. Address uniqueness is an installation responsibility.

The Rover reserves sequences in durable 256-entry blocks. The Base persists each Rover's latest canonical report, and peer Rovers persist alerts and confirmed clear tombstones they hear. Each receiver rejects older sequences and conflicting duplicates. A repeat of the current report can receive a new ACK for the current attempt without rewriting flash. Freshly heard RF and wall-clock age are separate concerns. The Base shows coordinates as last reported. A replay or RF interference can consume airtime without changing an older persisted state. Physical backup communications and measured coverage remain required.
