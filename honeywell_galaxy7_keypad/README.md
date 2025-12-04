```yaml
# Example configuration with display text that comes from Home Assistant
honeywell_galaxy7_keypad:
  id: galaxy_keypad
  display_text: "ESP-HOME|Initializing"  # defaults to this

uart:
  tx_pin: D0
  rx_pin: D1
  baud_rate: 9600

# Call this from Home Assistant (or any ESPHome automation) to change the two-line display.
api:
  services:
    - service: set_galaxy_keypad_text
      variables:
        msg: string
      then:
        - lambda: |-
            id(galaxy_keypad).set_display_text(msg);

# Optional: push a Home Assistant input_text straight to the keypad display.
text_sensor:
  - platform: homeassistant
    entity_id: input_text.galaxy_keypad_text
    id: keypad_text_from_ha
    on_value:
      then:
        - lambda: |-
            id(galaxy_keypad).set_display_text(x.c_str());
```

- `display_text` uses `|` to split line 1 and line 2 (e.g. `Ready|System`). If no pipe is present the second line is left blank.
- The component caches the last text and resends it periodically to the keypad instead of the old HELLO/TEST placeholder.
