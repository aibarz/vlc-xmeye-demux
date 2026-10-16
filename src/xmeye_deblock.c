/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye selective deblock filter for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * Conservative luma-only blocking-artifact suppression intended to run
 * before xmeyedeconv. It only modifies weak discontinuities located on a
 * configurable coding-grid boundary and leaves strong edges untouched.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_atomic.h>
#include <stdlib.h>
#include <string.h>

#define FILTER_PREFIX "xmeyedeblock-"

struct filter_sys_t
{
    atomic_int enabled;
    atomic_int grid;
    atomic_int threshold;
    atomic_int side_threshold;
    atomic_int strength;
    atomic_int max_delta;

    unsigned stats_frames;
    unsigned long long stats_tested;
    unsigned long long stats_adjusted;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = {
    "enabled", "grid", "threshold", "side-threshold", "strength", "max-delta", NULL
};

vlc_module_begin()
    set_shortname("XMEye Deblock")
    set_description("Selective luma deblocking for compressed XMEye video")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Deblock enabled", "Enable or bypass deblocking.", false)
    add_integer_with_range(FILTER_PREFIX "grid", 8, 4, 32,
             "Grid size", "Pixel spacing of candidate coding-block boundaries.", false)
    add_integer_with_range(FILTER_PREFIX "threshold", 8, 1, 32,
             "Boundary threshold", "Maximum luma jump eligible for deblocking.", false)
    add_integer_with_range(FILTER_PREFIX "side-threshold", 4, 0, 24,
             "Side-gradient threshold", "Maximum local gradient on each side of the candidate boundary.", false)
    add_integer_with_range(FILTER_PREFIX "strength", 35, 0, 100,
             "Deblock strength", "Fraction of the weak discontinuity removed.", false)
    add_integer_with_range(FILTER_PREFIX "max-delta", 2, 1, 8,
             "Maximum correction", "Maximum per-pixel luma correction at a block boundary.", false)
    add_shortcut("xmeyedeblock")
    set_callbacks(Create, Destroy)
vlc_module_end()

static inline int iabs_i(int v) { return v < 0 ? -v : v; }
static inline uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static int Create(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    if (filter->fmt_in.video.i_chroma != filter->fmt_out.video.i_chroma)
    {
        free(sys);
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    config_ChainParse(filter, FILTER_PREFIX, filter_options, filter->p_cfg);

    atomic_init(&sys->enabled, var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    atomic_init(&sys->grid, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "grid"), 4, 32));
    atomic_init(&sys->threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "threshold"), 1, 32));
    atomic_init(&sys->side_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "side-threshold"), 0, 24));
    atomic_init(&sys->strength, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "strength"), 0, 100));
    atomic_init(&sys->max_delta, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "max-delta"), 1, 8));

    const char *names[] = { "enabled", "grid", "threshold", "side-threshold", "strength", "max-delta", NULL };
    for (int i = 0; names[i]; ++i)
    {
        char full[64];
        snprintf(full, sizeof(full), FILTER_PREFIX "%s", names[i]);
        var_AddCallback(filter, full, OptionCallback, sys);
    }

    filter->pf_video_filter = Filter;
    msg_Info(filter, "XMEye Deblock ready: grid=%d threshold=%d side=%d strength=%d maxdelta=%d",
             atomic_load(&sys->grid), atomic_load(&sys->threshold),
             atomic_load(&sys->side_threshold), atomic_load(&sys->strength),
             atomic_load(&sys->max_delta));
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys) return;

    const char *names[] = { "enabled", "grid", "threshold", "side-threshold", "strength", "max-delta", NULL };
    for (int i = 0; names[i]; ++i)
    {
        char full[64];
        snprintf(full, sizeof(full), FILTER_PREFIX "%s", names[i]);
        var_DelCallback(filter, full, OptionCallback, sys);
    }
    free(sys);
}

static int OptionCallback(vlc_object_t *obj, const char *name,
                          vlc_value_t oldval, vlc_value_t newval, void *data)
{
    VLC_UNUSED(obj); VLC_UNUSED(oldval);
    filter_sys_t *sys = data;

    if (!strcmp(name, FILTER_PREFIX "enabled"))
        atomic_store(&sys->enabled, newval.b_bool ? 1 : 0);
    else if (!strcmp(name, FILTER_PREFIX "grid"))
        atomic_store(&sys->grid, VLC_CLIP(newval.i_int, 4, 32));
    else if (!strcmp(name, FILTER_PREFIX "threshold"))
        atomic_store(&sys->threshold, VLC_CLIP(newval.i_int, 1, 32));
    else if (!strcmp(name, FILTER_PREFIX "side-threshold"))
        atomic_store(&sys->side_threshold, VLC_CLIP(newval.i_int, 0, 24));
    else if (!strcmp(name, FILTER_PREFIX "strength"))
        atomic_store(&sys->strength, VLC_CLIP(newval.i_int, 0, 100));
    else if (!strcmp(name, FILTER_PREFIX "max-delta"))
        atomic_store(&sys->max_delta, VLC_CLIP(newval.i_int, 1, 8));
    return VLC_SUCCESS;
}

static inline int compute_delta(int p0, int q0, int strength, int max_delta)
{
    const int signed_diff = q0 - p0;
    int delta = (signed_diff * strength) / 200;
    if (delta == 0 && iabs_i(signed_diff) >= 4 && strength >= 25)
        delta = signed_diff > 0 ? 1 : -1;
    if (delta > max_delta) delta = max_delta;
    if (delta < -max_delta) delta = -max_delta;
    return delta;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic) return NULL;
    if (!atomic_load(&sys->enabled))
        return pic;
    if (pic->i_planes < 1 || pic->p[Y_PLANE].i_pixel_pitch != 1)
        return pic;

    picture_t *out = filter_NewPicture(filter);
    if (!out)
        return pic;

    picture_CopyPixels(out, pic);
    picture_CopyProperties(out, pic);

    const plane_t *src = &pic->p[Y_PLANE];
    plane_t *dst = &out->p[Y_PLANE];
    const int width = __MIN(src->i_visible_pitch, dst->i_visible_pitch);
    const int height = __MIN(src->i_visible_lines, dst->i_visible_lines);
    const int grid = atomic_load(&sys->grid);
    const int threshold = atomic_load(&sys->threshold);
    const int side_threshold = atomic_load(&sys->side_threshold);
    const int strength = atomic_load(&sys->strength);
    const int max_delta = atomic_load(&sys->max_delta);

    unsigned long long tested = 0, adjusted = 0;

    if (width >= 5 && height >= 5 && strength > 0)
    {
        /* Vertical candidate boundaries. Read from original, write to output. */
        for (int x = grid; x < width; x += grid)
        {
            if (x < 2 || x + 1 >= width) continue;
            for (int y = 0; y < height; ++y)
            {
                const uint8_t *s = src->p_pixels + y * src->i_pitch;
                uint8_t *d = dst->p_pixels + y * dst->i_pitch;
                const int p1 = s[x - 2], p0 = s[x - 1], q0 = s[x], q1 = s[x + 1];
                ++tested;
                const int jump = iabs_i(p0 - q0);
                if (jump == 0 || jump > threshold) continue;
                if (iabs_i(p1 - p0) > side_threshold || iabs_i(q1 - q0) > side_threshold) continue;
                const int delta = compute_delta(p0, q0, strength, max_delta);
                if (!delta) continue;
                d[x - 1] = clamp_u8(p0 + delta);
                d[x]     = clamp_u8(q0 - delta);
                ++adjusted;
            }
        }

        /* Horizontal candidate boundaries. Read from original to avoid cascaded blur. */
        for (int y = grid; y < height; y += grid)
        {
            if (y < 2 || y + 1 >= height) continue;
            const uint8_t *r1 = src->p_pixels + (y - 2) * src->i_pitch;
            const uint8_t *r0 = src->p_pixels + (y - 1) * src->i_pitch;
            const uint8_t *q0r = src->p_pixels + y * src->i_pitch;
            const uint8_t *q1r = src->p_pixels + (y + 1) * src->i_pitch;
            uint8_t *d0 = dst->p_pixels + (y - 1) * dst->i_pitch;
            uint8_t *d1 = dst->p_pixels + y * dst->i_pitch;
            for (int x = 0; x < width; ++x)
            {
                const int p1 = r1[x], p0 = r0[x], q0 = q0r[x], q1 = q1r[x];
                ++tested;
                const int jump = iabs_i(p0 - q0);
                if (jump == 0 || jump > threshold) continue;
                if (iabs_i(p1 - p0) > side_threshold || iabs_i(q1 - q0) > side_threshold) continue;
                const int delta = compute_delta(p0, q0, strength, max_delta);
                if (!delta) continue;
                d0[x] = clamp_u8(p0 + delta);
                d1[x] = clamp_u8(q0 - delta);
                ++adjusted;
            }
        }
    }

    picture_Release(pic);

    sys->stats_tested += tested;
    sys->stats_adjusted += adjusted;
    ++sys->stats_frames;
    if (sys->stats_frames >= 20)
    {
        const double pct = sys->stats_tested ? 100.0 * (double)sys->stats_adjusted / (double)sys->stats_tested : 0.0;
        msg_Info(filter, "deblock stats: adjusted=%.2f%% grid=%d threshold=%d side=%d strength=%d maxdelta=%d",
                 pct, grid, threshold, side_threshold, strength, max_delta);
        sys->stats_frames = 0;
        sys->stats_tested = sys->stats_adjusted = 0;
    }

    return out;
}
