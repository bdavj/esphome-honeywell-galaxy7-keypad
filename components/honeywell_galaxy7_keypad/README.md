# Honeywell Galaxy 7 Keypad (ESPHome)

Custom ESPHome component that emulates a Honeywell Galaxy Mk7 keypad over the RS485 bus. It lets you:

- Drive the two-line keypad display from Home Assistant or ESPHome automations.
- Capture keypad keypresses (0–9, A, B, *, #, ESC, ENT) and optionally publish entered codes to a text sensor.
- Keep the backlight alive while you interact, then time it out automatically.
- Silence the keypad beep at boot (configurable) and track tamper frames.
- Pick which keypad address to impersonate (screen 1–4).

## Installation

Copy the `honeywell_galaxy7_keypad` folder into your ESPHome `custom_components` directory, or reference the repository as an external component:

```yaml
external_components:
  - source: github://bdavj/esphome-honeywell-galaxy7-keypad@main
    components: [honeywell_galaxy7_keypad]
```

## Hardware

- ESP32/ESP8266 with a 3.3V RS485 transceiver (e.g., MAX3485/75176).
- Wire RS485 A/B lines to keypad
- Bus runs at 9600 baud, 8-N-1.

## Basic configuration

The keypad attaches to a shared `galaxybus` instance (which itself wraps a UART). Entities (code text sensor, tamper, panel-online, beep switch, page number) are declared directly inside the `honeywell_galaxy7_keypad:` block — no separate top-level `text_sensor:` / `binary_sensor:` / `switch:` templates are required.

```yaml
external_components:
  - source: github://<your-username>/esphome-honeywell-galaxy7-keypad@main
    components: [galaxybus, honeywell_galaxy7_keypad]

uart:
  id: rs485_bus
  tx_pin: D0
  rx_pin: D1
  baud_rate: 9600
  parity: NONE
  stop_bits: 1

galaxybus:
  id: galaxy_bus
  uart_id: rs485_bus
  reply_timeout: 100ms
  inter_frame_gap: 10ms

honeywell_galaxy7_keypad:
  id: honeywell_galaxy7_keypad_1
  bus_id: galaxy_bus
  screen_number: 1                   # 1..4 -> 0x10/0x20/0x30/0x40 (or use address: 0x10)
  display_text: "ESP-HOME|Initializing"
  backlight_timeout: 15s
  prox_poll: true                    # enable PROX fob polling

  code_sensor:
    id: rs485_rx
    name: "Galaxy Keypad Code"
  # rx_text_sensor:                  # optional raw RX debug stream
  #   id: galaxy_rx_debug
  #   name: "Galaxy RX Debug"
  page_sensor:
    id: galaxy_page
    name: "Galaxy Keypad Page"
  tamper_sensor:
    id: galaxy_tamper
    name: "Galaxy Keypad Tamper"
  panel_online_sensor:
    id: galaxy_panel_online
    name: "Galaxy Panel Online"
  beep_switch:
    id: galaxy_keypad_beep
    name: "Galaxy Keypad Beep"

api:
  services:
    - service: set_galaxy_keypad_text
      variables:
        msg: string
      then:
        - lambda: 'id(honeywell_galaxy7_keypad_1).set_display_text(msg);'
```

### Configuration keys

- `bus_id` (**required**) — id of a `galaxybus:` instance on the same RS485 UART.
- `screen_number` (optional, default `1`) — logical keypad 1–4, maps to addresses 0x10/0x20/0x30/0x40.
- `address` (optional) — raw RS485 address; overrides `screen_number` when set.
- `display_text` (optional, default `"ESP-HOME|Initializing"`) — initial two-line text; use `|` to split lines.
- `backlight_timeout` (optional, default `15s`) — how long the backlight stays on after interaction.
- `prox_poll` (optional, default `false`) — enable polling of the matching PROX address for fob reads.
- `code_sensor` — text sensor that receives entered codes (and PROX tag hex when enabled).
- `rx_text_sensor` — optional raw RX debug text sensor.
- `page_sensor` — numeric sensor exposing the current keypad page.
- `tamper_sensor` — binary sensor for tamper (`F4 7F`) frames.
- `panel_online_sensor` — binary sensor that tracks whether the panel is polling us.
- `beep_switch` — switch to enable/disable the keypad beep.

### Updating the display from Home Assistant

```yaml
text_sensor:
  - platform: homeassistant
    id: keypad_text_from_ha
    entity_id: input_text.galaxy_keypad_text
    on_value:
      then:
        - lambda: 'id(honeywell_galaxy7_keypad_1).set_display_text(x.c_str());'
```

Use `A|B` to split the two lines. If no pipe is present, the second line is blank. Use `set_display_text_nobl()` inside a lambda if you want to change text without bumping the backlight.

### Key handling

- ESC clears the input buffer and refreshes the screen.
- ENT publishes the buffered digits/letters to the `code_sensor` (if configured), clears the buffer, and briefly clears the sensor again so repeated codes still fire.
- Other keys are masked with `*` on the second line while you type.
- The component automatically ACKs keys back to the keypad to stop repeats and tracks tamper (`F4 7F`).

### Optional automation helpers

```yaml
# Change beep mode at runtime (mode 0=off, 1=on, 3=periodic)
on_boot:
  priority: 800
  then:
    - lambda: |-
        id(honeywell_galaxy7_keypad_1).set_beep_enabled(false);     // silence by default
        id(honeywell_galaxy7_keypad_1).set_backlight_timeout(30000); // 30s backlight
        id(honeywell_galaxy7_keypad_1).enable_prox_polling(true);    // enable PROX polling at runtime
```

### PROX fobs

- When `prox_poll: true` is set, the component polls the matching PROX address (0x91/0x92/0x93/0x94) for the keypad and publishes detected fobs to the configured `code_sensor` as hex (e.g., `AA BB CC DD EE`). Empty replies (`0x10`) are ignored, and duplicates are debounced for 2s. A short beep is emitted on each new tag.

## RemoteIO (RIO)

This repo also includes a simple RemoteIO pair: a panel/master poller and a device/slave responder. The panel polls the RIO over RS485 and exposes inputs, outputs, uptime, and RSSI to Home Assistant.

Panel side (master on the bus):

```yaml
external_components:
  - source: github://<your-username>/esphome-honeywell-galaxy7-keypad@main
    components: [galaxybus, galaxy_rio_panel]

galaxybus:
  id: galaxy_bus
  uart_id: rs485_bus
  reply_timeout: 100ms
  inter_frame_gap: 10ms

galaxy_rio_panel:
  id: rio_garage
  bus_id: galaxy_bus
  device_id: 0x50
  poll_interval: 2s
  status_interval: 2s

  status:
    uptime_sensor:
      name: "RIO Uptime"
    rssi_sensor:
      name: "RIO RSSI"

  inputs:
    - name: "RIO In 1"
    - name: "RIO In 2"

  outputs:
    - name: "RIO Out 1"
    - name: "RIO Out 2"
```

RIO side (device on the bus):

```yaml
external_components:
  - source: github://<your-username>/esphome-honeywell-galaxy7-keypad@main
    components: [galaxy_rio_device]

galaxy_rio_device:
  id: rio_garage_dev
  uart_id: rs485
  device_id: 0x50
  inter_frame_gap: 10ms

  inputs:
    - id: KC868-A16-X01
      invert: true
      latch: true
    - id: KC868-A16-X02

  outputs:
    - id: out_y01
    - id: out_y02
```

Notes:
- `device_id` must match between panel and RIO device.
- `invert` flips a given input’s logic.
- `latch` reports an input as active if it went high at any time since the last poll, then clears after the poll reply.

## Troubleshooting

- Increase `logger:` level to `VERBOSE` to see raw frames and key/tamper messages.
- Confirm UART pins and 9600 baud. If sharing the bus with a real keypad, keep wiring short and termination sensible.
- If the panel stops answering, watch for `Panel timeout` logs and power-cycle the RS485 side to resync.

## Status

Work in progress but already handles display control, key capture/ACK, beep disable, backlight management, and tamper reporting.
