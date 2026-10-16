/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye adaptive gray/invalid-frame guard for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_atomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define FILTER_PREFIX "xmeyegrayguard-"

struct filter_sys_t
{
    picture_t *last_good;
    bool have_good;
    unsigned good_count;
    uint32_t baseline_activity_q8;
    uint32_t baseline_spread_q8;

    atomic_int enabled;
    atomic_int adaptive;
    atomic_int sample_step;
    atomic_int flat_activity;
    atomic_int flat_spread;
    atomic_int ratio_percent;
    atomic_int min_luma;
    atomic_int max_luma;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = {
    "enabled", "adaptive", "sample-step", "flat-activity", "flat-spread",
    "ratio-percent", "min-luma", "max-luma", NULL
};

vlc_module_begin()
    set_shortname("XMEye Gray Guard")
    set_description("Adaptive guard that replaces suspicious flat gray decoder frames with the last valid frame")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Gray-frame guard enabled", "Enable adaptive invalid-frame protection.", false)
    add_bool(FILTER_PREFIX "adaptive", true,
             "Adaptive baseline", "Learn normal image activity from valid frames.", false)
    add_integer_with_range(FILTER_PREFIX "sample-step", 8, 2, 32,
                           "Sampling step", "Pixel sampling step used for low-cost frame statistics.", false)
    add_integer_with_range(FILTER_PREFIX "flat-activity", 3, 0, 32,
                           "Absolute flat activity", "Maximum average local luma activity for an absolutely flat frame.", false)
    add_integer_with_range(FILTER_PREFIX "flat-spread", 8, 0, 64,
                           "Absolute flat spread", "Maximum mean absolute luma spread for an absolutely flat frame.", false)
    add_integer_with_range(FILTER_PREFIX "ratio-percent", 20, 5, 60,
                           "Adaptive collapse ratio", "Frame activity/spread must collapse below this percentage of learned normal levels.", false)
    add_integer_with_range(FILTER_PREFIX "min-luma", 32, 0, 255,
                           "Minimum gray luma", "Lower mean-luma bound for suspicious gray frames.", false)
    add_integer_with_range(FILTER_PREFIX "max-luma", 224, 0, 255,
                           "Maximum gray luma", "Upper mean-luma bound for suspicious gray frames.", false)
    add_shortcut("xmeyegrayguard")
    set_callbacks(Create, Destroy)
vlc_module_end()

static void ResetState(filter_sys_t *sys)
{
    sys->have_good = false;
    sys->good_count = 0;
    sys->baseline_activity_q8 = 0;
    sys->baseline_spread_q8 = 0;
}

static int Create(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    sys->last_good = picture_NewFromFormat(&filter->fmt_in.video);
    if (!sys->last_good || sys->last_good->i_planes <= Y_PLANE)
    {
        if (sys->last_good) picture_Release(sys->last_good);
        free(sys);
        msg_Err(filter, "XMEye Gray Guard requires CPU-accessible luma plane");
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    config_ChainParse(filter, FILTER_PREFIX, filter_options, filter->p_cfg);

    atomic_init(&sys->enabled, var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    atomic_init(&sys->adaptive, var_CreateGetBoolCommand(filter, FILTER_PREFIX "adaptive") ? 1 : 0);
    atomic_init(&sys->sample_step, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "sample-step"), 2, 32));
    atomic_init(&sys->flat_activity, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "flat-activity"), 0, 32));
    atomic_init(&sys->flat_spread, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "flat-spread"), 0, 64));
    atomic_init(&sys->ratio_percent, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "ratio-percent"), 5, 60));
    atomic_init(&sys->min_luma, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "min-luma"), 0, 255));
    atomic_init(&sys->max_luma, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "max-luma"), 0, 255));

    for (const char *const *opt = filter_options; *opt; ++opt)
    {
        char name[96];
        snprintf(name, sizeof(name), FILTER_PREFIX "%s", *opt);
        var_AddCallback(filter, name, OptionCallback, sys);
    }

    ResetState(sys);
    filter->pf_video_filter = Filter;
    filter->pf_flush = Flush;
    msg_Info(filter, "XMEye Gray Guard ready: adaptive=%d step=%d flat_activity=%d flat_spread=%d ratio=%d%%",
             atomic_load(&sys->adaptive), atomic_load(&sys->sample_step),
             atomic_load(&sys->flat_activity), atomic_load(&sys->flat_spread),
             atomic_load(&sys->ratio_percent));
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys)
        return;

    for (const char *const *opt = filter_options; *opt; ++opt)
    {
        char name[96];
        snprintf(name, sizeof(name), FILTER_PREFIX "%s", *opt);
        var_DelCallback(filter, name, OptionCallback, sys);
    }

    if (sys->last_good) picture_Release(sys->last_good);
    free(sys);
}

static void Flush(filter_t *filter)
{
    ResetState(filter->p_sys);
}

static int OptionCallback(vlc_object_t *obj, const char *name,
                          vlc_value_t oldval, vlc_value_t newval, void *data)
{
    VLC_UNUSED(obj);
    VLC_UNUSED(oldval);
    filter_sys_t *sys = data;

    if (!strcmp(name, FILTER_PREFIX "enabled"))
    {
        atomic_store(&sys->enabled, newval.b_bool ? 1 : 0);
        ResetState(sys);
    }
    else if (!strcmp(name, FILTER_PREFIX "adaptive"))
    {
        atomic_store(&sys->adaptive, newval.b_bool ? 1 : 0);
        ResetState(sys);
    }
    else if (!strcmp(name, FILTER_PREFIX "sample-step"))
        atomic_store(&sys->sample_step, VLC_CLIP(newval.i_int, 2, 32));
    else if (!strcmp(name, FILTER_PREFIX "flat-activity"))
        atomic_store(&sys->flat_activity, VLC_CLIP(newval.i_int, 0, 32));
    else if (!strcmp(name, FILTER_PREFIX "flat-spread"))
        atomic_store(&sys->flat_spread, VLC_CLIP(newval.i_int, 0, 64));
    else if (!strcmp(name, FILTER_PREFIX "ratio-percent"))
        atomic_store(&sys->ratio_percent, VLC_CLIP(newval.i_int, 5, 60));
    else if (!strcmp(name, FILTER_PREFIX "min-luma"))
        atomic_store(&sys->min_luma, VLC_CLIP(newval.i_int, 0, 255));
    else if (!strcmp(name, FILTER_PREFIX "max-luma"))
        atomic_store(&sys->max_luma, VLC_CLIP(newval.i_int, 0, 255));

    return VLC_SUCCESS;
}

static void FrameStats(const plane_t *yplane, int step,
                       unsigned *mean_out, unsigned *spread_out, unsigned *activity_out)
{
    const int width = yplane->i_visible_pitch;
    const int height = yplane->i_visible_lines;
    uint64_t sum = 0;
    uint64_t count = 0;

    for (int y = 0; y < height; y += step)
    {
        const uint8_t *row = yplane->p_pixels + y * yplane->i_pitch;
        for (int x = 0; x < width; x += step)
        {
            sum += row[x];
            ++count;
        }
    }

    if (!count)
    {
        *mean_out = *spread_out = *activity_out = 0;
        return;
    }

    const unsigned mean = (unsigned)((sum + count / 2) / count);
    uint64_t spread_sum = 0;
    uint64_t activity_sum = 0;
    uint64_t activity_count = 0;

    for (int y = 0; y < height; y += step)
    {
        const uint8_t *row = yplane->p_pixels + y * yplane->i_pitch;
        const uint8_t *row2 = (y + step < height) ? yplane->p_pixels + (y + step) * yplane->i_pitch : NULL;
        for (int x = 0; x < width; x += step)
        {
            const int v = row[x];
            spread_sum += (uint64_t)abs(v - (int)mean);
            if (x + step < width)
            {
                activity_sum += (uint64_t)abs(v - (int)row[x + step]);
                ++activity_count;
            }
            if (row2)
            {
                activity_sum += (uint64_t)abs(v - (int)row2[x]);
                ++activity_count;
            }
        }
    }

    *mean_out = mean;
    *spread_out = (unsigned)((spread_sum + count / 2) / count);
    *activity_out = activity_count ? (unsigned)((activity_sum + activity_count / 2) / activity_count) : 0;
}

static void UpdateBaseline(filter_sys_t *sys, unsigned activity, unsigned spread)
{
    const uint32_t aq8 = activity << 8;
    const uint32_t sq8 = spread << 8;
    if (sys->good_count == 0)
    {
        sys->baseline_activity_q8 = aq8;
        sys->baseline_spread_q8 = sq8;
    }
    else
    {
        /* Slow EWMA: 15/16 history + 1/16 new sample. */
        sys->baseline_activity_q8 = (15u * sys->baseline_activity_q8 + aq8 + 8u) >> 4;
        sys->baseline_spread_q8 = (15u * sys->baseline_spread_q8 + sq8 + 8u) >> 4;
    }
    if (sys->good_count < 1000000u)
        ++sys->good_count;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic)
        return NULL;

    if (!atomic_load(&sys->enabled))
        return pic;

    const plane_t *yp = &pic->p[Y_PLANE];
    const int step = atomic_load(&sys->sample_step);
    unsigned mean = 0, spread = 0, activity = 0;
    FrameStats(yp, step, &mean, &spread, &activity);

    int min_luma = atomic_load(&sys->min_luma);
    int max_luma = atomic_load(&sys->max_luma);
    if (min_luma > max_luma)
    {
        int t = min_luma;
        min_luma = max_luma;
        max_luma = t;
    }

    const bool midgray = mean >= (unsigned)min_luma && mean <= (unsigned)max_luma;
    const unsigned flat_activity = (unsigned)atomic_load(&sys->flat_activity);
    const unsigned flat_spread = (unsigned)atomic_load(&sys->flat_spread);
    const unsigned ratio = (unsigned)atomic_load(&sys->ratio_percent);

    const bool absolute_flat = midgray && activity <= flat_activity && spread <= flat_spread;
    bool adaptive_flat = false;

    if (atomic_load(&sys->adaptive) && sys->good_count >= 5)
    {
        const unsigned base_activity = (sys->baseline_activity_q8 + 128u) >> 8;
        const unsigned base_spread = (sys->baseline_spread_q8 + 128u) >> 8;
        adaptive_flat = midgray &&
                        activity * 100u < base_activity * ratio &&
                        spread * 100u < base_spread * ratio &&
                        activity <= flat_activity + 3u &&
                        spread <= flat_spread + 8u;
    }

    const bool suspicious = absolute_flat || adaptive_flat;

    if (suspicious && sys->have_good)
    {
        picture_t *out = filter_NewPicture(filter);
        if (out)
        {
            picture_CopyPixels(out, sys->last_good);
            picture_CopyProperties(out, pic);
            msg_Dbg(filter, "Gray Guard held frame: mean=%u spread=%u activity=%u", mean, spread, activity);
            picture_Release(pic);
            return out;
        }
        return pic;
    }

    picture_CopyPixels(sys->last_good, pic);
    sys->have_good = true;
    UpdateBaseline(sys, activity, spread);
    return pic;
}
