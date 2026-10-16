/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye temporal 2-to-1 averaging filter for VLC 3.x
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

struct filter_sys_t
{
    picture_t *first;
    bool have_first;
};

static int Create(vlc_object_t *);
static void Destroy(vlc_object_t *);
static picture_t *Filter(filter_t *, picture_t *);
static void Flush(filter_t *);

vlc_module_begin()
    set_shortname("XMEye Temporal 2x")
    set_description("Average consecutive frame pairs and output one frame per pair")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
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
    if (!sys->first || sys->first->i_planes <= 0)
    {
        if (sys->first)
            picture_Release(sys->first);
        free(sys);
        msg_Err(filter, "XMEye Temporal 2x requires CPU-accessible video planes");
        return VLC_EGENERIC;
    }

    filter->p_sys = sys;
    filter->pf_video_filter = Filter;
    filter->pf_flush = Flush;
    msg_Info(filter, "XMEye Temporal 2x enabled");
    return VLC_SUCCESS;
}

static void Destroy(vlc_object_t *obj)
{
    filter_t *filter = (filter_t *)obj;
    filter_sys_t *sys = filter->p_sys;
    if (!sys)
        return;
    if (sys->first)
        picture_Release(sys->first);
    free(sys);
}

static void Flush(filter_t *filter)
{
    filter_sys_t *sys = filter->p_sys;
    sys->have_first = false;
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

    const int planes = __MIN(out->i_planes, __MIN(sys->first->i_planes, pic->i_planes));
    for (int p = 0; p < planes; ++p)
    {
        const plane_t *a = &sys->first->p[p];
        const plane_t *b = &pic->p[p];
        plane_t *dst = &out->p[p];
        const int lines = __MIN(dst->i_visible_lines, __MIN(a->i_visible_lines, b->i_visible_lines));
        const int bytes = __MIN(dst->i_visible_pitch, __MIN(a->i_visible_pitch, b->i_visible_pitch));

        for (int y = 0; y < lines; ++y)
        {
            const uint8_t *pa = a->p_pixels + y * a->i_pitch;
            const uint8_t *pb = b->p_pixels + y * b->i_pitch;
            uint8_t *pd = dst->p_pixels + y * dst->i_pitch;
            for (int x = 0; x < bytes; ++x)
                pd[x] = (uint8_t)(((unsigned)pa[x] + (unsigned)pb[x] + 1u) >> 1);
        }
    }

    picture_CopyProperties(out, pic);
    picture_Release(pic);
    sys->have_first = false;
    return out;
}
