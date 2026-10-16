#!/usr/bin/env python3

def classify(mean, spread, activity, base_spread, base_activity,
             flat_spread=8, flat_activity=3, ratio=20,
             min_luma=32, max_luma=224, good_count=10):
    midgray = min_luma <= mean <= max_luma
    absolute_flat = midgray and activity <= flat_activity and spread <= flat_spread
    adaptive_flat = False
    if good_count >= 5:
        adaptive_flat = (
            midgray and
            activity * 100 < base_activity * ratio and
            spread * 100 < base_spread * ratio and
            activity <= flat_activity + 3 and
            spread <= flat_spread + 8
        )
    return absolute_flat or adaptive_flat

# Typical textured grayscale surveillance frame must pass.
assert not classify(105, 34, 21, 32, 20)

# Flat decoder-gray frame must be caught.
assert classify(128, 1, 0, 32, 20)

# Legitimate black frame is outside the mid-gray window and must pass.
assert not classify(4, 1, 0, 32, 20)

# Legitimate white frame is also outside the guard window.
assert not classify(248, 1, 0, 32, 20)

# A dim but structured IR frame should pass even if monochrome.
assert not classify(58, 18, 11, 24, 16)

# Adaptive collapse: not absolutely flat, but a sharp collapse vs learned baseline.
assert classify(120, 10, 4, 60, 30)

print('grayguard synthetic checks: OK')
