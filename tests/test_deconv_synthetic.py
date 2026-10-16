#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
# Original concept and functional design: Álvaro Ibarz

import math


def blur_1d(signal):
    k = [1, 4, 6, 4, 1]
    out = signal[:]
    for i in range(2, len(signal)-2):
        out[i] = sum(k[j] * signal[i+j-2] for j in range(5)) / 16.0
    return out


def deconv_1d(signal, strength=0.30, polarity=1):
    out = signal[:]
    for i in range(1, len(signal)-1):
        lap = 2.0 * signal[i] - signal[i-1] - signal[i+1]
        out[i] = signal[i] + polarity * strength * lap
    return out


def max_gradient(signal, lo=40, hi=60):
    return max(abs(signal[i+1] - signal[i]) for i in range(lo, hi))


step = [0.0] * 50 + [1.0] * 51
blurred = blur_1d(step)
restored = deconv_1d(blurred, 0.30, +1)
inverse = deconv_1d(blurred, 0.30, -1)

g0 = max_gradient(blurred)
g1 = max_gradient(restored)
gm = max_gradient(inverse)

print(f"blurred_gradient={g0:.6f}")
print(f"positive_gradient={g1:.6f}")
print(f"inverse_gradient={gm:.6f}")

assert g1 > g0, "positive polarity must increase edge slope"
assert gm < g0, "negative polarity must reduce edge slope"
