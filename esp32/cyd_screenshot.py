#!/usr/bin/env python3
"""Zrzut ekranu dashboardu CYD do PNG (bez Pillow — tylko pyserial + zlib).

Firmware na literę 'S' odsyła bufor ekranu: linia "[shot] W H 8", potem W*H bajtów RGB332.
Działa tylko na ekranie dashboardu.

Uwaga: na macOS otwarcie portu resetuje CYD (mimo dtr/rts=False), więc skrypt czeka
--wait sekund, aż płytka połączy się z Wi-Fi i dostanie dane. Pełna historia GPU (60 s)
wymaga --wait 70.

Użycie:
    python3 cyd_screenshot.py [plik.png] [--scale N] [--wait S] [--port /dev/cu.usbserial-XXXX]
Domyślnie: dashboard.png, skala 2, czekanie 12 s, pierwszy port /dev/cu.usbserial-*.
"""
import argparse
import glob
import struct
import sys
import time
import zlib

import serial  # pyserial


def rgb332_to_rgb888(v):
    # rozciągnięcie 3/3/2 bitów na pełne 0..255
    return ((v >> 5) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 255 // 3)


def write_png(path, w, h, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    raw = b"".join(b"\x00" + bytes(r) for r in rows)  # filtr 0 dla każdego wiersza
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))  # 8 bit RGB
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out", nargs="?", default="dashboard.png")
    ap.add_argument("--scale", type=int, default=2)
    ap.add_argument("--wait", type=float, default=12,
                    help="sekundy od otwarcia portu do zrzutu (płytka się resetuje)")
    ap.add_argument("--port", default=None)
    ap.add_argument("--rot", type=int, default=0,
                    help="obróć ekran o N x 90° przed zrzutem (bez zapisu ustawienia)")
    ap.add_argument("--print", action="store_true",
                    help="zrzut strony drukarki (firmware przełącza stronę na 'P')")
    a = ap.parse_args()

    port = a.port or next(iter(sorted(glob.glob("/dev/cu.usbserial-*"))), None)
    if not port:
        sys.exit("Nie znaleziono portu /dev/cu.usbserial-* — podłącz CYD.")

    s = serial.Serial()
    s.port, s.baudrate, s.timeout = port, 115200, 15
    s.dtr = False  # próba uniknięcia resetu (na macOS i tak bywa reset — stąd --wait)
    s.rts = False
    s.open()
    print(f"Czekam {a.wait:.0f} s na Wi-Fi i dane…", file=sys.stderr)
    t_end = time.time() + a.wait
    while time.time() < t_end:       # odbieramy log, żeby bufor się nie przepełnił
        s.read(s.in_waiting or 1)
    for _ in range(a.rot % 4):
        s.write(b"R")
        time.sleep(0.6)
    if a.print:
        s.write(b"P")                # strona drukarki, chwila na przerysowanie
        time.sleep(1.0)
    s.reset_input_buffer()
    s.write(b"S")

    # czekamy na nagłówek (pomijamy inne linie logu)
    deadline = time.time() + 10
    w = h = 0
    while time.time() < deadline:
        line = s.readline().decode("ascii", "replace").strip()
        if line.startswith("[shot] brak"):
            sys.exit(line)
        if line.startswith("[shot] "):
            _, sw, sh, depth = line.split()
            w, h = int(sw), int(sh)
            if depth != "8":
                sys.exit(f"Nieobsługiwana głębia: {depth}")
            break
    if not w:
        sys.exit("Brak odpowiedzi — czy CYD jest na ekranie dashboardu?")

    data = s.read(w * h)
    s.close()
    if len(data) != w * h:
        sys.exit(f"Odebrano {len(data)} z {w * h} bajtów")

    lut = [rgb332_to_rgb888(v) for v in range(256)]
    k = max(1, a.scale)
    rows = []
    for y in range(h):
        row = []
        for v in data[y * w:(y + 1) * w]:
            row.extend(lut[v] * k)
        for _ in range(k):
            rows.append(row)
    write_png(a.out, w * k, h * k, rows)
    print(f"Zapisano {a.out} ({w * k}x{h * k})")


if __name__ == "__main__":
    main()
