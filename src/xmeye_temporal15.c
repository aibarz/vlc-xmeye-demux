/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye temporal 20-to-15 fps filter for VLC 3.x
 * Original concept and functional design: Álvaro Ibarz
 * Copyright (C) 2026 Álvaro Ibarz
 *
 * 4 input frames -> 3 output frames, preserving a uniform 15 fps cadence:
 *   O0 = F0
 *   O1 = 2/3 F1 + 1/3 F2
 *   O2 = 1/3 F2 + 2/3 F3
 * Then repeat.
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

#define FILTER_PREFIX "xmeye15-"

struct filter_sys_t
{
    picture_t *prev;
    int phase;
    atomic_int enabled;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);
static int OptionCallback(vlc_object_t *, const char *, vlc_value_t, vlc_value_t, void *);

static const char *const filter_options[] = { "enabled", NULL };

vlc_module_begin()
    set_shortname("XMEye Temporal 15fps")
    set_description("Temporal 4-to-3 resampler: 20 fps input to 15 fps output")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_bool(FILTER_PREFIX "enabled", true,
             "Temporal 20-to-15 enabled",
             "Enable or bypass the 4-to-3 temporal resampler.", false)
    add_shortcut("xmeye15")
    set_callbacks(Create, Destroy)
vlc_module_end()

static int Create(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    sys->prev = picture_NewFromFormat(&filter->fmt_in.video);
    if (!sys->prev || sys->prev->i_planes <= 0)
    {
        if (sys->prev) picture_Release(sys->prev);
        free(sys);
        msg_Err(filter, "XMEye Temporal 15fps requires CPU-accessible video planes");
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    config_ChainParse(filter, FILTER_PREFIX, filter_options, filter->p_cfg);
    atomic_init(&sys->enabled,
                var_CreateGetBoolCommand(filter, FILTER_PREFIX "enabled") ? 1 : 0);
    var_AddCallback(filter, FILTER_PREFIX "enabled", OptionCallback, sys);

    filter->pf_video_filter = Filter;
    filter->pf_flush = Flush;
    msg_Info(filter, "XMEye Temporal 15fps ready: 4 input -> 3 output");
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys) return;
    var_DelCallback(filter, FILTER_PREFIX "enabled", OptionCallback, sys);
    if (sys->prev) picture_Release(sys->prev);
    free(sys);
}

static void Flush(filter_t *filter)
{
    filter_sys_t *sys = filter->p_sys;
    sys->phase = 0;
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
        sys->phase = 0;
    }
    return VLC_SUCCESS;
}

static picture_t *blend_pair(filter_t *filter, const picture_t *a,
                             const picture_t *b, unsigned wa, unsigned wb,
                             int fraction_num)
{
    picture_t *out = filter_NewPicture(filter);
    if (!out)
        return NULL;

    const int planes = __MIN(out->i_planes, __MIN(a->i_planes, b->i_planes));
    for (int p = 0; p < planes; ++p)
    {
        const plane_t *pa = &a->p[p];
        const plane_t *pb = &b->p[p];
        plane_t *pd = &out->p[p];
        const int lines = __MIN(pd->i_visible_lines,
                          __MIN(pa->i_visible_lines, pb->i_visible_lines));
        const int bytes = __MIN(pd->i_visible_pitch,
                          __MIN(pa->i_visible_pitch, pb->i_visible_pitch));

        for (int y = 0; y < lines; ++y)
        {
            const uint8_t *sa = pa->p_pixels + y * pa->i_pitch;
            const uint8_t *sb = pb->p_pixels + y * pb->i_pitch;
            uint8_t *dst = pd->p_pixels + y * pd->i_pitch;
            for (int x = 0; x < bytes; ++x)
                dst[x] = (uint8_t)((wa * (unsigned)sa[x] + wb * (unsigned)sb[x] + 1u) / 3u);
        }
    }

    picture_CopyProperties(out, b);
    if (a->date != VLC_TICK_INVALID && b->date != VLC_TICK_INVALID)
    {
        const vlc_tick_t delta = b->date - a->date;
        out->date = a->date + (delta * fraction_num) / 3;
    }
    return out;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic)
        return NULL;

    if (!atomic_load(&sys->enabled))
    {
        sys->phase = 0;
        return pic;
    }

    switch (sys->phase)
    {
        case 0:
            sys->phase = 1;
            return pic;

        case 1:
            picture_CopyPixels(sys->prev, pic);
            picture_CopyProperties(sys->prev, pic);
            sys->phase = 2;
            picture_Release(pic);
            return NULL;

        case 2:
        {
            picture_t *out = blend_pair(filter, sys->prev, pic, 2, 1, 1);
            picture_CopyPixels(sys->prev, pic);
            picture_CopyProperties(sys->prev, pic);
            sys->phase = 3;
            picture_Release(pic);
            return out;
        }

        default: /* phase 3 */
        {
            picture_t *out = blend_pair(filter, sys->prev, pic, 1, 2, 2);
            sys->phase = 0;
            picture_Release(pic);
            return out;
        }
    }
}
