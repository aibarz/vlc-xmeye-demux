from pathlib import Path

p = Path("modules/demux/avi/avi.c")
s = p.read_text(encoding="utf-8")

track_before = "    bool            is_qnap;\n"
track_after = track_before + "    bool            is_xmeye_hevc;\n"
if s.count(track_before) != 1:
    raise SystemExit("AVI track structure changed")
s = s.replace(track_before, track_after, 1)

marker = """static void AVI_SendFrame( demux_t *p_demux, avi_track_t *tk, block_t *p_frame )
{
    demux_sys_t *p_sys = p_demux->p_sys;
"""
new_functions = """/* HEVC NAL type alone is not a codec signature: an ordinary XMEye H.264
 * SPS (0x27) can masquerade as HEVC IRAP type 19. Check both bytes of the
 * HEVC header and, before switching codecs, require a VPS/SPS/PPS triplet.
 * Never infer HEVC merely from 00 00 00 FD + Annex-B. */
static bool AVI_XMEyeHEVCHeader( const block_t *frame )
{
    if( frame->i_buffer < 14 )
        return false;

    const uint8_t *p = frame->p_buffer;
    if( p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0xFD )
        return false;
    if( p[8] != 0 || p[9] != 0 || p[10] != 0 || p[11] != 1 )
        return false;

    /* HEVC forbidden_zero_bit=0, layer_id=0 and temporal_id_plus1!=0. */
    if( ( p[12] & 0x81 ) != 0 || ( p[13] & 0xF8 ) != 0 ||
        ( p[13] & 0x07 ) == 0 )
        return false;

    const uint8_t type = ( p[12] >> 1 ) & 0x3F;
    return type == 32 || type == 33 || type == 34 ||
           ( type >= 16 && type <= 23 );
}

static bool AVI_XMEyeHEVCParameterSets( const block_t *frame )
{
    if( !AVI_XMEyeHEVCHeader( frame ) )
        return false;

    const uint8_t *p = frame->p_buffer;
    /* The first wrapped HEVC keyframe is expected to start with a VPS. */
    if( ( ( p[12] >> 1 ) & 0x3F ) != 32 )
        return false;

    const size_t limit = frame->i_buffer < 4096 ? frame->i_buffer : 4096;
    unsigned present = 0;
    for( size_t i = 8; i + 5 < limit; ++i )
    {
        if( p[i] || p[i+1] || p[i+2] || p[i+3] != 1 )
            continue;

        const uint8_t b0 = p[i+4], b1 = p[i+5];
        if( ( b0 & 0x81 ) != 0 || ( b1 & 0xF8 ) != 0 ||
            ( b1 & 0x07 ) == 0 )
            continue;

        const uint8_t type = ( b0 >> 1 ) & 0x3F;
        if( type == 32 ) present |= 1;
        else if( type == 33 ) present |= 2;
        else if( type == 34 ) present |= 4;
        if( present == 7 )
            return true;
    }
    return false;
}

"""
if s.count(marker) != 1:
    raise SystemExit("AVI_SendFrame signature changed")
s = s.replace(marker, new_functions + marker, 1)

old_fragment = """    if( tk->i_dv_audio_rate )
        AVI_DvHandleAudio( p_demux, tk, p_frame );

    /* Strip 3rd party header */
"""
new_fragment = """    if( tk->i_dv_audio_rate )
        AVI_DvHandleAudio( p_demux, tk, p_frame );

    /* Switch H.264-tagged XMEye AVI to HEVC only on a complete and
     * structurally valid HEVC VPS/SPS/PPS keyframe, never on NAL type alone.
     * Ordinary H.264 files using the same private FD wrapper stay H.264. */
    if( tk->fmt.i_cat == VIDEO_ES && !tk->is_xmeye_hevc &&
        tk->fmt.i_codec == VLC_CODEC_H264 &&
        AVI_XMEyeHEVCParameterSets( p_frame ) )
    {
        if( tk->p_es )
            es_out_Del( p_demux->out, tk->p_es );
        tk->p_es = NULL;
        tk->fmt.i_codec = VLC_CODEC_HEVC;
        tk->fmt.i_original_fourcc = 0;
        tk->fmt.b_packetized = false;
        free( tk->fmt.p_extra );
        tk->fmt.p_extra = NULL;
        tk->fmt.i_extra = 0;
        tk->p_es = es_out_Add( p_demux->out, &tk->fmt );
        tk->is_xmeye_hevc = true;
        msg_Info( p_demux, "XMEye AVI HEVC VPS/SPS/PPS verified" );
    }

    if( tk->is_xmeye_hevc && AVI_XMEyeHEVCHeader( p_frame ) )
    {
        p_frame->p_buffer += 8;
        p_frame->i_buffer -= 8;
    }

    /* Strip 3rd party header */
"""
if s.count(old_fragment) != 1:
    raise SystemExit("AVI payload insert point changed")
s = s.replace(old_fragment, new_fragment, 1)
p.write_text(s, encoding="utf-8")
print("Patched", p)
