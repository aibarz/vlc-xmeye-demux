/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye strict static-only temporal denoiser for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * V2: one input -> one output, cadence unchanged.
 * Temporal blending is allowed ONLY in blocks that are confidently static.
 * Any uncertain/moving block and its configurable neighbours remain 100% current frame.
 * The previous reference is always RAW luma, never filtered output.
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
    uint8_t *motion_map;
    size_t motion_capacity;
    int width;
    int height;
    bool have_prev;
    uint64_t frame_count;

    atomic_int enabled;
    atomic_int block_size;
    atomic_int sample_step;
    atomic_int static_threshold;
    atomic_int motion_threshold;
    atomic_int max_changed_percent;
    atomic_int pixel_threshold;
    atomic_int dilate_blocks;
    atomic_int max_prev_weight;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = {
    "enabled", "block-size", "sample-step", "static-threshold",
    "motion-threshold", "max-changed-percent", "pixel-threshold",
    "dilate-blocks", "max-prev-weight", NULL
};

vlc_module_begin()
    set_shortname("XMEye Static Gate V2")
    set_description("Strict static-only temporal luma denoiser; uncertain/moving regions remain current-frame only")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Static temporal gate enabled", "Enable strict static-only temporal blending.", false)
    add_integer_with_range(FILTER_PREFIX "block-size", 16, 8, 32,
                           "Motion block size", "Luma block size for temporal motion classification.", false)
    add_integer_with_range(FILTER_PREFIX "sample-step", 2, 1, 4,
                           "Motion sample step", "Sampling stride inside each motion block.", false)
    add_integer_with_range(FILTER_PREFIX "static-threshold", 2, 0, 16,
                           "Static MAD threshold", "Maximum block mean luma difference allowed for temporal blending.", false)
    add_integer_with_range(FILTER_PREFIX "motion-threshold", 4, 1, 32,
                           "Changed-sample threshold", "A sampled pixel changing by this amount counts as motion evidence.", false)
    add_integer_with_range(FILTER_PREFIX "max-changed-percent", 8, 0, 50,
                           "Maximum changed samples", "Maximum percentage of changed samples in a block still considered static.", false)
    add_integer_with_range(FILTER_PREFIX "pixel-threshold", 2, 1, 32,
                           "Per-pixel residual gate", "Pixels changing more than this value always use the current frame.", false)
    add_integer_with_range(FILTER_PREFIX "dilate-blocks", 1, 0, 2,
                           "Motion protection radius", "Protect this many neighbouring blocks around detected motion.", false)
    add_integer_with_range(FILTER_PREFIX "max-prev-weight", 50, 0, 60,
                           "Maximum previous-frame weight", "Previous RAW luma percentage used only in confidently static pixels.", false)
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

    /* Capacity for the smallest selectable block size (8x8). */
    const size_t max_cols = ((size_t)sys->width + 7u) / 8u;
    const size_t max_rows = ((size_t)sys->height + 7u) / 8u;
    sys->motion_capacity = max_cols * max_rows;
    sys->motion_map = malloc(sys->motion_capacity);

    if (!sys->prev_y || !sys->motion_map)
    {
        free(sys->motion_map);
        free(sys->prev_y);
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
    atomic_init(&sys->max_changed_percent, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "max-changed-percent"), 0, 50));
    atomic_init(&sys->pixel_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "pixel-threshold"), 1, 32));
    atomic_init(&sys->dilate_blocks, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "dilate-blocks"), 0, 2));
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
    msg_Info(filter, "XMEye Static Gate V2 ready: block=%d static=%d changed=%d maxchg=%d%% pixel=%d dilate=%d prev=%d%%",
             atomic_load(&sys->block_size), atomic_load(&sys->static_threshold),
             atomic_load(&sys->motion_threshold), atomic_load(&sys->max_changed_percent),
             atomic_load(&sys->pixel_threshold), atomic_load(&sys->dilate_blocks),
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

    free(sys->motion_map);
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
    else if (!strcmp(name, FILTER_PREFIX "max-changed-percent"))
        atomic_store(&sys->max_changed_percent, VLC_CLIP(newval.i_int, 0, 50));
    else if (!strcmp(name, FILTER_PREFIX "pixel-threshold"))
        atomic_store(&sys->pixel_threshold, VLC_CLIP(newval.i_int, 1, 32));
    else if (!strcmp(name, FILTER_PREFIX "dilate-blocks"))
        atomic_store(&sys->dilate_blocks, VLC_CLIP(newval.i_int, 0, 2));
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

typedef struct
{
    unsigned mad;
    unsigned changed_percent;
} block_metrics_t;

static block_metrics_t BlockMetrics(const plane_t *yp, const filter_sys_t *sys,
                                    int bx, int by, int bw, int bh,
                                    int sample_step, unsigned changed_threshold)
{
    uint64_t sum = 0;
    unsigned count = 0;
    unsigned changed = 0;

    for (int y = 0; y < bh; y += sample_step)
    {
        const uint8_t *cur = yp->p_pixels + (by + y) * yp->i_pitch + bx;
        const uint8_t *prev = sys->prev_y + (size_t)(by + y) * sys->width + bx;
        for (int x = 0; x < bw; x += sample_step)
        {
            const unsigned d = (unsigned)abs((int)cur[x] - (int)prev[x]);
            sum += d;
            if (d >= changed_threshold)
                ++changed;
            ++count;
        }
    }

    block_metrics_t m = {0, 0};
    if (count)
    {
        m.mad = (unsigned)((sum + count / 2u) / count);
        m.changed_percent = (100u * changed + count / 2u) / count;
    }
    return m;
}

static bool BlockProtected(const uint8_t *map, int cols, int rows,
                           int cx, int cy, int radius)
{
    const int y0 = __MAX(0, cy - radius);
    const int y1 = __MIN(rows - 1, cy + radius);
    const int x0 = __MAX(0, cx - radius);
    const int x1 = __MIN(cols - 1, cx + radius);

    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
            if (map[(size_t)y * cols + x])
                return true;
    return false;
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
    const unsigned changed_thr = (unsigned)atomic_load(&sys->motion_threshold);
    const unsigned max_changed = (unsigned)atomic_load(&sys->max_changed_percent);
    const unsigned pixel_thr = (unsigned)atomic_load(&sys->pixel_threshold);
    const int dilate = atomic_load(&sys->dilate_blocks);
    const unsigned max_weight = (unsigned)atomic_load(&sys->max_prev_weight);

    const int cols = (width + block - 1) / block;
    const int rows = (height + block - 1) / block;
    const size_t map_size = (size_t)cols * (size_t)rows;
    if (map_size > sys->motion_capacity)
    {
        SaveCurrentRaw(yp, sys);
        return pic;
    }

    /* Pass 1: classify blocks. 1 means moving OR uncertain. */
    for (int cy = 0; cy < rows; ++cy)
    {
        const int by = cy * block;
        const int bh = __MIN(block, height - by);
        for (int cx = 0; cx < cols; ++cx)
        {
            const int bx = cx * block;
            const int bw = __MIN(block, width - bx);
            const block_metrics_t m = BlockMetrics(yp, sys, bx, by, bw, bh,
                                                   sample_step, changed_thr);
            const bool confident_static = m.mad <= static_thr &&
                                          m.changed_percent <= max_changed;
            sys->motion_map[(size_t)cy * cols + cx] = confident_static ? 0u : 1u;
        }
    }

    uint64_t blended_pixels = 0;
    uint64_t protected_blocks = 0;

    /* Pass 2: blend only blocks that remain static after motion-mask dilation. */
    for (int cy = 0; cy < rows; ++cy)
    {
        const int by = cy * block;
        const int bh = __MIN(block, height - by);
        for (int cx = 0; cx < cols; ++cx)
        {
            const int bx = cx * block;
            const int bw = __MIN(block, width - bx);
            const bool protected = BlockProtected(sys->motion_map, cols, rows, cx, cy, dilate);
            if (protected)
                ++protected_blocks;

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

                    if (!protected && max_weight && residual <= pixel_thr)
                    {
                        cur[x] = (uint8_t)(((100u - max_weight) * cv + max_weight * pv + 50u) / 100u);
                        ++blended_pixels;
                    }
                }
            }
        }
    }

    ++sys->frame_count;
    if ((sys->frame_count % 100u) == 0u)
        msg_Dbg(filter, "Static Gate V2: protected_blocks=%llu/%llu blended_pixels=%llu",
                (unsigned long long)protected_blocks,
                (unsigned long long)map_size,
                (unsigned long long)blended_pixels);

    return pic;
}
