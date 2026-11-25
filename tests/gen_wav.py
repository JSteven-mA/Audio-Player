#!/usr/bin/env python3
"""Genera WAV de prueba (seno) : gen_wav.py <salida> <seg> <hz_muestreo> <canales> <frecuencia>"""
import math, struct, sys, wave
out, secs, rate, ch, freq = sys.argv[1], float(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), float(sys.argv[5])
with wave.open(out, "wb") as w:
    w.setnchannels(ch); w.setsampwidth(2); w.setframerate(rate)
    frames = bytearray()
    for i in range(int(secs * rate)):
        v = int(12000 * math.sin(2 * math.pi * freq * i / rate))
        frames += struct.pack("<h", v) * ch
    w.writeframes(bytes(frames))
