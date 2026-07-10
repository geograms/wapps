# APRS-over-BLE wire format (for ESP32 / other peers)

Connectionless **broadcast**: each station advertises its latest frame; all
nearby stations scan and receive it. No pairing/connections.

## Advertisement

Carried in **manufacturer-specific data**:

- Company ID: **0xFFFF** (reserved/test id). Must match on every device.
- Payload: a **compact frame** (NOT a full TNC2 line — legacy BLE advertising
  only fits ~31 bytes total). Fields are separated by the unit separator byte
  `0x1F`:

  ```
  <from> 0x1F <to> 0x1F <text>
  ```

  - `from` — sender callsign (e.g. `X16JK8`)
  - `to`   — routing target:
    - a callsign      → 1:1 message
    - `#GRP`          → group/bulletin message
    - `!`             → position; `text` = `lat,lon[,comment]` (decimal degrees)
    - empty           → area / geo-chat broadcast text (may start with `>>`)
  - `text` — the message / comment / position string

  Examples:
  - `X16JK8 0x1F CT1ABC 0x1F hello` — direct message
  - `X16JK8 0x1F #WX 0x1F Net at 8pm` — group bulletin
  - `X16JK8 0x1F ! 0x1F 38.7223,-9.1393,Aurora BLE` — position
  - `X16JK8 0x1F  0x1F >>anyone around?` — geo-chat

Keep the whole payload under ~27 bytes so it fits a legacy advertisement;
longer payloads are skipped by the host (until BLE-5 extended advertising is
used on capable hardware).

## Behaviour

- The identical compact frame also rides **Reticulum** (broadcast + directed
  datagrams, tagged `RET` on receipt) — the PRIMARY transport; BLE is the
  local off-grid path and APRS-IS is legacy/opt-in (licensed callsign only).
- Frames are deduped by content across Reticulum, BLE and APRS-IS, so a
  station on several transports shows each message once.
- With "Relay Bluetooth ↔ internet" on, a dual-link station bridges: BLE→internet
  is re-originated as APRS third-party traffic (`MYCALL>APRS,TCPIP*:}<TNC2>`,
  reconstructed from the compact fields); internet→BLE re-encodes the parsed
  APRS packet into the compact form above.
- BLE is shared across wapps by the host (one adapter, fan-out scan +
  multiplexed advertise); APRS does not own it exclusively.
