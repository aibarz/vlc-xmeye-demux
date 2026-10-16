def deblock_pair(p1, p0, q0, q1, threshold=8, side_threshold=4, strength=35, max_delta=2):
    jump = abs(p0 - q0)
    if jump == 0 or jump > threshold:
        return p0, q0, False
    if abs(p1 - p0) > side_threshold or abs(q1 - q0) > side_threshold:
        return p0, q0, False
    signed_diff = q0 - p0
    delta = int((signed_diff * strength) / 200)
    if delta == 0 and abs(signed_diff) >= 4 and strength >= 25:
        delta = 1 if signed_diff > 0 else -1
    delta = max(-max_delta, min(max_delta, delta))
    return p0 + delta, q0 - delta, bool(delta)

# Weak artificial block step should be reduced.
a0, b0 = 100, 106
a1, b1, changed = deblock_pair(100, a0, b0, 106)
assert changed
assert abs(a1 - b1) < abs(a0 - b0), (a0, b0, a1, b1)

# Strong real edge must be preserved.
a0, b0 = 90, 130
a1, b1, changed = deblock_pair(90, a0, b0, 130)
assert not changed
assert (a1, b1) == (a0, b0)

# Strong side gradients indicate local detail; do not smooth.
a1, b1, changed = deblock_pair(80, 100, 106, 125)
assert not changed

print('deblock synthetic validation: PASS')
