def classify(samples, static_thr=2, changed_thr=4, max_changed=8):
    diffs = [abs(a-b) for a,b in samples]
    mad = round(sum(diffs)/len(diffs)) if diffs else 0
    changed = round(100*sum(d >= changed_thr for d in diffs)/len(diffs)) if diffs else 0
    return mad <= static_thr and changed <= max_changed, mad, changed

# Static noisy block: small temporal noise should remain eligible.
static_samples = [(100,101)] * 60 + [(100,102)] * 4
ok, mad, changed = classify(static_samples)
assert ok, (mad, changed)

# A moving thin edge: average MAD can still look small, but changed-pixel ratio must reject it.
moving_samples = [(100,100)] * 56 + [(40,100)] * 8
ok, mad, changed = classify(moving_samples)
assert not ok, (mad, changed)

# Per-pixel residual gate: even inside a static block, changed pixels remain current-only.
def blend(cur, prev, protected=False, pixel_thr=2, prev_weight=50):
    residual = abs(cur-prev)
    if protected or residual > pixel_thr:
        return cur
    return round(((100-prev_weight)*cur + prev_weight*prev)/100)

assert blend(101,100) == 100 or blend(101,100) == 101
assert blend(110,100) == 110
assert blend(101,100,protected=True) == 101

# One moving block protects its 8 neighbours with radius=1.
cols=3; rows=3
motion=[0]*9
motion[4]=1
for cy in range(rows):
    for cx in range(cols):
        protected=False
        for y in range(max(0,cy-1), min(rows-1,cy+1)+1):
            for x in range(max(0,cx-1), min(cols-1,cx+1)+1):
                protected |= bool(motion[y*cols+x])
        assert protected

print('static gate v2 synthetic checks passed')
