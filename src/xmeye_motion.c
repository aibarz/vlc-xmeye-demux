/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye motion-compensated temporal filter for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * Goal: improve moving regions without recursively accumulating static areas.
 * The previous raw frame is block-matched against the current frame. Only
 * reliably matched moving luma blocks are blended after motion compensation.
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

#define FILTER_PREFIX "xmeyemc-"

struct filter_sys_t
{
    picture_t *prev;
    bool have_prev;

    atomic_int enabled;
    atomic_int block_size;
    atomic_int search_radius;
    atomic_int search_step;
    atomic_int motion_threshold;
    atomic_int match_threshold;
    atomic_int min_improve;
    atomic_int blend_prev;

    unsigned stats_frames;
    unsigned long long stats_blocks;
    unsigned long long stats_moving;
    unsigned long long stats_matched;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = {
    "enabled", "block-size", "search-radius", "search-step",
    "motion-threshold", "match-threshold", "min-improve", "blend-prev", NULL
};

vlc_module_begin()
    set_shortname("XMEye Motion Comp")
    set_description("Block motion-compensated temporal enhancement for moving regions")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Motion-compensated enhancement enabled",
             "Enable or bypass the motion-compensated filter.", false)
    add_integer_with_range(FILTER_PREFIX "block-size", 16, 8, 32,
             "Block size", "Motion-analysis block size in luma pixels.", false)
    add_integer_with_range(FILTER_PREFIX "search-radius", 8, 2, 24,
             "Search radius", "Maximum motion search radius in pixels.", false)
    add_integer_with_range(FILTER_PREFIX "search-step", 2, 1, 4,
             "Coarse search step", "Pixel step for the coarse block search; best candidate is refined at 1-pixel precision.", false)
    add_integer_with_range(FILTER_PREFIX "motion-threshold", 6, 0, 32,
             "Motion threshold", "Average luma difference above which a block is considered moving.", false)
    add_integer_with_range(FILTER_PREFIX "match-threshold", 18, 0, 64,
             "Match threshold", "Maximum average luma error accepted for a motion-compensated match.", false)
    add_integer_with_range(FILTER_PREFIX "min-improve", 10, 0, 50,
             "Minimum match improvement", "Minimum percent improvement over the same-position match before blending.", false)
    add_integer_with_range(FILTER_PREFIX "blend-prev", 30, 0, 50,
             "Previous-frame blend", "Percent contribution of the aligned previous frame in matched moving blocks.", false)
    add_shortcut("xmeyemc")
    set_callbacks(Create, Destroy)
vlc_module_end()

static unsigned block_sad_sampled(const plane_t *prev, const plane_t *cur,
                                  int px, int py, int cx, int cy,
                                  int w, int h, int sample_step,
                                  unsigned abort_above, unsigned *samples_out)
{
    unsigned sad = 0;
    unsigned samples = 0;

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

static void blend_luma_block(plane_t *dst, const plane_t *cur, const plane_t *prev,
                             int cx, int cy, int px, int py, int w, int h,
                             int previous_percent)
{
    const unsigned wp = (unsigned)previous_percent;
    const unsigned wc = 100u - wp;

    for (int y = 0; y < h; ++y)
    {
        uint8_t *pd = dst->p_pixels + (cy + y) * dst->i_pitch + cx;
        const uint8_t *pc = cur->p_pixels + (cy + y) * cur->i_pitch + cx;
        const uint8_t *pp = prev->p_pixels + (py + y) * prev->i_pitch + px;
        for (int x = 0; x < w; ++x)
            pd[x] = (uint8_t)((wc * (unsigned)pc[x] + wp * (unsigned)pp[x] + 50u) / 100u);
    }
}

static int Create(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    sys->prev = picture_NewFromFormat(&filter->fmt_in.video);
    if (!sys->prev || sys->prev->i_planes < 1 ||
        sys->prev->p[Y_PLANE].i_visible_pitch != (int)filter->fmt_in.video.i_visible_width)
    {
        if (sys->prev) picture_Release(sys->prev);
        free(sys);
        msg_Err(filter, "XMEye Motion Comp requires CPU-accessible planar 8-bit luma");
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    config_ChainParse(filter, FILTER_PREFIX, filter_options, filter->p_cfg);

    atomic_init(&sys->enabled, var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    atomic_init(&sys->block_size, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "block-size"), 8, 32));
    atomic_init(&sys->search_radius, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "search-radius"), 2, 24));
    atomic_init(&sys->search_step, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "search-step"), 1, 4));
    atomic_init(&sys->motion_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "motion-threshold"), 0, 32));
    atomic_init(&sys->match_threshold, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "match-threshold"), 0, 64));
    atomic_init(&sys->min_improve, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "min-improve"), 0, 50));
    atomic_init(&sys->blend_prev, VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "blend-prev"), 0, 50));

    const char *names[] = { "enabled", "block-size", "search-radius", "search-step",
                            "motion-threshold", "match-threshold", "min-improve", "blend-prev", NULL };
    for (int i = 0; names[i]; ++i)
    {
        char full[64];
        snprintf(full, sizeof(full), FILTER_PREFIX "%s", names[i]);
        var_AddCallback(filter, full, OptionCallback, sys);
    }

    filter->pf_video_filter = Filter;
    filter->pf_flush = Flush;
    msg_Info(filter, "XMEye Motion Comp ready: 20fps-preserving block compensation enabled");
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys) return;

    const char *names[] = { "enabled", "block-size", "search-radius", "search-step",
                            "motion-threshold", "match-threshold", "min-improve", "blend-prev", NULL };
    for (int i = 0; names[i]; ++i)
    {
        char full[64];
        snprintf(full, sizeof(full), FILTER_PREFIX "%s", names[i]);
        var_DelCallback(filter, full, OptionCallback, sys);
    }

    if (sys->prev) picture_Release(sys->prev);
    free(sys);
}

static void Flush(filter_t *filter)
{
    filter_sys_t *sys = filter->p_sys;
    sys->have_prev = false;
    sys->stats_frames = 0;
    sys->stats_blocks = sys->stats_moving = sys->stats_matched = 0;
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
        sys->have_prev = false;
    }
    else if (!strcmp(name, FILTER_PREFIX "block-size"))
        atomic_store(&sys->block_size, VLC_CLIP(newval.i_int, 8, 32));
    else if (!strcmp(name, FILTER_PREFIX "search-radius"))
        atomic_store(&sys->search_radius, VLC_CLIP(newval.i_int, 2, 24));
    else if (!strcmp(name, FILTER_PREFIX "search-step"))
        atomic_store(&sys->search_step, VLC_CLIP(newval.i_int, 1, 4));
    else if (!strcmp(name, FILTER_PREFIX "motion-threshold"))
        atomic_store(&sys->motion_threshold, VLC_CLIP(newval.i_int, 0, 32));
    else if (!strcmp(name, FILTER_PREFIX "match-threshold"))
        atomic_store(&sys->match_threshold, VLC_CLIP(newval.i_int, 0, 64));
    else if (!strcmp(name, FILTER_PREFIX "min-improve"))
        atomic_store(&sys->min_improve, VLC_CLIP(newval.i_int, 0, 50));
    else if (!strcmp(name, FILTER_PREFIX "blend-prev"))
        atomic_store(&sys->blend_prev, VLC_CLIP(newval.i_int, 0, 50));

    return VLC_SUCCESS;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic) return NULL;

    if (!atomic_load(&sys->enabled))
    {
        sys->have_prev = false;
        return pic;
    }

    if (!sys->have_prev)
    {
        picture_CopyPixels(sys->prev, pic);
        sys->have_prev = true;
        return pic;
    }

    picture_t *out = filter_NewPicture(filter);
    if (!out)
    {
        picture_CopyPixels(sys->prev, pic);
        return pic;
    }

    picture_CopyPixels(out, pic);
    picture_CopyProperties(out, pic);

    const plane_t *cur_y = &pic->p[Y_PLANE];
    const plane_t *prev_y = &sys->prev->p[Y_PLANE];
    plane_t *out_y = &out->p[Y_PLANE];

    const int width = __MIN(cur_y->i_visible_pitch, prev_y->i_visible_pitch);
    const int height = __MIN(cur_y->i_visible_lines, prev_y->i_visible_lines);
    const int block = atomic_load(&sys->block_size);
    const int radius = atomic_load(&sys->search_radius);
    const int coarse_step = atomic_load(&sys->search_step);
    const int motion_threshold = atomic_load(&sys->motion_threshold);
    const int match_threshold = atomic_load(&sys->match_threshold);
    const int min_improve = atomic_load(&sys->min_improve);
    const int blend_prev = atomic_load(&sys->blend_prev);
    const int sample_step = 2;

    unsigned long long blocks = 0, moving = 0, matched = 0;

    for (int by = 0; by < height; by += block)
    {
        const int bh = __MIN(block, height - by);
        for (int bx = 0; bx < width; bx += block)
        {
            const int bw = __MIN(block, width - bx);
            ++blocks;

            unsigned same_samples = 0;
            const unsigned same_sad = block_sad_sampled(prev_y, cur_y,
                                                        bx, by, bx, by, bw, bh,
                                                        sample_step, UINT_MAX, &same_samples);
            if (!same_samples)
                continue;

            const unsigned same_avg = same_sad / same_samples;
            if ((int)same_avg <= motion_threshold)
                continue; /* Static: keep current frame untouched. */

            ++moving;
            unsigned best_sad = same_sad;
            int best_dx = 0, best_dy = 0;

            for (int dy = -radius; dy <= radius; dy += coarse_step)
            {
                const int py = by + dy;
                if (py < 0 || py + bh > height) continue;
                for (int dx = -radius; dx <= radius; dx += coarse_step)
                {
                    const int px = bx + dx;
                    if (px < 0 || px + bw > width) continue;
                    if (dx == 0 && dy == 0) continue;

                    const unsigned sad = block_sad_sampled(prev_y, cur_y,
                                                           px, py, bx, by, bw, bh,
                                                           sample_step, best_sad, NULL);
                    if (sad < best_sad)
                    {
                        best_sad = sad;
                        best_dx = dx;
                        best_dy = dy;
                    }
                }
            }

            /* One-pixel refinement around the best coarse vector. */
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

                        const unsigned sad = block_sad_sampled(prev_y, cur_y,
                                                               px, py, bx, by, bw, bh,
                                                               sample_step, best_sad, NULL);
                        if (sad < best_sad)
                        {
                            best_sad = sad;
                            best_dx = dx;
                            best_dy = dy;
                        }
                    }
                }
            }

            const unsigned best_avg = best_sad / same_samples;
            const bool error_ok = (int)best_avg <= match_threshold;
            const bool improvement_ok =
                best_sad * 100u <= same_sad * (unsigned)(100 - min_improve);

            if (error_ok && improvement_ok)
            {
                blend_luma_block(out_y, cur_y, prev_y,
                                 bx, by, bx + best_dx, by + best_dy,
                                 bw, bh, blend_prev);
                ++matched;
            }
        }
    }

    picture_CopyPixels(sys->prev, pic); /* Keep RAW current frame, never filtered output. */
    sys->have_prev = true;
    picture_Release(pic);

    sys->stats_blocks += blocks;
    sys->stats_moving += moving;
    sys->stats_matched += matched;
    ++sys->stats_frames;
    if (sys->stats_frames >= 20)
    {
        const unsigned long long mbp = sys->stats_blocks ? (100ull * sys->stats_moving / sys->stats_blocks) : 0;
        const unsigned long long mcp = sys->stats_moving ? (100ull * sys->stats_matched / sys->stats_moving) : 0;
        msg_Info(filter, "XMEye MC stats/20f: blocks=%llu moving=%llu (%llu%%) matched=%llu (%llu%% of moving)",
                 sys->stats_blocks, sys->stats_moving, mbp, sys->stats_matched, mcp);
        sys->stats_frames = 0;
        sys->stats_blocks = sys->stats_moving = sys->stats_matched = 0;
    }

    return out;
}
