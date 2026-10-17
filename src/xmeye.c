/* SPDX-License-Identifier: LGPL-2.1-or-later
 * XMEye proprietary recording demuxer for VLC 3.x
 */
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_demux.h>
#include <vlc_codec.h>
#include <string.h>

#define XMEYE_PROBE_BYTES_INITIAL (256 * 1024)
#define XMEYE_PROBE_BYTES_EXTENDED (2 * 1024 * 1024)
#define XMEYE_MAX_PAYLOAD (64 * 1024 * 1024)
#define XMEYE_FALLBACK_FPS 25

struct demux_sys_t
{
    es_out_id_t *video_es;
    decoder_t *packetizer;
    vlc_fourcc_t codec;
    vlc_tick_t next_dts;
    vlc_tick_t frame_duration;
    bool have_keyframe;
    bool discontinuity_pending;
};

static int Open(vlc_object_t *);
static void Close(vlc_object_t *);
static int Demux(demux_t *);
static int Control(demux_t *, int, va_list);

vlc_module_begin()
    set_shortname("XMEye")
    set_description("XMEye DVR/NVR recording demuxer")
    set_category(CAT_INPUT)
    set_subcategory(SUBCAT_INPUT_DEMUX)
    set_capability("demux", 20)
    add_float("xmeye-fps", 0.0, "Fallback frames per second",
              "Used when XMEye timestamps and bitstream timing do not provide a usable rate.", true)
    set_callbacks(Open, Close)
    add_shortcut("xmeye")
vlc_module_end()

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static size_t annexb_prefix(const uint8_t *p, size_t n)
{
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
        return 4;
    if (n >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
        return 3;
    return 0;
}

static vlc_fourcc_t detect_payload_codec(const uint8_t *p, size_t n)
{
    /* Inspect several Annex-B NAL units. XMEye files may be named .h264 while
       carrying HEVC, so the extension is intentionally ignored. */
    unsigned h264_score = 0, hevc_score = 0;
    size_t i = 0;

    while (i + 6 < n && i < 512)
    {
        size_t sc = annexb_prefix(p + i, n - i);
        if (!sc) { i++; continue; }

        const uint8_t b = p[i + sc];
        const uint8_t h264_type = b & 0x1f;
        const uint8_t hevc_type = (b >> 1) & 0x3f;

        if (!(b & 0x80))
        {
            if (h264_type == 5 || h264_type == 7 || h264_type == 8)
                h264_score++;
            if (hevc_type >= 16 && hevc_type <= 23)
                hevc_score++;
            if (hevc_type == 32 || hevc_type == 33 || hevc_type == 34)
                hevc_score += 2;
        }
        i += sc + 1;
    }

    if (hevc_score >= 2 && hevc_score > h264_score)
        return VLC_CODEC_HEVC;
    if (h264_score >= 2)
        return VLC_CODEC_H264;
    return 0;
}

typedef struct
{
    vlc_fourcc_t codec;
    unsigned valid_records;
    unsigned video_records;
} probe_result_t;

static bool probe_xmeye(const uint8_t *p, size_t n, probe_result_t *out)
{
    memset(out, 0, sizeof(*out));

    for (size_t i = 0; i + 16 <= n; )
    {
        if (p[i] != 0 || p[i + 1] != 0 || p[i + 2] != 1)
        {
            i++;
            continue;
        }

        const uint8_t type = p[i + 3];
        size_t header = 0;
        uint32_t payload = 0;

        switch (type)
        {
            case 0xFC:
                header = 16;
                payload = le32(p + i + 12);
                break;
            case 0xFD:
                header = 8;
                payload = le32(p + i + 4);
                break;
            case 0xFA:
                header = 8;
                payload = le16(p + i + 6);
                break;
            case 0xF9:
                header = 24;
                payload = 0;
                break;
            default:
                i += 4;
                continue;
        }

        if (payload > XMEYE_MAX_PAYLOAD || i + header > n)
        {
            i += 4;
            continue;
        }

        out->valid_records++;

        if (type == 0xFC || type == 0xFD)
        {
            out->video_records++;
            if (!out->codec && payload > 0 && i + header < n)
            {
                size_t available = n - (i + header);
                if (available > payload) available = payload;
                out->codec = detect_payload_codec(p + i + header, available);
            }
        }

        /* If the complete record is in the probe window, jump to the next
           record. Otherwise stop: the structure itself is already plausible. */
        if (payload && i + header + payload <= n)
            i += header + payload;
        else if (!payload)
            i += header;
        else
            break;
    }

    return out->valid_records >= 2 && out->video_records >= 1 && out->codec != 0;
}

static int read_exact(stream_t *s, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t done = 0;
    while (done < n)
    {
        ssize_t r = vlc_stream_Read(s, p + done, n - done);
        if (r <= 0) return VLC_EGENERIC;
        done += (size_t)r;
    }
    return VLC_SUCCESS;
}

static int read_marker(stream_t *s, uint8_t marker[4])
{
    if (read_exact(s, marker, 4) != VLC_SUCCESS)
        return VLC_EGENERIC;

    while (!(marker[0] == 0 && marker[1] == 0 && marker[2] == 1 &&
             (marker[3] == 0xFC || marker[3] == 0xFD ||
              marker[3] == 0xFA || marker[3] == 0xF9)))
    {
        marker[0] = marker[1];
        marker[1] = marker[2];
        marker[2] = marker[3];
        if (read_exact(s, marker + 3, 1) != VLC_SUCCESS)
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static int send_video(demux_t *d, block_t *in)
{
    demux_sys_t *sys = d->p_sys;
    if (sys->discontinuity_pending)
    {
        in->i_flags |= BLOCK_FLAG_DISCONTINUITY;
        sys->discontinuity_pending = false;
    }
    in->i_dts = sys->next_dts;
    in->i_pts = VLC_TICK_INVALID;
    sys->next_dts += sys->frame_duration;

    block_t *out;
    while ((out = sys->packetizer->pf_packetize(sys->packetizer, &in)) != NULL)
    {
        while (out)
        {
            block_t *next = out->p_next;
            out->p_next = NULL;

            if (!sys->video_es)
            {
                sys->packetizer->fmt_out.b_packetized = true;
                sys->video_es = es_out_Add(d->out, &sys->packetizer->fmt_out);
                if (!sys->video_es)
                {
                    block_ChainRelease(out);
                    return VLC_EGENERIC;
                }
            }

            if (out->i_dts == VLC_TICK_INVALID)
                out->i_dts = sys->next_dts - sys->frame_duration;
            es_out_SetPCR(d->out, out->i_dts);
            es_out_Send(d->out, sys->video_es, out);
            out = next;
        }
    }
    return VLC_SUCCESS;
}

static int Open(vlc_object_t *obj)
{
    demux_t *d = (demux_t *)obj;
    const uint8_t *peek = NULL;
    ssize_t got = vlc_stream_Peek(d->s, &peek, XMEYE_PROBE_BYTES_INITIAL);
    if (got < 32)
        return VLC_EGENERIC;

    probe_result_t probe;
    bool detected = probe_xmeye(peek, (size_t)got, &probe);

    /* Some XMEye exports begin mid-GOP. They can contain many valid FD
       records before the first FC keyframe carrying VPS/SPS/PPS. Avoid a
       large read for normal files, but extend the probe when the container
       structure is already clearly XMEye and only codec confirmation is
       missing. */
    if (!detected && probe.valid_records >= 2 && probe.video_records >= 1 &&
        probe.codec == 0 && got >= XMEYE_PROBE_BYTES_INITIAL)
    {
        const uint8_t *extended = NULL;
        ssize_t extended_got = vlc_stream_Peek(d->s, &extended,
                                               XMEYE_PROBE_BYTES_EXTENDED);
        if (extended_got > got)
            detected = probe_xmeye(extended, (size_t)extended_got, &probe);
    }

    if (!detected)
        return VLC_EGENERIC;

    demux_sys_t *sys = calloc(1, sizeof(*sys));
    if (!sys) return VLC_ENOMEM;

    sys->codec = probe.codec;
    sys->next_dts = VLC_TICK_0;

    float fps = var_CreateGetFloat(d, "xmeye-fps");
    if (fps < 0.001f) fps = XMEYE_FALLBACK_FPS;
    sys->frame_duration = (vlc_tick_t)((double)CLOCK_FREQ / fps);

    es_format_t fmt;
    es_format_Init(&fmt, VIDEO_ES, sys->codec);
    fmt.b_packetized = false;

    sys->packetizer = demux_PacketizerNew(d, &fmt, "XMEye video");
    if (!sys->packetizer)
    {
        free(sys);
        return VLC_EGENERIC;
    }

    d->p_sys = sys;
    d->pf_demux = Demux;
    d->pf_control = Control;

    msg_Info(d, "XMEye recording detected, video codec=%4.4s, records=%u",
             (char *)&sys->codec, probe.valid_records);
    return VLC_SUCCESS;
}

static void Close(vlc_object_t *obj)
{
    demux_t *d = (demux_t *)obj;
    demux_sys_t *sys = d->p_sys;
    if (!sys) return;
    if (sys->packetizer) demux_PacketizerDestroy(sys->packetizer);
    free(sys);
}

static int Demux(demux_t *d)
{
    demux_sys_t *sys = d->p_sys;
    uint8_t marker[4];
    if (read_marker(d->s, marker) != VLC_SUCCESS)
        return VLC_DEMUXER_EOF;

    if (marker[3] == 0xF9)
    {
        uint8_t rest[20];
        if (read_exact(d->s, rest, sizeof(rest)) != VLC_SUCCESS)
            return VLC_DEMUXER_EOF;
        /* Timestamp at record +8 is available as le32(rest+4).
           Native timestamp-driven timing will replace the fallback clock. */
        return VLC_DEMUXER_SUCCESS;
    }

    if (marker[3] == 0xFA)
    {
        uint8_t rest[4];
        if (read_exact(d->s, rest, sizeof(rest)) != VLC_SUCCESS)
            return VLC_DEMUXER_EOF;
        uint16_t size = le16(rest + 2);
        if (size > 0 && vlc_stream_Read(d->s, NULL, size) != size)
            return VLC_DEMUXER_EOF;
        /* G.711 A-law output will be added as a second ES. */
        return VLC_DEMUXER_SUCCESS;
    }

    uint32_t size;
    if (marker[3] == 0xFC)
    {
        uint8_t rest[12];
        if (read_exact(d->s, rest, sizeof(rest)) != VLC_SUCCESS)
            return VLC_DEMUXER_EOF;
        size = le32(rest + 8);
    }
    else /* FD */
    {
        uint8_t rest[4];
        if (read_exact(d->s, rest, sizeof(rest)) != VLC_SUCCESS)
            return VLC_DEMUXER_EOF;
        size = le32(rest);
    }

    if (size == 0 || size > XMEYE_MAX_PAYLOAD)
        return VLC_DEMUXER_EOF;

    /* XMEye exports can begin mid-GOP with FD inter frames. Do not feed
       those to VLC before the first FC keyframe carrying VPS/SPS/PPS. */
    if (marker[3] == 0xFC)
        sys->have_keyframe = true;
    else if (!sys->have_keyframe)
    {
        if (vlc_stream_Read(d->s, NULL, size) != (ssize_t)size)
            return VLC_DEMUXER_EOF;
        return VLC_DEMUXER_SUCCESS;
    }

    block_t *block = block_Alloc(size);
    if (!block) return VLC_ENOMEM;
    if (read_exact(d->s, block->p_buffer, size) != VLC_SUCCESS)
    {
        block_Release(block);
        return VLC_DEMUXER_EOF;
    }

    if (send_video(d, block) != VLC_SUCCESS)
        return VLC_DEMUXER_EOF;
    return VLC_DEMUXER_SUCCESS;
}

static void ResetAfterSeek(demux_t *d)
{
    demux_sys_t *sys = d->p_sys;
    sys->have_keyframe = false;
    sys->discontinuity_pending = true;
    if (sys->packetizer && sys->packetizer->pf_flush)
        sys->packetizer->pf_flush(sys->packetizer);
}

static int Control(demux_t *d, int query, va_list args)
{
    if (query == DEMUX_SET_POSITION)
    {
        double position = va_arg(args, double);
        uint64_t size = 0;
        if (vlc_stream_GetSize(d->s, &size) != VLC_SUCCESS || size == 0)
            return VLC_EGENERIC;

        if (position < 0.0) position = 0.0;
        if (position > 1.0) position = 1.0;
        uint64_t offset = (uint64_t)(position * (double)size);

        if (vlc_stream_Seek(d->s, offset) != VLC_SUCCESS)
            return VLC_EGENERIC;

        ResetAfterSeek(d);
        return VLC_SUCCESS;
    }

    if (query == DEMUX_SET_TIME)
        return VLC_EGENERIC;

    return demux_vaControlHelper(d->s, 0, -1, 0, 1, query, args);
}
