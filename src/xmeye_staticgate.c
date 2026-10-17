/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye strict static-only temporal denoiser for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * One input frame -> one output frame. Output cadence is unchanged.
 * Only luma in confidently static areas may use the previous RAW frame.
 * Moving/uncertain pixels always keep the current frame.
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

#define FILTER_PREFIX "xmeyegate-"

struct filter_sys_t
{
    uint8_t *prev_y;
    size_t prev_size;
    int width;
    int height;
    bool have_prev;
    uint64_t frame_count;

    atomic_int enabled;
    atomic_int block_size;
    atomic_int sample_step;
    atomic_int static_threshold;
    atomic_int motion_threshold;
    atomic_int pixel_threshold;
    atomic_int max_prev_weight;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = {
    "enabled", "block-size", "sample-step", "static-threshold",
    "motion-threshold", "pixel-threshold", "max-prev-weight", NULL
};

vlc_module_begin()
    set_shortname("XMEye Static Gate")
    set_description("Strict motion-gated temporal luma denoiser; motion remains current-frame only")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Static temporal gate enabled", "Enable strict static-only temporal blending.", false)
    add_integer_with_range(FILTER_PREFIX "block-size", 16, 8, 32,
                           "Motion block size", "Luma block size for temporal motion classification.", false)
    add_integer_with_range(FILTER_PREFIX "sample-step", 2, 1, 4,
                           "Motion sample step", "Sampling stride inside each motion block.", false)
    add_integer_with_range(FILTER_PREFIX "static-threshold", 3, 0, 16,
                           "Static threshold", "Mean luma difference at or below this value gets maximum temporal blend.", false)
    add_integer_with_range(FILTER_PREFIX "motion-threshold", 8, 1, 32,
                           "Motion threshold", "Mean luma difference at or above this value disables temporal blend.", false)
    add_integer_with_range(FILTER_PREFIX "pixel-threshold", 6, 1, 32,
                           "Per-pixel residual gate", "Pixels changing more than this value always use the current frame.", false)
    add_integer_with_range(FILTER_PREFIX "max-prev-weight", 50, 0, 60,
                           "Maximum previous-frame weight", "Maximum percent of previous RAW luma in confidently static pixels.", false)
    add_shortcut("xmeyegate")
    set_callbacks(Create, Destroy)
vlc_module_end()

static void ResetState(filter_sys_t *sys)
{
    sys->have_prev = false;
    sys->frame_count = 0;
}

static int Create(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    if (filter->fmt_in.video.i_visible_width == 0 || filter->fmt_in.video.i_visible_height == 0)
    {
        free(sys);
        return VLC_EGENERIC;
    }

    sys->width = (int)filter->fmt_in.video.i_visible_width;
    sys->height = (int)filter->fmt_in.video.i_visible_height;
    sys->prev_size = (size_t)sys->width * (size_t)sys->height;
    sys->prev_y = malloc(sys->prev_size);
    if (!sys->prev_y)
    {
        free(sys);
        return VLC_ENOMEM;
    }

    filter->p_sys = sys;
    config_ChainParse(filter, FILTER_PREFIX, filter_options, filter->p_cfg);

    atomic_init(&sys->enabled, var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    atomic_init(&sys->block_size, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "block-size"), 8, 32));
    atomic_init(&sys->sample_step, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "sample-step"), 1, 4));
    atomic_init(&sys->static_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "static-threshold"), 0, 16));
    atomic_init(&sys->motion_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "motion-threshold"), 1, 32));
    atomic_init(&sys->pixel_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "pixel-threshold"), 1, 32));
    atomic_init(&sys->max_prev_weight, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "max-prev-weight"), 0, 60));

    for (const char *const *opt = filter_options; *opt; ++opt)
    {
        char name[96];
        snprintf(name, sizeof(name), FILTER_PREFIX "%s", *opt);
        var_AddCallback(filter, name, OptionCallback, sys);
    }

    ResetState(sys);
    filter->pf_video_filter = Filter;
    filter->pf_flush = Flush;
    msg_Info(filter, "XMEye Static Gate ready: block=%d static=%d motion=%d pixel=%d prev=%d%%",
             atomic_load(&sys->block_size), atomic_load(&sys->static_threshold),
             atomic_load(&sys->motion_threshold), atomic_load(&sys->pixel_threshold),
             atomic_load(&sys->max_prev_weight));
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

    free(sys->prev_y);
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
    else if (!strcmp(name, FILTER_PREFIX "block-size"))
        atomic_store(&sys->block_size, VLC_CLIP(newval.i_int, 8, 32));
    else if (!strcmp(name, FILTER_PREFIX "sample-step"))
        atomic_store(&sys->sample_step, VLC_CLIP(newval.i_int, 1, 4));
    else if (!strcmp(name, FILTER_PREFIX "static-threshold"))
        atomic_store(&sys->static_threshold, VLC_CLIP(newval.i_int, 0, 16));
    else if (!strcmp(name, FILTER_PREFIX "motion-threshold"))
        atomic_store(&sys->motion_threshold, VLC_CLIP(newval.i_int, 1, 32));
    else if (!strcmp(name, FILTER_PREFIX "pixel-threshold"))
        atomic_store(&sys->pixel_threshold, VLC_CLIP(newval.i_int, 1, 32));
    else if (!strcmp(name, FILTER_PREFIX "max-prev-weight"))
        atomic_store(&sys->max_prev_weight, VLC_CLIP(newval.i_int, 0, 60));

    return VLC_SUCCESS;
}

static void SaveCurrentRaw(const plane_t *yp, filter_sys_t *sys)
{
    const int width = __MIN(sys->width, yp->i_visible_pitch);
    const int height = __MIN(sys->height, yp->i_visible_lines);
    for (int y = 0; y < height; ++y)
        memcpy(sys->prev_y + (size_t)y * sys->width,
               yp->p_pixels + y * yp->i_pitch, (size_t)width);
}

static unsigned BlockMAD(const plane_t *yp, const filter_sys_t *sys,
                         int bx, int by, int bw, int bh, int sample_step)
{
    uint64_t sum = 0;
    unsigned count = 0;
    for (int y = 0; y < bh; y += sample_step)
    {
        const uint8_t *cur = yp->p_pixels + (by + y) * yp->i_pitch + bx;
        const uint8_t *prev = sys->prev_y + (size_t)(by + y) * sys->width + bx;
        for (int x = 0; x < bw; x += sample_step)
        {
            sum += (unsigned)abs((int)cur[x] - (int)prev[x]);
            ++count;
        }
    }
    return count ? (unsigned)((sum + count / 2u) / count) : 0u;
}

static unsigned PrevWeight(unsigned mad, unsigned static_thr,
                           unsigned motion_thr, unsigned max_weight)
{
    if (motion_thr <= static_thr)
        motion_thr = static_thr + 1u;
    if (mad <= static_thr)
        return max_weight;
    if (mad >= motion_thr)
        return 0u;
    return (max_weight * (motion_thr - mad) +
            (motion_thr - static_thr) / 2u) /
           (motion_thr - static_thr);
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic)
        return NULL;

    if (!atomic_load(&sys->enabled) || pic->i_planes <= Y_PLANE)
    {
        ResetState(sys);
        return pic;
    }

    plane_t *yp = &pic->p[Y_PLANE];
    const int width = __MIN(sys->width, yp->i_visible_pitch);
    const int height = __MIN(sys->height, yp->i_visible_lines);
    if (width <= 0 || height <= 0)
        return pic;

    if (!sys->have_prev)
    {
        SaveCurrentRaw(yp, sys);
        sys->have_prev = true;
        return pic;
    }

    const int block = atomic_load(&sys->block_size);
    const int sample_step = atomic_load(&sys->sample_step);
    const unsigned static_thr = (unsigned)atomic_load(&sys->static_threshold);
    const unsigned motion_thr = (unsigned)atomic_load(&sys->motion_threshold);
    const unsigned pixel_thr = (unsigned)atomic_load(&sys->pixel_threshold);
    const unsigned max_weight = (unsigned)atomic_load(&sys->max_prev_weight);

    uint64_t blended_pixels = 0;
    uint64_t total_pixels = 0;

    for (int by = 0; by < height; by += block)
    {
        const int bh = __MIN(block, height - by);
        for (int bx = 0; bx < width; bx += block)
        {
            const int bw = __MIN(block, width - bx);
            const unsigned mad = BlockMAD(yp, sys, bx, by, bw, bh, sample_step);
            const unsigned block_weight = PrevWeight(mad, static_thr, motion_thr, max_weight);

            for (int y = 0; y < bh; ++y)
            {
                uint8_t *cur = yp->p_pixels + (by + y) * yp->i_pitch + bx;
                uint8_t *prev = sys->prev_y + (size_t)(by + y) * sys->width + bx;
                for (int x = 0; x < bw; ++x)
                {
                    const unsigned cv = cur[x];
                    const unsigned pv = prev[x];
                    const unsigned residual = (unsigned)abs((int)cv - (int)pv);
                    prev[x] = (uint8_t)cv; /* Save RAW current before modifying output. */

                    if (block_weight && residual <= pixel_thr)
                    {
                        cur[x] = (uint8_t)(((100u - block_weight) * cv + block_weight * pv + 50u) / 100u);
                        ++blended_pixels;
                    }
                    ++total_pixels;
                }
            }
        }
    }

    ++sys->frame_count;
    if ((sys->frame_count % 100u) == 0u && total_pixels)
        msg_Dbg(filter, "Static Gate: blended=%llu/%llu (%llu%%)",
                (unsigned long long)blended_pixels,
                (unsigned long long)total_pixels,
                (unsigned long long)(100u * blended_pixels / total_pixels));

    return pic;
}
