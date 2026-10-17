from pathlib import Path
p=Path('modules/demux/avi/avi.c')
s=p.read_text(encoding='utf-8')
s=s.replace('    bool            is_qnap;\n', '    bool            is_qnap;\n    bool            is_xmeye_hevc;\n', 1)
needle='''static void AVI_SendFrame( demux_t *p_demux, avi_track_t *tk, block_t *p_frame )\n{\n    demux_sys_t *p_sys = p_demux->p_sys;\n'''
insert='''static bool AVI_XMEyeHEVCHeader( const block_t *p_frame )\n{\n    if( p_frame->i_buffer < 13 )\n        return false;\n    const uint8_t *p = p_frame->p_buffer;\n    if( p[0] != 0 || p[1] != 0 || p[2] != 0 || p[3] != 0xFD )\n        return false;\n    if( p[8] != 0 || p[9] != 0 || p[10] != 0 || p[11] != 1 )\n        return false;\n    const uint8_t nal_type = (p[12] >> 1) & 0x3f;\n    return nal_type == 32 || nal_type == 33 || nal_type == 34 ||\n           (nal_type >= 16 && nal_type <= 23);\n}\n\nstatic void AVI_SendFrame( demux_t *p_demux, avi_track_t *tk, block_t *p_frame )\n{\n    demux_sys_t *p_sys = p_demux->p_sys;\n'''
if needle not in s: raise SystemExit('sendframe needle missing')
s=s.replace(needle,insert,1)
needle2='''    if( tk->i_dv_audio_rate )\n        AVI_DvHandleAudio( p_demux, tk, p_frame );\n\n    /* Strip 3rd party header */\n'''
insert2='''    if( tk->i_dv_audio_rate )\n        AVI_DvHandleAudio( p_demux, tk, p_frame );\n\n    /* XMEye AVI exports may advertise H264 while carrying HEVC Annex-B.\n       Their periodic keyframe packets start with an 8-byte 00 00 00 FD\n       private header. Detect it on the first frame, switch the ES to HEVC,\n       then strip that private header whenever it appears. */\n    if( tk->fmt.i_cat == VIDEO_ES && !tk->is_xmeye_hevc &&\n        tk->fmt.i_codec == VLC_CODEC_H264 && AVI_XMEyeHEVCHeader( p_frame ) )\n    {\n        if( tk->p_es )\n            es_out_Del( p_demux->out, tk->p_es );\n        tk->p_es = NULL;\n        tk->fmt.i_codec = VLC_CODEC_HEVC;\n        tk->fmt.i_original_fourcc = 0;\n        tk->fmt.b_packetized = false;\n        free( tk->fmt.p_extra );\n        tk->fmt.p_extra = NULL;\n        tk->fmt.i_extra = 0;\n        tk->p_es = es_out_Add( p_demux->out, &tk->fmt );\n        tk->is_xmeye_hevc = true;\n        msg_Info( p_demux, "XMEye AVI wrapper detected: H264 tag contains HEVC payload" );\n    }\n\n    if( tk->is_xmeye_hevc && AVI_XMEyeHEVCHeader( p_frame ) )\n    {\n        p_frame->p_buffer += 8;\n        p_frame->i_buffer -= 8;\n    }\n\n    /* Strip 3rd party header */\n'''
if needle2 not in s: raise SystemExit('payload needle missing')
s=s.replace(needle2,insert2,1)
p.write_text(s,encoding='utf-8')
print('patched',p)
