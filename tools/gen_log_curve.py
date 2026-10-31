#!/usr/bin/env python3
"""CineMate Log curve in plain Python (stdlib only): prints the per-pair constants and the golden values
that tools/log_curve_test.cpp hard-codes. Regenerate with:  python3 tools/gen_log_curve.py
and paste the GOLDEN block into the test. Formulas mirror src/cdng/LogCurve.hpp."""
import math

# (source, target): (mu, footroom codes F, footroom lsb); keep in step with kPairs in LogCurve.hpp
PAIRS = {(16, 12): (10000.0, 32, 487), (12, 10): (1500.0, 32, 200), (10, 8): (160.0, 8, 22)}
# representative black/white per pair (the test also runs a second black level, without golden values)
LEVELS = {(16, 12): (3200, 65535), (12, 10): (200, 4095), (10, 8): (50, 1023)}


def enc(level, mu, F, foot, bits, bl, wl):
    cmax, top, span = (1 << bits) - 1, (1 << bits) - 1 - F, wl - bl
    if foot > 0 and F > 0 and level < bl:
        c = min(max(math.floor((level - (bl - foot)) / foot * F), 0), F - 1)
    else:
        x = min(max((level - bl) / span, 0.0), 1.0)
        c = round(math.log1p(mu * x) / math.log1p(mu) * top) + F  # round() is ties-to-even
    return int(min(max(c, 0), cmax))


def dec(code, mu, F, foot, bits, bl, wl):
    top, span = (1 << bits) - 1 - F, wl - bl
    if foot > 0 and F > 0 and code < F:
        v = (bl - foot) + (code + 0.5) / F * foot
    else:
        v = bl + (math.pow(1.0 + mu, max(0, code - F) / top) - 1.0) / mu * span
    return int(min(max(round(v), 0), wl))


def golden_levels(bl, wl, foot):
    span = wl - bl
    pts = [0, bl - foot // 2, bl - 1, bl, bl + 1, bl + 2, bl + 10, bl + span // 100, bl + span // 10,
           bl + span // 2, wl - 1, wl]
    return sorted(set(pts))


for (src, tgt), (mu, F, foot) in PAIRS.items():
    bl, wl = LEVELS[(src, tgt)]
    print(f"// {src}->{tgt}: mu={mu:g} F={F} foot={foot} black={bl} white={wl}")
    print(f"{{{src}, {tgt}, {bl}, {wl}, {{")
    for lv in golden_levels(bl, wl, foot):
        c = enc(lv, mu, F, foot, tgt, bl, wl)
        print(f"    {{{lv}, {c}, {dec(c, mu, F, foot, tgt, bl, wl)}}},")
    print("}},")
