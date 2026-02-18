#!/usr/bin/env python3
from __future__ import annotations

import sys
import time
import select
import argparse
try:
    import serial  # type: ignore
except ImportError as exc:
    sys.exit(
        "Missing dependency: pyserial. "
        "Install with `pip install pyserial` (or add to your env) and rerun."
    )

# Some environments accidentally install the `serial` package (namespace, no Serial).
# Fail fast with a clear hint instead of an AttributeError during annotations.
if not getattr(serial, "Serial", None):
    sys.exit(
        "The `serial` module was found but has no `Serial` class. "
        "You likely installed the wrong package. "
        "Run `pip uninstall serial` then `pip install pyserial` and retry."
    )

LOG_FRAMES = False

# ---- Timings (mirror C++ ms values) ----
INIT_POLL_SECOND_MS       = 5000
INIT_POLL_INTERVAL_MS     = 5000
SCREEN_PUSH_INTERVAL_MS   = 25000
ACTIVITY_POLL_INTERVAL_MS = 150
REPLY_WAIT_S = 0.1  # 20 ms reply window – tweak if needed


INIT_SECOND_POLL_DELAY    = INIT_POLL_SECOND_MS / 1000.0
STATUS_POLL_INTERVAL      = INIT_POLL_INTERVAL_MS / 1000.0
SCREEN_REFRESH_INTERVAL   = SCREEN_PUSH_INTERVAL_MS / 1000.0
ACTIVITY_POLL_INTERVAL    = ACTIVITY_POLL_INTERVAL_MS / 1000.0


# ---- last_cmd equivalents ----
CMD_NONE        = 0
CMD_POLL_00     = 1
CMD_SCREEN_07   = 2
CMD_ACTIVITY_19 = 3
CMD_BEEP_0C     = 4


def bytes_to_hex(data: bytes | bytearray) -> str:
    return " ".join(f"{b:02X}" for b in data)


def galaxy_checksum(data: bytes | bytearray) -> int:
    """Mirror the C++ checksum exactly."""
    temp = 0xAA
    for b in data:
        temp += b
    temp &= 0xFFFFFFFF
    res = (
        ((temp >> 24) & 0xFF)
        + ((temp >> 16) & 0xFF)
        + ((temp >> 8) & 0xFF)
        + (temp & 0xFF)
    ) & 0xFF
    return res


def open_serial(port: str, baud: int = 9600) -> serial.Serial:
    ser = serial.Serial(
        port,
        baudrate=baud,
        bytesize=8,
        parity=serial.PARITY_NONE,
        stopbits=serial.STOPBITS_ONE,
        timeout=0.01,   # small timeout so loop ticks quickly
    )
    return ser


def send_frame(ser: serial.Serial, payload: bytes | bytearray):
    """Append checksum and send."""
    cs = galaxy_checksum(payload)
    frame = bytes(payload) + bytes([cs])
    if LOG_FRAMES:
        print(f"[TX] {bytes_to_hex(frame)}")
        sys.stdout.flush()
    ser.write(frame)
    ser.flush()


def send_init_poll_00_0E(ser: serial.Serial, panel_id: int):
    send_frame(ser, bytes([panel_id, 0x00, 0x0E]))


def send_status_poll_00_0F(ser: serial.Serial, panel_id: int):
    send_frame(ser, bytes([panel_id, 0x00, 0x0F]))


def send_activity_poll_19_01(ser: serial.Serial, panel_id: int):
    send_frame(ser, bytes([panel_id, 0x19, 0x01]))


def send_beep_mode_0C(ser: serial.Serial, panel_id: int, mode: int, beep_period: int = 0x00, quiet_period: int = 0x00):
    """
    Command 0C sets keypad beep mode.

    mode: 0=off, 1=on, 3=intermittent (period/quiet in 1/10s)
    """
    payload = bytes([panel_id, 0x0C, mode & 0xFF, beep_period & 0xFF, quiet_period & 0xFF])
    send_frame(ser, payload)


def build_screen_frame(panel_id: int, display_text: str) -> bytes:
    """
    Mirror the C++ screen frame format:

    screen = {ID, 0x07, 0xA1, 0x17}
    + line1
    + 0x02
    + line2
    + 0x07
    """
    if not display_text:
        display_text = "ESP-HOME|Initializing"

    # Split on first '|'
    sep = display_text.find("|")
    if sep != -1:
        line1 = display_text[:sep]
        line2 = display_text[sep + 1 :]
    else:
        line1 = display_text
        line2 = ""

    screen = bytearray([panel_id, 0x07, 0xA1, 0x17])
    screen.extend(line1.encode("ascii", errors="replace"))
    screen.append(0x02)
    screen.extend(line2.encode("ascii", errors="replace"))
    screen.append(0x07)

    return bytes(screen)


def handle_stdin_line(line: str, state: dict):
    line = line.strip()
    if not line:
        return

    if line == "/quit":
        print("[MAIN] Quit requested")
        state["quit"] = True
        return

    # future: backlight / beep frames if you want them
    if line == "/bl on":
        print("[CMD] Backlight ON (not implemented)")
        return
    if line == "/bl off":
        print("[CMD] Backlight OFF (not implemented)")
        return
    if line == "/beep":
        print("[CMD] Beep (not implemented)")
        return

    # Otherwise treat input as display_text_
    state["display_text"] = line
    state["screen_dirty"] = True
    print(f"[CMD] Set display text: {line!r}")

def decode_key_and_tamper(code: int):
    """
    Decode the 3rd byte of an F4 reply:

      00–09  => keys 0–9
      0A     => B
      0B     => A
      0C     => ENT
      0D     => ESC
      0E     => *
      0F     => #
      +0x40  => tamper + that key
      0x7F   => tamper only
    """
    # special case: tamper only
    if code == 0x7F:
        return None, True

    tamper = bool(code & 0x40)
    key_code = code & 0x0F

    key_map = {
        0x00: "0",
        0x01: "1",
        0x02: "2",
        0x03: "3",
        0x04: "4",
        0x05: "5",
        0x06: "6",
        0x07: "7",
        0x08: "8",
        0x09: "9",
        0x0A: "B",
        0x0B: "A",
        0x0C: "ENT",
        0x0D: "ESC",
        0x0E: "*",
        0x0F: "#",
    }

    return key_map.get(key_code), tamper

def handle_reply_for_cmd(bytes_in: bytes,
                         panel_id: int,
                         keypad_addr: int,
                         state: dict):
    if LOG_FRAMES:
        print(f"[RX for last_cmd={state['last_cmd']}] {bytes_to_hex(bytes_in)}")

    if not bytes_in or bytes_in[0] != keypad_addr or len(bytes_in) < 2:
        return

    type_ = bytes_in[1]
    last_cmd = state["last_cmd"]

    def update_tamper(new_tamper: bool, context: str, frame: bytes | None = None) -> bool:
        """Set tamper state and log only when it changes."""
        prev = state["in_tamper"]
        if new_tamper == prev:
            return False
        state["in_tamper"] = new_tamper
        msg = f"[TAMPER] {context}: {new_tamper}"
        if frame:
            msg += f" {bytes_to_hex(frame)}"
        print(msg)
        return True

    # --- Simple cases first: FE / FF etc for non-F4 replies ---

    # status / 00 polls
    if last_cmd == CMD_POLL_00:
        state["needs_status_before_screen"] = False
        # things like 11 FF 08 00 64 28, 11 FE BA, etc – C++ ignores them
        return

    # activity poll: 11 FE BA => no key/tamper change
    if last_cmd == CMD_ACTIVITY_19 and type_ == 0xFE:
        return

    # screen write: F2 => "bad frame"
    if last_cmd == CMD_SCREEN_07 and type_ == 0xF2:
        print(f"[SCREEN] Keypad rejected frame (F2): {bytes_to_hex(bytes_in)}")
        return

    # screen write: FE BA => "busy/OK"
    if last_cmd == CMD_SCREEN_07 and type_ == 0xFE and len(bytes_in) >= 3 and bytes_in[2] == 0xBA:
        update_tamper(False, "Cleared after SCREEN FE BA", bytes_in)
        if LOG_FRAMES:
            print(f"[SCREEN] Keypad OK (busy) FE BA: {bytes_to_hex(bytes_in)}")
        return

    # beep mode: treat FE BA as OK/busy (same shape as screen ack)
    if last_cmd == CMD_BEEP_0C and type_ == 0xFE and len(bytes_in) >= 3 and bytes_in[2] == 0xBA:
        if LOG_FRAMES:
            print(f"[BEEP] Keypad OK (FE BA): {bytes_to_hex(bytes_in)}")
        return

    # --- Now handle F4 once, for all last_cmd values ---

    if type_ == 0xF4 and len(bytes_in) == 4:
        code = bytes_in[2]
        cs   = bytes_in[3]
        expected = galaxy_checksum(bytes([keypad_addr, 0xF4, code]))
        if expected != cs:
            print(f"[F4] Bad checksum: {bytes_to_hex(bytes_in)}")
            return

        key_name, tamper = decode_key_and_tamper(code)
        tamper_changed = update_tamper(tamper, "From F4", bytes_in)

        # safety: if it's neither key nor tamper, bail
        if key_name is None and not tamper:
            print(f"[F4] Unknown code=0x{code:02X}: {bytes_to_hex(bytes_in)}")
            return

        # --- SCREEN context: screen ACK + optional tamper, no 0B here ---
        if last_cmd == CMD_SCREEN_07:
            if code == 0x7F:
                print(f"[SCREEN] ACK (tamper={tamper}) {bytes_to_hex(bytes_in)}")
            else:
                # you *can* see key+tamper piggybacked here; debug only
                if LOG_FRAMES:
                    tstr = " [TAMPER]" if tamper else ""
                    print(f"[SCREEN reply] key={key_name}{tstr} {bytes_to_hex(bytes_in)}")
            return

        # --- ACTIVITY poll (19 01): real key events live here ---
        if last_cmd == CMD_ACTIVITY_19:
            # tamper-only (0x7F) – no key; just track tamper and bail
            if key_name is None:
                if LOG_FRAMES or tamper_changed:
                    print(f"[KEY] tamper-only [TAMPER] {bytes_to_hex(bytes_in)}")
                return

            # there *is* a key: de-dup logs but ALWAYS send a 0B ACK & flip the toggle
            now = time.monotonic()
            last_evt = state.get("last_key_event", {"key": None, "tamper": None, "ts": 0.0})
            if not (
                last_evt["key"] == key_name
                and last_evt["tamper"] == tamper
                and (now - last_evt["ts"]) <= 0.2
            ):
                tstr = " [TAMPER]" if tamper else ""
                print(f"[KEY] key={key_name}{tstr} {bytes_to_hex(bytes_in)}")
                state["last_key_event"] = {"key": key_name, "tamper": tamper, "ts": now}

            # ACK back to keypad {panel_id, 0x0B, ack_toggle} and FLIP it every time
            ack_val = state["ack_toggle"]
            send_frame(state["ser"], bytes([panel_id, 0x0B, ack_val]))
            state["ack_toggle"] = 0x02 if ack_val == 0x00 else 0x00
            return

        # --- F4 after other commands (00 poll, beep, etc.) ---
        # Only interesting for tamper tracking; no 0B here.
        if LOG_FRAMES:
            tstr = " [TAMPER]" if tamper else ""
            if key_name:
                print(f"[F4 OTHER after cmd={last_cmd}] key={key_name}{tstr} {bytes_to_hex(bytes_in)}")
            else:
                print(f"[F4 OTHER after cmd={last_cmd}] tamper-only{tstr} {bytes_to_hex(bytes_in)}")
        return


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", required=False, default='/dev/tty.usbserial-0001')
    parser.add_argument("--addr", type=lambda x: int(x, 0), default=0x20)
    args = parser.parse_args()

    panel_id = args.addr
    keypad_addr = 0x11

    ser = open_serial(args.port)

    print(f"Opened {args.port}, panel ID 0x{panel_id:02X}, keypad 0x{keypad_addr:02X}")

    state = {
        "ser": ser,
        "display_text": "ESP-HOME|Initializing",
        "last_init_poll": time.monotonic(),
        "sent_second_init": False,
        "last_status_poll": 0.0,
        "last_screen_push": 0.0,
        "last_activity_poll": 0.0,
        "last_cmd": CMD_NONE,
        "ack_toggle": 0x00,
        "awaiting_reply": False,
        "last_tx_time": 0.0,
        "rx_buf": bytearray(),
        "quit": False,
        "in_tamper": False,
        "screen_dirty": False,
        "last_key_event": {"key": None, "tamper": None, "ts": 0.0},
        "beep_set": False,
        "needs_status_before_screen": False
    }

    # initial 00 0E like C++
    send_init_poll_00_0E(ser, panel_id)
    state["last_cmd"] = CMD_POLL_00
    state["awaiting_reply"] = True
    state["last_tx_time"] = time.monotonic()
    state["rx_buf"].clear()

    while not state["quit"]:
        now = time.monotonic()

        # --- 1. If idle: pick exactly ONE thing to send ---
        if not state["awaiting_reply"]:
            cmd_to_send = CMD_NONE


            # a) second init poll (00 0F) once after 200 ms
            if (not state["sent_second_init"]
                and now - state["last_init_poll"] >= INIT_SECOND_POLL_DELAY):
                cmd_to_send = CMD_POLL_00

            # b) periodic 00 0F every 1s
            elif now - state["last_init_poll"] >= STATUS_POLL_INTERVAL or state["needs_status_before_screen"]:
                cmd_to_send = CMD_POLL_00

            # c) one-time beep disable once init is done
            elif state["sent_second_init"] and not state["beep_set"]:
                cmd_to_send = CMD_BEEP_0C
            
            # d) screen update ASAP if text has changed
            elif state["sent_second_init"] and state["screen_dirty"] and not state["needs_status_before_screen"]:
                cmd_to_send = CMD_SCREEN_07


            # e) activity poll (19 01) every 50ms
            elif now - state["last_activity_poll"] >= ACTIVITY_POLL_INTERVAL:
                cmd_to_send = CMD_ACTIVITY_19

            # If something is due, send just that one
            if cmd_to_send != CMD_NONE:
                if cmd_to_send == CMD_POLL_00:
                    if not state["sent_second_init"]:
                        # second init poll
                        send_status_poll_00_0F(ser, panel_id)
                        state["sent_second_init"] = True
                    else:
                        # regular status poll
                        send_status_poll_00_0F(ser, panel_id)
                    state["last_init_poll"] = now

                elif cmd_to_send == CMD_SCREEN_07:
                    print("screen dirty:", state["screen_dirty"])
                    screen_payload = build_screen_frame(panel_id, state["display_text"])
                    frame_with_cs = screen_payload + bytes([galaxy_checksum(screen_payload)])
                    if LOG_FRAMES:
                        print(f"[SCREEN TX] ({len(frame_with_cs)} bytes): {bytes_to_hex(frame_with_cs)}")
                    send_frame(ser, screen_payload)
                    state["last_screen_push"] = now
                    state["screen_dirty"] = False
                    state["needs_status_before_screen"] = True
                    print("screen dirty:", state["screen_dirty"])

                elif cmd_to_send == CMD_BEEP_0C:
                    # ensure keypad is silent after start: mode=0 (off), zero periods
                    send_beep_mode_0C(ser, panel_id, mode=0x00, beep_period=0x00, quiet_period=0x00)
                    state["beep_set"] = True

                elif cmd_to_send == CMD_ACTIVITY_19:
                    send_activity_poll_19_01(ser, panel_id)
                    state["last_activity_poll"] = now

                # mark that we've sent something and now expect a reply
                state["last_cmd"] = cmd_to_send
                state["awaiting_reply"] = True
                state["last_tx_time"] = now
                state["rx_buf"].clear()

        # --- 2. Always read serial into rx_buf ---
        while True:
            try:
                b = ser.read(1)
            except serial.SerialException as e:
                print(f"[SERIAL] Error: {e}", file=sys.stderr)
                state["quit"] = True
                break
            if not b:
                break
            state["rx_buf"].extend(b)

        # --- 3. If waiting for a reply and time’s up: process it ---
        if state["awaiting_reply"]:
            if (now - state["last_tx_time"]) >= REPLY_WAIT_S:
                if state["rx_buf"]:
                    # treat whatever we got in this window as the reply
                    handle_reply_for_cmd(bytes(state["rx_buf"]), panel_id, keypad_addr, state)
                else:
                    # no reply – you can log or ignore
                    # print(f"[WARN] No reply for cmd {state['last_cmd']}")
                    pass
                state["rx_buf"].clear()
                state["awaiting_reply"] = False

        # --- 4. Stdin non-blocking ---
        rlist, _, _ = select.select([sys.stdin], [], [], 0.0)
        if sys.stdin in rlist:
            line = sys.stdin.readline()
            if line == "":
                state["quit"] = True
            else:
                handle_stdin_line(line, state)

        time.sleep(0.002)

    print("[MAIN] Exiting...")
    ser.close()


if __name__ == "__main__":
    main()
