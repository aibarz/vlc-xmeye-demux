def prev_weight(mad, static_thr=3, motion_thr=8, max_weight=50):
    if motion_thr <= static_thr:
        motion_thr = static_thr + 1
    if mad <= static_thr:
        return max_weight
    if mad >= motion_thr:
        return 0
    return (max_weight * (motion_thr - mad) + (motion_thr - static_thr)//2) // (motion_thr - static_thr)


def blend(curr, prev, weight, pixel_thr=6):
    residual = abs(curr - prev)
    if weight and residual <= pixel_thr:
        return ((100-weight)*curr + weight*prev + 50)//100
    return curr


assert prev_weight(0) == 50
assert prev_weight(3) == 50
assert prev_weight(8) == 0
assert 0 < prev_weight(5) < 50

# Static noisy pixel: temporal blend reduces a small fluctuation.
assert blend(104, 100, 50, 6) == 102

# Moving edge: per-pixel residual gate preserves current frame exactly.
assert blend(180, 40, 50, 6) == 180

# Clear motion block: no previous-frame contribution at all.
assert blend(103, 100, prev_weight(10), 6) == 103

# Static identical edge stays identical; no spatial blur is introduced.
assert blend(220, 220, 50, 6) == 220

print('static gate synthetic checks passed')
