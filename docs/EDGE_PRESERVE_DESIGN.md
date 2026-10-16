# Edge-preserving temporal fusion

Original concept and functional design: Álvaro Ibarz

## Problem

Straight temporal averaging reduces random noise but visibly softens moving edges, even when block motion compensation is reasonably accurate. A later sharpening stage cannot fully restore an edge that has already been averaged across slightly different spatial positions.

## Revised approach

For each current/aligned-previous sample, split luma into:

- low-frequency component (`L`) using a compact 3x3 Gaussian approximation;
- high-frequency/detail component (`H = Y - L`).

Temporal fusion is applied primarily to `L`, while the current-frame `H` is preserved:

`Y_out = blend(L_current, L_previous_aligned) + H_current`

This preserves the spatial location and contrast of the current moving edge while still averaging slower-varying noise/illumination information.

A future confidence-weighted mode may blend a small amount of aligned previous high-frequency detail only when block-match confidence is high.

## Pipeline

`XMEye demux -> motion classification/matching -> edge-preserving temporal fusion -> optional focus correction -> zoom`

The focus-correction stage remains independent and may be disabled for A/B comparison.
