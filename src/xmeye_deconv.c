/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye Laplacian deconvolution-style edge restoration filter for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * Goal: recover edge slope from mildly defocused surveillance video without
 * temporal averaging. This is a bounded spatial inverse-blur approximation,
 * not generative reconstruction.
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

#define FILTER_PREFIX "xmeyedeconv-"

static const char *const filter_options[] = {
    "enabled", "strength", "threshold", "correction-limit", "polarity", NULL
};

struct filter_sys_t
{
    atomic_int enabled;
    atomic_int strength;
    atomic_int threshold;
    atomic_int correction_limit;
    atomic_int polarity;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

vlc_module_begin()
    set_shortname("XMEye Deconv")
    set_description("Bounded Laplacian edge restoration for mildly defocused XMEye video")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Deconvolution enabled", "Enable or bypass the filter.", false)
    add_integer_with_range(FILTER_PREFIX "strength", 30, 0, 100,
             "Restoration strength", "Laplacian edge restoration gain.", false)
    add_integer_with_range(FILTER_PREFIX "threshold", 3, 0, 32,
             "Edge threshold", "Minimum absolute Laplacian required before correction.", false)
    add_integer_with_range(FILTER_PREFIX "correction-limit", 10, 1, 32,
             "Correction limit", "Maximum absolute luma correction per pixel.", false)
    add_integer_with_range(FILTER_PREFIX "polarity", 1, -1, 1,
             "Correction polarity", "+1 restores edges; -1 is diagnostic inverse polarity.", false)
    add_shortcut("xmeyedeconv")
    set_callbacks(Create, Destroy)
vlc_module_end()

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
    atomic_init(&sys->enabled,
                var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    atomic_init(&sys->strength,
                VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "strength"), 0, 100));
    atomic_init(&sys->threshold,
                VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "threshold"), 0, 32));
    atomic_init(&sys->correction_limit,
                VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "correction-limit"), 1, 32));
    atomic_init(&sys->polarity,
                VLC_CLIP(var_CreateGetIntegerCommand(filter, FILTER_PREFIX "polarity"), -1, 1));

    var_AddCallback(filter, FILTER_PREFIX "enabled", OptionCallback, sys);
    var_AddCallback(filter, FILTER_PREFIX "strength", OptionCallback, sys);
    var_AddCallback(filter, FILTER_PREFIX "threshold", OptionCallback, sys);
    var_AddCallback(filter, FILTER_PREFIX "correction-limit", OptionCallback, sys);
    var_AddCallback(filter, FILTER_PREFIX "polarity", OptionCallback, sys);

    filter->pf_video_filter = Filter;
    msg_Info(filter, "XMEye Deconv ready: strength=%d threshold=%d limit=%d polarity=%d",
             atomic_load(&sys->strength), atomic_load(&sys->threshold),
             atomic_load(&sys->correction_limit), atomic_load(&sys->polarity));
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys) return;

    var_DelCallback(filter, FILTER_PREFIX "enabled", OptionCallback, sys);
    var_DelCallback(filter, FILTER_PREFIX "strength", OptionCallback, sys);
    var_DelCallback(filter, FILTER_PREFIX "threshold", OptionCallback, sys);
    var_DelCallback(filter, FILTER_PREFIX "correction-limit", OptionCallback, sys);
    var_DelCallback(filter, FILTER_PREFIX "polarity", OptionCallback, sys);
    free(sys);
}

static int OptionCallback(vlc_object_t *obj, const char *name,
                          vlc_value_t oldval, vlc_value_t newval, void *data)
{
    VLC_UNUSED(obj); VLC_UNUSED(oldval);
    filter_sys_t *sys = data;

    if (!strcmp(name, FILTER_PREFIX "enabled"))
        atomic_store(&sys->enabled, newval.b_bool ? 1 : 0);
    else if (!strcmp(name, FILTER_PREFIX "strength"))
        atomic_store(&sys->strength, VLC_CLIP(newval.i_int, 0, 100));
    else if (!strcmp(name, FILTER_PREFIX "threshold"))
        atomic_store(&sys->threshold, VLC_CLIP(newval.i_int, 0, 32));
    else if (!strcmp(name, FILTER_PREFIX "correction-limit"))
        atomic_store(&sys->correction_limit, VLC_CLIP(newval.i_int, 1, 32));
    else if (!strcmp(name, FILTER_PREFIX "polarity"))
        atomic_store(&sys->polarity, VLC_CLIP(newval.i_int, -1, 1));
    return VLC_SUCCESS;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic || !atomic_load(&sys->enabled))
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
    const int threshold = atomic_load(&sys->threshold);
    const int limit = atomic_load(&sys->correction_limit);
    const int polarity = atomic_load(&sys->polarity);

    if (strength > 0 && polarity != 0 && width >= 3 && height >= 3)
    {
        for (int y = 1; y < height - 1; ++y)
        {
            const uint8_t *up = src->p_pixels + (y - 1) * src->i_pitch;
            const uint8_t *mid = src->p_pixels + y * src->i_pitch;
            const uint8_t *dn = src->p_pixels + (y + 1) * src->i_pitch;
            uint8_t *d = dst->p_pixels + y * dst->i_pitch;

            for (int x = 1; x < width - 1; ++x)
            {
                const int c = mid[x];
                const int lap = 4 * c - (int)mid[x - 1] - (int)mid[x + 1] - (int)up[x] - (int)dn[x];
                const int abs_lap = lap < 0 ? -lap : lap;
                if (abs_lap < threshold)
                    continue;

                int corr = (lap * strength + (lap >= 0 ? 50 : -50)) / 100;
                if (corr > limit) corr = limit;
                if (corr < -limit) corr = -limit;
                d[x] = clamp_u8(c + polarity * corr);
            }
        }
    }

    picture_Release(pic);
    return out;
}
