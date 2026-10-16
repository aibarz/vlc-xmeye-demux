/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye focus correction filter for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * Conservative luma-only microcontrast restoration for mildly defocused video.
 * This is an edge-limited inverse-blur approximation, not generative detail.
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

#define FILTER_PREFIX "xmeyefocus-"

struct filter_sys_t
{
    atomic_int enabled;
    atomic_int strength;
    atomic_int activity_threshold;
    atomic_int correction_limit;

    unsigned stats_frames;
    unsigned long long stats_pixels;
    unsigned long long stats_corrected;
    unsigned long long stats_abs_correction;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = {
    "enabled", "strength", "activity-threshold", "correction-limit", NULL
};

vlc_module_begin()
    set_shortname("XMEye Focus Correction")
    set_description("Edge-limited luma microcontrast restoration for mild optical defocus")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Focus correction enabled",
             "Enable or bypass focus correction without removing the filter from the chain.", false)
    add_integer_with_range(FILTER_PREFIX "strength", 35, 0, 100,
             "Focus correction strength",
             "Amount of recovered local contrast. 35 is a conservative starting point.", false)
    add_integer_with_range(FILTER_PREFIX "activity-threshold", 5, 0, 32,
             "Local activity threshold",
             "Minimum 3x3 luma range required before enhancement; suppresses flat-area noise.", false)
    add_integer_with_range(FILTER_PREFIX "correction-limit", 8, 1, 32,
             "Per-pixel correction limit",
             "Maximum absolute luma correction per pixel; limits halos and ringing.", false)
    add_shortcut("xmeyefocus")
    set_callbacks(Create, Destroy)
vlc_module_end()

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

    atomic_init(&sys->enabled,
                var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    atomic_init(&sys->strength,
                VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "strength"), 0, 100));
    atomic_init(&sys->activity_threshold,
                VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "activity-threshold"), 0, 32));
    atomic_init(&sys->correction_limit,
                VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "correction-limit"), 1, 32));

    var_AddCallback(filter, FILTER_PREFIX "enabled", OptionCallback, sys);
    var_AddCallback(filter, FILTER_PREFIX "strength", OptionCallback, sys);
    var_AddCallback(filter, FILTER_PREFIX "activity-threshold", OptionCallback, sys);
    var_AddCallback(filter, FILTER_PREFIX "correction-limit", OptionCallback, sys);

    filter->pf_video_filter = Filter;
    msg_Info(filter, "XMEye Focus Correction ready: strength=%d activity=%d limit=%d",
             atomic_load(&sys->strength), atomic_load(&sys->activity_threshold),
             atomic_load(&sys->correction_limit));
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys)
        return;

    var_DelCallback(filter, FILTER_PREFIX "enabled", OptionCallback, sys);
    var_DelCallback(filter, FILTER_PREFIX "strength", OptionCallback, sys);
    var_DelCallback(filter, FILTER_PREFIX "activity-threshold", OptionCallback, sys);
    var_DelCallback(filter, FILTER_PREFIX "correction-limit", OptionCallback, sys);
    free(sys);
}

static int OptionCallback(vlc_object_t *obj, const char *name,
                          vlc_value_t oldval, vlc_value_t newval, void *data)
{
    VLC_UNUSED(obj);
    VLC_UNUSED(oldval);
    filter_sys_t *sys = data;

    if (!strcmp(name, FILTER_PREFIX "enabled"))
        atomic_store(&sys->enabled, newval.b_bool ? 1 : 0);
    else if (!strcmp(name, FILTER_PREFIX "strength"))
        atomic_store(&sys->strength, VLC_CLIP(newval.i_int, 0, 100));
    else if (!strcmp(name, FILTER_PREFIX "activity-threshold"))
        atomic_store(&sys->activity_threshold, VLC_CLIP(newval.i_int, 0, 32));
    else if (!strcmp(name, FILTER_PREFIX "correction-limit"))
        atomic_store(&sys->correction_limit, VLC_CLIP(newval.i_int, 1, 32));

    return VLC_SUCCESS;
}

static inline uint8_t clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic)
        return NULL;
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
    const int strength = atomic_load(&sys->strength);
    const int activity_threshold = atomic_load(&sys->activity_threshold);
    const int correction_limit = atomic_load(&sys->correction_limit);

    unsigned long long pixels = 0;
    unsigned long long corrected = 0;
    unsigned long long abs_correction = 0;

    if (width >= 3 && height >= 3 && strength > 0)
    {
        for (int y = 1; y < height - 1; ++y)
        {
            const uint8_t *r0 = src->p_pixels + (y - 1) * src->i_pitch;
            const uint8_t *r1 = src->p_pixels + y * src->i_pitch;
            const uint8_t *r2 = src->p_pixels + (y + 1) * src->i_pitch;
            uint8_t *d = dst->p_pixels + y * dst->i_pitch;

            for (int x = 1; x < width - 1; ++x)
            {
                ++pixels;

                int mn = r0[x - 1], mx = r0[x - 1];
#define ACC(v) do { int _q = (v); if (_q < mn) mn = _q; if (_q > mx) mx = _q; } while (0)
                ACC(r0[x]); ACC(r0[x + 1]);
                ACC(r1[x - 1]); ACC(r1[x]); ACC(r1[x + 1]);
                ACC(r2[x - 1]); ACC(r2[x]); ACC(r2[x + 1]);
#undef ACC
                if (mx - mn < activity_threshold)
                    continue;

                /* 3x3 Gaussian PSF approximation: [1 2 1; 2 4 2; 1 2 1] / 16. */
                const int blur =
                    (r0[x - 1] + 2 * r0[x] + r0[x + 1] +
                     2 * r1[x - 1] + 4 * r1[x] + 2 * r1[x + 1] +
                     r2[x - 1] + 2 * r2[x] + r2[x + 1] + 8) >> 4;

                const int detail = (int)r1[x] - blur;
                int correction = (detail * strength + (detail >= 0 ? 50 : -50)) / 100;
                if (correction > correction_limit) correction = correction_limit;
                if (correction < -correction_limit) correction = -correction_limit;
                if (correction == 0)
                    continue;

                d[x] = clamp_u8((int)r1[x] + correction);
                ++corrected;
                abs_correction += (unsigned)(correction < 0 ? -correction : correction);
            }
        }
    }

    picture_Release(pic);

    sys->stats_pixels += pixels;
    sys->stats_corrected += corrected;
    sys->stats_abs_correction += abs_correction;
    ++sys->stats_frames;
    if (sys->stats_frames >= 20)
    {
        const double pct = sys->stats_pixels ? 100.0 * (double)sys->stats_corrected / (double)sys->stats_pixels : 0.0;
        const double avg = sys->stats_corrected ? (double)sys->stats_abs_correction / (double)sys->stats_corrected : 0.0;
        msg_Info(filter, "focus stats: corrected=%.1f%% avg_delta=%.2f strength=%d activity=%d limit=%d",
                 pct, avg, strength, activity_threshold, correction_limit);
        sys->stats_frames = 0;
        sys->stats_pixels = sys->stats_corrected = sys->stats_abs_correction = 0;
    }

    return out;
}
