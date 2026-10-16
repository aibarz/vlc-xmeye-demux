/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye edge-preserving temporal enhancement filter for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * 20 fps preserving block-adaptive filter.
 * Temporal fusion is applied to low-frequency luma while current-frame
 * high-frequency detail is retained, avoiding softened moving edges.
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_atomic.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define FILTER_PREFIX "xmeyeedge-"

struct filter_sys_t
{
    picture_t *prev;
    bool have_prev;
    atomic_int enabled;
    atomic_int focus;
    atomic_int block_size;
    atomic_int search_radius;
    atomic_int search_step;
    atomic_int motion_threshold;
    atomic_int match_threshold;
    atomic_int min_improve;
    atomic_int static_blend;
    atomic_int motion_blend;
    atomic_int detail_retain;
    unsigned stats_frames;
    unsigned long long stats_blocks;
    unsigned long long stats_static;
    unsigned long long stats_moving;
    unsigned long long stats_matched;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = {
    "enabled", "focus", "block-size", "search-radius", "search-step",
    "motion-threshold", "match-threshold", "min-improve",
    "static-blend", "motion-blend", "detail-retain", NULL
};

vlc_module_begin()
    set_shortname("XMEye Edge Preserve")
    set_description("Edge-preserving static/motion temporal enhancement")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Edge-preserving enhancement enabled", "Enable or bypass the filter.", false)
    add_integer_with_range(FILTER_PREFIX "focus", 75, 0, 100,
             "Static-to-motion focus", "0 favors static regions; 100 favors moving regions.", false)
    add_integer_with_range(FILTER_PREFIX "block-size", 16, 8, 32,
             "Block size", "Motion-analysis block size in luma pixels.", false)
    add_integer_with_range(FILTER_PREFIX "search-radius", 8, 2, 24,
             "Search radius", "Maximum motion search radius in pixels.", false)
    add_integer_with_range(FILTER_PREFIX "search-step", 2, 1, 4,
             "Coarse search step", "Coarse block-search step; refined at one pixel.", false)
    add_integer_with_range(FILTER_PREFIX "motion-threshold", 6, 0, 32,
             "Motion threshold", "Average luma difference above which a block is moving.", false)
    add_integer_with_range(FILTER_PREFIX "match-threshold", 18, 0, 64,
             "Match threshold", "Maximum average luma error for a motion match.", false)
    add_integer_with_range(FILTER_PREFIX "min-improve", 10, 0, 50,
             "Minimum match improvement", "Required percent improvement over zero-motion match.", false)
    add_integer_with_range(FILTER_PREFIX "static-blend", 30, 0, 50,
             "Static low-frequency blend", "Maximum previous-frame contribution in static areas.", false)
    add_integer_with_range(FILTER_PREFIX "motion-blend", 30, 0, 50,
             "Motion low-frequency blend", "Maximum aligned previous-frame contribution in moving areas.", false)
    add_integer_with_range(FILTER_PREFIX "detail-retain", 100, 0, 150,
             "Current detail retention", "Percent of current-frame high-frequency detail retained.", false)
    add_shortcut("xmeyeedge")
    set_callbacks(Create, Destroy)
vlc_module_end()

static inline uint8_t clip_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return (uint8_t)v;
}

static inline int gauss3(const plane_t *p, int x, int y)
{
    const uint8_t *r0 = p->p_pixels + (y - 1) * p->i_pitch;
    const uint8_t *r1 = p->p_pixels + y * p->i_pitch;
    const uint8_t *r2 = p->p_pixels + (y + 1) * p->i_pitch;
    return (r0[x - 1] + 2 * r0[x] + r0[x + 1] +
            2 * r1[x - 1] + 4 * r1[x] + 2 * r1[x + 1] +
            r2[x - 1] + 2 * r2[x] + r2[x + 1] + 8) >> 4;
}

static unsigned block_sad_sampled(const plane_t *prev, const plane_t *cur,
                                  int px, int py, int cx, int cy,
                                  int w, int h, int sample_step,
                                  unsigned abort_above, unsigned *samples_out)
{
    unsigned sad = 0, samples = 0;
    for (int y = 0; y < h; y += sample_step)
    {
        const uint8_t *pp = prev->p_pixels + (py + y) * prev->i_pitch + px;
        const uint8_t *pc = cur->p_pixels + (cy + y) * cur->i_pitch + cx;
        for (int x = 0; x < w; x += sample_step)
        {
            int d = (int)pp[x] - (int)pc[x];
            if (d < 0) d = -d;
            sad += (unsigned)d;
            ++samples;
            if (abort_above != UINT_MAX && sad > abort_above)
            {
                if (samples_out) *samples_out = samples;
                return sad;
            }
        }
    }
    if (samples_out) *samples_out = samples;
    return sad;
}

static void edge_preserve_blend(plane_t *dst, const plane_t *cur, const plane_t *prev,
                                int cx, int cy, int px, int py, int w, int h,
                                int previous_percent, int detail_retain,
                                int width, int height)
{
    if (previous_percent <= 0) return;
    const int wp = previous_percent;
    const int wc = 100 - wp;

    for (int y = 0; y < h; ++y)
    {
        const int c_y = cy + y;
        const int p_y = py + y;
        if (c_y <= 0 || c_y >= height - 1 || p_y <= 0 || p_y >= height - 1)
            continue;

        uint8_t *pd = dst->p_pixels + c_y * dst->i_pitch;
        const uint8_t *pc = cur->p_pixels + c_y * cur->i_pitch;

        for (int x = 0; x < w; ++x)
        {
            const int c_x = cx + x;
            const int p_x = px + x;
            if (c_x <= 0 || c_x >= width - 1 || p_x <= 0 || p_x >= width - 1)
                continue;

            const int cur_low = gauss3(cur, c_x, c_y);
            const int prev_low = gauss3(prev, p_x, p_y);
            const int low_mix = (wc * cur_low + wp * prev_low + 50) / 100;
            const int cur_detail = (int)pc[c_x] - cur_low;
            const int detail = (cur_detail * detail_retain + (cur_detail >= 0 ? 50 : -50)) / 100;
            pd[c_x] = clip_u8(low_mix + detail);
        }
    }
}

static int Create(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;

    sys->prev = picture_NewFromFormat(&filter->fmt_in.video);
    if (!sys->prev || sys->prev->i_planes < 1 ||
        sys->prev->p[Y_PLANE].i_pixel_pitch != 1)
    {
        if (sys->prev) picture_Release(sys->prev);
        free(sys);
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    config_ChainParse(filter, FILTER_PREFIX, filter_options, filter->p_cfg);
    atomic_init(&sys->enabled, var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    atomic_init(&sys->focus, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "focus"), 0, 100));
    atomic_init(&sys->block_size, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "block-size"), 8, 32));
    atomic_init(&sys->search_radius, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "search-radius"), 2, 24));
    atomic_init(&sys->search_step, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "search-step"), 1, 4));
    atomic_init(&sys->motion_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "motion-threshold"), 0, 32));
    atomic_init(&sys->match_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "match-threshold"), 0, 64));
    atomic_init(&sys->min_improve, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "min-improve"), 0, 50));
    atomic_init(&sys->static_blend, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "static-blend"), 0, 50));
    atomic_init(&sys->motion_blend, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "motion-blend"), 0, 50));
    atomic_init(&sys->detail_retain, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "detail-retain"), 0, 150));

    const char *names[] = { "enabled", "focus", "block-size", "search-radius", "search-step",
                            "motion-threshold", "match-threshold", "min-improve",
                            "static-blend", "motion-blend", "detail-retain", NULL };
    for (int i = 0; names[i]; ++i)
    {
        char full[64];
        snprintf(full, sizeof(full), FILTER_PREFIX "%s", names[i]);
        var_AddCallback(filter, full, OptionCallback, sys);
    }

    filter->pf_video_filter = Filter;
    filter->pf_flush = Flush;
    msg_Info(filter, "XMEye Edge Preserve ready");
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys) return;
    if (sys->prev) picture_Release(sys->prev);
    free(sys);
}

static void Flush(filter_t *filter)
{
    filter_sys_t *sys = filter->p_sys;
    sys->have_prev = false;
    sys->stats_frames = 0;
    sys->stats_blocks = sys->stats_static = sys->stats_moving = sys->stats_matched = 0;
}

static int OptionCallback(vlc_object_t *obj, const char *name,
                          vlc_value_t oldval, vlc_value_t newval, void *data)
{
    VLC_UNUSED(obj); VLC_UNUSED(oldval);
    filter_sys_t *sys = data;
    if (!strcmp(name, FILTER_PREFIX "enabled")) { atomic_store(&sys->enabled, newval.b_bool ? 1 : 0); sys->have_prev = false; }
    else if (!strcmp(name, FILTER_PREFIX "focus")) atomic_store(&sys->focus, VLC_CLIP(newval.i_int, 0, 100));
    else if (!strcmp(name, FILTER_PREFIX "block-size")) atomic_store(&sys->block_size, VLC_CLIP(newval.i_int, 8, 32));
    else if (!strcmp(name, FILTER_PREFIX "search-radius")) atomic_store(&sys->search_radius, VLC_CLIP(newval.i_int, 2, 24));
    else if (!strcmp(name, FILTER_PREFIX "search-step")) atomic_store(&sys->search_step, VLC_CLIP(newval.i_int, 1, 4));
    else if (!strcmp(name, FILTER_PREFIX "motion-threshold")) atomic_store(&sys->motion_threshold, VLC_CLIP(newval.i_int, 0, 32));
    else if (!strcmp(name, FILTER_PREFIX "match-threshold")) atomic_store(&sys->match_threshold, VLC_CLIP(newval.i_int, 0, 64));
    else if (!strcmp(name, FILTER_PREFIX "min-improve")) atomic_store(&sys->min_improve, VLC_CLIP(newval.i_int, 0, 50));
    else if (!strcmp(name, FILTER_PREFIX "static-blend")) atomic_store(&sys->static_blend, VLC_CLIP(newval.i_int, 0, 50));
    else if (!strcmp(name, FILTER_PREFIX "motion-blend")) atomic_store(&sys->motion_blend, VLC_CLIP(newval.i_int, 0, 50));
    else if (!strcmp(name, FILTER_PREFIX "detail-retain")) atomic_store(&sys->detail_retain, VLC_CLIP(newval.i_int, 0, 150));
    return VLC_SUCCESS;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic) return NULL;
    if (!atomic_load(&sys->enabled)) { sys->have_prev = false; return pic; }

    if (!sys->have_prev)
    {
        picture_CopyPixels(sys->prev, pic);
        sys->have_prev = true;
        return pic;
    }

    picture_t *out = filter_NewPicture(filter);
    if (!out) { picture_CopyPixels(sys->prev, pic); return pic; }
    picture_CopyPixels(out, pic);
    picture_CopyProperties(out, pic);

    const plane_t *cur_y = &pic->p[Y_PLANE];
    const plane_t *prev_y = &sys->prev->p[Y_PLANE];
    plane_t *out_y = &out->p[Y_PLANE];
    const int width = __MIN(cur_y->i_visible_pitch, prev_y->i_visible_pitch);
    const int height = __MIN(cur_y->i_visible_lines, prev_y->i_visible_lines);
    const int focus = atomic_load(&sys->focus);
    const int block = atomic_load(&sys->block_size);
    const int radius = atomic_load(&sys->search_radius);
    const int coarse_step = atomic_load(&sys->search_step);
    const int motion_threshold = atomic_load(&sys->motion_threshold);
    const int match_threshold = atomic_load(&sys->match_threshold);
    const int min_improve = atomic_load(&sys->min_improve);
    const int static_weight = ((100 - focus) * atomic_load(&sys->static_blend) + 50) / 100;
    const int motion_weight = (focus * atomic_load(&sys->motion_blend) + 50) / 100;
    const int detail_retain = atomic_load(&sys->detail_retain);
    const int sample_step = 2;
    unsigned long long blocks = 0, statics = 0, moving = 0, matched = 0;

    for (int by = 0; by < height; by += block)
    {
        const int bh = __MIN(block, height - by);
        for (int bx = 0; bx < width; bx += block)
        {
            const int bw = __MIN(block, width - bx);
            ++blocks;
            unsigned same_samples = 0;
            const unsigned same_sad = block_sad_sampled(prev_y, cur_y, bx, by, bx, by, bw, bh,
                                                        sample_step, UINT_MAX, &same_samples);
            if (!same_samples) continue;
            const unsigned same_avg = same_sad / same_samples;

            if ((int)same_avg <= motion_threshold)
            {
                ++statics;
                if (static_weight > 0)
                    edge_preserve_blend(out_y, cur_y, prev_y, bx, by, bx, by, bw, bh,
                                        static_weight, detail_retain, width, height);
                continue;
            }

            ++moving;
            if (motion_weight <= 0) continue;
            unsigned best_sad = same_sad;
            int best_dx = 0, best_dy = 0;

            for (int dy = -radius; dy <= radius; dy += coarse_step)
            {
                const int py = by + dy;
                if (py < 0 || py + bh > height) continue;
                for (int dx = -radius; dx <= radius; dx += coarse_step)
                {
                    const int px = bx + dx;
                    if (px < 0 || px + bw > width || (dx == 0 && dy == 0)) continue;
                    const unsigned sad = block_sad_sampled(prev_y, cur_y, px, py, bx, by, bw, bh,
                                                           sample_step, best_sad, NULL);
                    if (sad < best_sad) { best_sad = sad; best_dx = dx; best_dy = dy; }
                }
            }

            if (coarse_step > 1)
            {
                const int refine = coarse_step - 1;
                const int base_dx = best_dx, base_dy = best_dy;
                for (int dy = base_dy - refine; dy <= base_dy + refine; ++dy)
                {
                    const int py = by + dy;
                    if (py < 0 || py + bh > height) continue;
                    for (int dx = base_dx - refine; dx <= base_dx + refine; ++dx)
                    {
                        const int px = bx + dx;
                        if (px < 0 || px + bw > width) continue;
                        const unsigned sad = block_sad_sampled(prev_y, cur_y, px, py, bx, by, bw, bh,
                                                               sample_step, best_sad, NULL);
                        if (sad < best_sad) { best_sad = sad; best_dx = dx; best_dy = dy; }
                    }
                }
            }

            const unsigned best_avg = best_sad / same_samples;
            const bool error_ok = (int)best_avg <= match_threshold;
            const bool improvement_ok = best_sad * 100u <= same_sad * (unsigned)(100 - min_improve);
            if (error_ok && improvement_ok)
            {
                edge_preserve_blend(out_y, cur_y, prev_y,
                                    bx, by, bx + best_dx, by + best_dy, bw, bh,
                                    motion_weight, detail_retain, width, height);
                ++matched;
            }
        }
    }

    picture_CopyPixels(sys->prev, pic);
    sys->have_prev = true;
    picture_Release(pic);

    sys->stats_blocks += blocks;
    sys->stats_static += statics;
    sys->stats_moving += moving;
    sys->stats_matched += matched;
    if (++sys->stats_frames >= 20)
    {
        msg_Info(filter, "edge stats: blocks=%llu static=%llu moving=%llu matched=%llu focus=%d detail=%d",
                 sys->stats_blocks, sys->stats_static, sys->stats_moving, sys->stats_matched,
                 focus, detail_retain);
        sys->stats_frames = 0;
        sys->stats_blocks = sys->stats_static = sys->stats_moving = sys->stats_matched = 0;
    }
    return out;
}
