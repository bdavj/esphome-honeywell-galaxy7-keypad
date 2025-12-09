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
  - source: github://<your-username>/esphome-honeywell-galaxy7-keypad@main
    components: [honeywell_galaxy7_keypad]
```

## Hardware

- ESP32/ESP8266 with a 3.3V RS485 transceiver (e.g., MAX3485/75176).
- Wire UART TX/RX to the transceiver DI/RO pins, tie DE/RE together and drive from a GPIO (or tie high for always-on if you share the bus carefully).
- Bus runs at 9600 baud, 8-N-1.

## Basic configuration

```yaml
external_components:
  - source: github://<your-username>/esphome-honeywell-galaxy7-keypad@main
    components: [honeywell_galaxy7_keypad]

uart:
  id: rs485_bus
  tx_pin: D0
  rx_pin: D1
  baud_rate: 9600
  parity: NONE
  stop_bits: 1

text_sensor:
  - platform: template
    id: rs485_rx
    name: "RS485 RX"

honeywell_galaxy7_keypad:
  id: honeywell_galaxy7_keypad_1
  uart_id: rs485_bus
  rs485_rx_id: rs485_rx                     # Publishes buffered digits when ENT is pressed
  screen_number: 2                          # 1–4 -> keypad IDs 0x10/0x20/0x30/0x40 (default: 2)
  display_text: "ESP-HOME|Initializing"     # Two lines split by '|'
  backlight_timeout: 15s                    # How long to keep the backlight on after activity

# Enable Home Assistant API and expose services to set the display
api:
  encryption:
    key: "Key"
  services:
    - service: set_galaxy_keypad_text
      variables:
        msg: string
      then:
        - lambda: |-
            id(honeywell_galaxy7_keypad_1).set_display_text(msg);
    - service: set_galaxy_keypad_text_no_backlight
      variables:
        msg: string
      then:
        - lambda: |-
            id(honeywell_galaxy7_keypad_1).set_display_text_nobl(msg);

# Optional: toggle keypad beep from HA
switch:
  - platform: template
    id: galaxy_keypad_beep
    name: "Galaxy Keypad Beep"
    lambda: |-
      return id(galaxy_beep_enabled);  // report current state
    turn_on_action:
      - lambda: |-
          id(galaxy_beep_enabled) = true;
          id(honeywell_galaxy7_keypad_1).set_beep_enabled(true, 8, 4);
    turn_off_action:
      - lambda: |-
          id(galaxy_beep_enabled) = false;
          id(honeywell_galaxy7_keypad_1).set_beep_enabled(false, 8, 4);

binary_sensor:
  - platform: status
    name: "Galaxy Keypad ESP Status"
    id: galaxy_keypad_status
  - platform: template
    name: "Galaxy Panel Online"
    id: galaxy_panel_online
    lambda: |-
      auto *kp = id(honeywell_galaxy7_keypad_1);
      if (!kp) return {};
      return kp->is_panel_online();

button:
  - platform: template
    name: "Keypad Refresh"
    on_press:
      - lambda: 'id(honeywell_galaxy7_keypad_1).set_display_text("Ready|System");'
```

- Beep switch toggles `set_beep_enabled()` and returns the cached `galaxy_beep_enabled` state (add a `globals:` entry if you want it persisted).
- `Galaxy Panel Online` mirrors `is_panel_online()` so you can drive HA cards/automations.
- `Keypad Refresh` gives you a one-tap way to push a known screen.

### Updating the display from Home Assistant

```yaml
text_sensor:
  - platform: homeassistant
    id: keypad_text_from_ha
    entity_id: input_text.galaxy_keypad_text
    on_value:
      then:
        - lambda: 'id(galaxy_keypad).set_display_text(x.c_str());'
```

Use `A|B` to split the two lines. If no pipe is present, the second line is blank. Use `set_display_text_nobl()` inside a lambda if you want to change text without bumping the backlight.

### Key handling

- ESC clears the input buffer and refreshes the screen.
- ENT publishes the buffered digits/letters to `rs485_rx_id` (if provided), clears the buffer, and briefly clears the sensor again so repeated codes still fire.
- Other keys are masked with `*` on the second line while you type.
- The component automatically ACKs keys back to the keypad to stop repeats and tracks tamper (`F4 7F`).

### Optional automation helpers

```yaml
# Change beep mode at runtime (mode 0=off, 1=on, 3=periodic)
on_boot:
  priority: 800
  then:
    - lambda: |-
        id(galaxy_keypad).set_beep_enabled(false);     // silence by default
        id(galaxy_keypad).set_backlight_timeout(30000); // 30s backlight
```

## Troubleshooting

- Increase `logger:` level to `VERBOSE` to see raw frames and key/tamper messages.
- Confirm UART pins and 9600 baud. If sharing the bus with a real keypad, keep wiring short and termination sensible.
- If the panel stops answering, watch for `Panel timeout` logs and power-cycle the RS485 side to resync.

## Status

Work in progress but already handles display control, key capture/ACK, beep disable, backlight management, and tamper reporting.
