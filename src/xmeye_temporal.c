/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye temporal 2-to-1 adaptive accumulation filter for VLC 3.x
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

#define FILTER_PREFIX "xmeye2x-"

#define STRENGTH_TEXT N_("Temporal accumulation strength")
#define STRENGTH_LONGTEXT N_("Recursive temporal strength. 2 is mild; 4 is the recommended default; higher values denoise static areas more strongly.")
#define THRESH_Y_TEXT N_("Luma motion threshold")
#define THRESH_Y_LONGTEXT N_("Maximum luma difference that is still considered temporally stable.")
#define THRESH_C_TEXT N_("Chroma motion threshold")
#define THRESH_C_LONGTEXT N_("Maximum chroma difference that is still considered temporally stable.")

struct filter_sys_t
{
    picture_t *first;
    picture_t *state;
    bool have_first;
    bool have_state;
    int strength;
    int threshold_y;
    int threshold_c;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);

static const char *const filter_options[] = {
    "strength", "threshold-y", "threshold-c", NULL
};

vlc_module_begin()
    set_shortname("XMEye Temporal 2x")
    set_description("Motion-adaptive temporal accumulation, two input frames per output frame")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    add_integer_with_range(FILTER_PREFIX "strength", 4, 2, 8,
                           STRENGTH_TEXT, STRENGTH_LONGTEXT, false)
    add_integer_with_range(FILTER_PREFIX "threshold-y", 12, 0, 64,
                           THRESH_Y_TEXT, THRESH_Y_LONGTEXT, false)
    add_integer_with_range(FILTER_PREFIX "threshold-c", 8, 0, 64,
                           THRESH_C_TEXT, THRESH_C_LONGTEXT, false)
    add_shortcut("xmeye2x")
    set_callbacks(Create, Destroy)
vlc_module_end()

static int Create(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys)
        return VLC_ENOMEM;

    sys->first = picture_NewFromFormat(&filter->fmt_in.video);
    sys->state = picture_NewFromFormat(&filter->fmt_in.video);
    if (!sys->first || !sys->state || sys->first->i_planes <= 0 || sys->state->i_planes <= 0)
    {
        if (sys->first) picture_Release(sys->first);
        if (sys->state) picture_Release(sys->state);
        free(sys);
        msg_Err(filter, "XMEye Temporal 2x requires CPU-accessible video planes");
        return VLC_EGENERIC;
    }

    config_ChainParse(filter, FILTER_PREFIX, filter_options, filter->p_cfg);
    sys->strength = VLC_CLIP(var_CreateGetInteger(filter, FILTER_PREFIX "strength"), 2, 8);
    sys->threshold_y = VLC_CLIP(var_CreateGetInteger(filter, FILTER_PREFIX "threshold-y"), 0, 64);
    sys->threshold_c = VLC_CLIP(var_CreateGetInteger(filter, FILTER_PREFIX "threshold-c"), 0, 64);

    filter->p_sys = sys;
    filter->pf_video_filter = Filter;
    filter->pf_flush = Flush;
    msg_Info(filter, "XMEye Temporal 2x enabled: strength=%d threshold-y=%d threshold-c=%d",
             sys->strength, sys->threshold_y, sys->threshold_c);
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys)
        return;
    if (sys->first) picture_Release(sys->first);
    if (sys->state) picture_Release(sys->state);
    free(sys);
}

static void Flush(filter_t *filter)
{
    filter_sys_t *sys = filter->p_sys;
    sys->have_first = false;
    sys->have_state = false;
}

static picture_t *Filter(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys = filter->p_sys;
    if (!pic)
        return NULL;

    if (!sys->have_first)
    {
        picture_CopyPixels(sys->first, pic);
        sys->have_first = true;
        picture_Release(pic);
        return NULL;
    }

    picture_t *out = filter_NewPicture(filter);
    if (!out)
    {
        sys->have_first = false;
        picture_Release(pic);
        return NULL;
    }

    const int planes = __MIN(out->i_planes,
                       __MIN(sys->first->i_planes,
                       __MIN(sys->state->i_planes, pic->i_planes)));

    for (int p = 0; p < planes; ++p)
    {
        const plane_t *a = &sys->first->p[p];
        const plane_t *b = &pic->p[p];
        const plane_t *old = &sys->state->p[p];
        plane_t *dst = &out->p[p];
        const int lines = __MIN(dst->i_visible_lines,
                          __MIN(a->i_visible_lines,
                          __MIN(b->i_visible_lines, old->i_visible_lines)));
        const int bytes = __MIN(dst->i_visible_pitch,
                          __MIN(a->i_visible_pitch,
                          __MIN(b->i_visible_pitch, old->i_visible_pitch)));
        const int threshold = (p == Y_PLANE) ? sys->threshold_y : sys->threshold_c;

        for (int y = 0; y < lines; ++y)
        {
            const uint8_t *pa = a->p_pixels + y * a->i_pitch;
            const uint8_t *pb = b->p_pixels + y * b->i_pitch;
            const uint8_t *po = old->p_pixels + y * old->i_pitch;
            uint8_t *pd = dst->p_pixels + y * dst->i_pitch;

            for (int x = 0; x < bytes; ++x)
            {
                const unsigned pair = ((unsigned)pa[x] + (unsigned)pb[x] + 1u) >> 1;

                if (!sys->have_state)
                {
                    pd[x] = (uint8_t)pair;
                    continue;
                }

                int diff = (int)pair - (int)po[x];
                if (diff < 0) diff = -diff;

                if (diff <= threshold)
                {
                    const unsigned weighted =
                        (unsigned)(sys->strength - 1) * (unsigned)po[x] + pair;
                    pd[x] = (uint8_t)((weighted + (unsigned)(sys->strength / 2)) /
                                      (unsigned)sys->strength);
                }
                else
                {
                    pd[x] = (uint8_t)pair;
                }
            }
        }
    }

    picture_CopyProperties(out, pic);
    picture_CopyPixels(sys->state, out);
    sys->have_state = true;
    sys->have_first = false;
    picture_Release(pic);
    return out;
}
