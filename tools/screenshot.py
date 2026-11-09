"""Save a screenshot of the Tab5 screen over USB serial, and optionally tap it first.

    python tools/screenshot.py --port COM12 out.png
    python tools/screenshot.py --port COM12 --tap 778,54 --tap 100,200 out.png

The firmware answers the serial command `P` with the finished frame as raw RGB565, and
accepts `U x y` as a touch. Needs pyserial and Pillow.
"""

import argparse
import sys
import time

from PIL import Image

sys.path.insert(0, "tools")
from tab5serial import open_port


def read_until(ser, marker, timeout):
    data = b""
    end = time.time() + timeout
    while time.time() < end:
        chunk = ser.read(4096)
        if chunk:
            data += chunk
            if marker in data:
                return data
    return data


def tap(ser, x, y, wait=0.6):
    """Touch the screen at x, y (1280x720 landscape coordinates)."""
    ser.write(f"U {x} {y}\n".encode())
    time.sleep(wait)


def capture(ser, output, tries=3):
    """Ask for the current frame and save it as a PNG."""
    for attempt in range(tries):
        if try_capture(ser, output):
            return
        print("Frame incomplete, retrying")
    sys.exit("Could not get a complete frame")


def try_capture(ser, output):
    magic = b"\xa5\x5a\xa5\x5a"
    ser.reset_input_buffer()
    ser.write(b"P\n")
    data = read_until(ser, b"SHOT ", 5)
    start = data.rfind(b"\nSHOT ")
    if start < 0:
        return False
    while b"\n" not in data[start + 1 :]:
        data += ser.read(4096)
    header_end = data.index(b"\n", start + 1)
    _, w, h = data[start + 1 : header_end].split()
    w, h = int(w), int(h)
    total = w * h * 2
    chunk = 2048
    count = (total + chunk - 1) // chunk
    raw = bytearray(total)
    got = set()
    buf = data[header_end + 1 :]
    deadline = time.time() + 25
    while time.time() < deadline and len(got) < count:
        while True:
            at = buf.find(magic)
            if at < 0:
                buf = buf[-3:]
                break
            if len(buf) < at + 8:
                buf = buf[at:]
                break
            index = buf[at + 4] | (buf[at + 5] << 8)
            length = buf[at + 6] | (buf[at + 7] << 8)
            if len(buf) < at + 8 + length:
                buf = buf[at:]
                break
            if index < count and length == min(chunk, total - index * chunk):
                raw[index * chunk : index * chunk + length] = buf[at + 8 : at + 8 + length]
                got.add(index)
                buf = buf[at + 8 + length :]
            else:
                buf = buf[at + 4 :]
        if len(got) >= count:
            break
        more = ser.read(65536)
        if more:
            buf += more
    if len(got) < count:
        print(f"Got {len(got)} of {count} chunks")
        return False

    # The sprite stores RGB565 with the bytes swapped.
    rgb = bytearray(w * h * 3)
    for i in range(0, total, 2):
        v = (raw[i] << 8) | raw[i + 1]
        j = i // 2 * 3
        rgb[j] = ((v >> 11) & 31) * 255 // 31
        rgb[j + 1] = ((v >> 5) & 63) * 255 // 63
        rgb[j + 2] = (v & 31) * 255 // 31
    Image.frombytes("RGB", (w, h), bytes(rgb)).save(output)
    print(f"Saved {output} ({w}x{h})")
    return True

def main():
    parser = argparse.ArgumentParser(description="Screenshot the Tab5")
    parser.add_argument("output")
    parser.add_argument("--port", default="COM12")
    parser.add_argument("--tap", action="append", default=[], help="x,y touch to send first")
    parser.add_argument("--wait", type=float, default=1.5, help="seconds to wait after each tap")
    args = parser.parse_args()
    ser = open_port(args.port, 0.3)
    for t in args.tap:
        x, y = (int(v) for v in t.split(","))
        tap(ser, x, y, args.wait)
    capture(ser, args.output)
    ser.close()


if __name__ == "__main__":
    main()
