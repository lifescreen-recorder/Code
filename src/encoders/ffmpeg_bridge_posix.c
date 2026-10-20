#include "ffmpeg_bridge.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<libavcodec/avcodec.h>)
#include <libavcodec/avcodec.h>
#define HAS_LIBAV 1
#endif
#endif

uint32_t ffmpeg_bridge_probe_encoders(void) {
    uint32_t caps = 0;
#ifdef HAS_LIBAV
    if (avcodec_find_encoder_by_name("h264_nvenc")) caps |= ENCODER_CAP_NVENC_H264;
    if (avcodec_find_encoder_by_name("hevc_nvenc")) caps |= ENCODER_CAP_NVENC_HEVC;
    if (avcodec_find_encoder_by_name("av1_nvenc"))  caps |= ENCODER_CAP_NVENC_AV1;
    if (avcodec_find_encoder_by_name("h264_amf"))   caps |= ENCODER_CAP_AMF_H264;
    if (avcodec_find_encoder_by_name("hevc_amf"))   caps |= ENCODER_CAP_AMF_HEVC;
    if (avcodec_find_encoder_by_name("av1_amf"))    caps |= ENCODER_CAP_AMF_AV1;
    if (avcodec_find_encoder_by_name("h264_qsv"))   caps |= ENCODER_CAP_QSV_H264;
    if (avcodec_find_encoder_by_name("hevc_qsv"))   caps |= ENCODER_CAP_QSV_HEVC;
    if (avcodec_find_encoder_by_name("av1_qsv"))    caps |= ENCODER_CAP_QSV_AV1;
    if (avcodec_find_encoder_by_name("libx264"))    caps |= ENCODER_CAP_SW_X264;
    if (avcodec_find_encoder_by_name("libx265"))    caps |= ENCODER_CAP_SW_X265;
#else
    // Default software capabilities for POSIX preview
    caps |= ENCODER_CAP_SW_X264;
    caps |= ENCODER_CAP_SW_X265;
#endif
    return caps;
}

VideoEncoderContext* ffmpeg_encoder_create(const VideoEncoderConfig *config, PacketCallback callback, void *user_data) {
    (void)config; (void)callback; (void)user_data;
    return NULL;
}

int ffmpeg_encoder_send_d3d11_texture(VideoEncoderContext *ctx, void *d3d11_texture, int64_t pts_nanos) {
    (void)ctx; (void)d3d11_texture; (void)pts_nanos;
    return -1;
}

int ffmpeg_encoder_send_cpu_frame(VideoEncoderContext *ctx, const uint8_t *bgra_data, uint32_t linesize, int64_t pts_nanos) {
    (void)ctx; (void)bgra_data; (void)linesize; (void)pts_nanos;
    return -1;
}

int ffmpeg_encoder_flush(VideoEncoderContext *ctx) {
    (void)ctx;
    return 0;
}

void ffmpeg_encoder_destroy(VideoEncoderContext *ctx) {
    (void)ctx;
}

MuxerContext* ffmpeg_muxer_create(const char *output_file, const VideoEncoderContext *encoder_ctx, const MuxerAudioStreamConfig *audio_cfg) {
    (void)output_file; (void)encoder_ctx; (void)audio_cfg;
    return NULL;
}

int ffmpeg_muxer_write_video_packet(MuxerContext *muxer, const EncodedPacket *pkt) {
    (void)muxer; (void)pkt;
    return -1;
}

int ffmpeg_muxer_write_audio_samples(MuxerContext *muxer, const float *pcm_samples, uint32_t sample_count, int64_t pts_nanos) {
    (void)muxer; (void)pcm_samples; (void)sample_count; (void)pts_nanos;
    return -1;
}

int ffmpeg_muxer_write_audio_samples_track(MuxerContext *muxer, int track_index, const float *pcm_samples, uint32_t sample_count, int64_t pts_nanos) {
    (void)muxer; (void)track_index; (void)pcm_samples; (void)sample_count; (void)pts_nanos;
    return -1;
}

int ffmpeg_muxer_close(MuxerContext *muxer) {
    (void)muxer;
    return 0;
}
