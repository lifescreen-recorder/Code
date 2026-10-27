#ifndef LIFESCREEN_FFMPEG_BRIDGE_H
#define LIFESCREEN_FFMPEG_BRIDGE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Available encoder flags
#define ENCODER_CAP_NVENC_H264   (1 << 0)
#define ENCODER_CAP_NVENC_HEVC   (1 << 1)
#define ENCODER_CAP_NVENC_AV1    (1 << 2)
#define ENCODER_CAP_AMF_H264     (1 << 3)
#define ENCODER_CAP_AMF_HEVC     (1 << 4)
#define ENCODER_CAP_AMF_AV1      (1 << 5)
#define ENCODER_CAP_QSV_H264     (1 << 6)
#define ENCODER_CAP_QSV_HEVC     (1 << 7)
#define ENCODER_CAP_QSV_AV1      (1 << 8)
#define ENCODER_CAP_SW_X264      (1 << 9)
#define ENCODER_CAP_SW_X265      (1 << 10)

typedef enum {
    CODEC_H264 = 0,
    CODEC_HEVC = 1,
    CODEC_AV1  = 2,
} BridgeCodecType;

typedef enum {
    BACKEND_AUTO     = 0,
    BACKEND_NVENC    = 1,
    BACKEND_AMF      = 2,
    BACKEND_QSV      = 3,
    BACKEND_SOFTWARE = 4,
} BridgeBackendType;

typedef enum {
    RC_CQP = 0,
    RC_CRF = 1,
    RC_VBR = 2,
    RC_CBR = 3,
} BridgeRateControl;

typedef struct {
    BridgeCodecType codec;
    BridgeBackendType backend;
    BridgeRateControl rate_control;
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t quality_cqp;       // 0-51
    uint32_t bitrate_kbps;      // e.g. 15000
    uint32_t keyframe_interval_secs;
    const char *preset;         // "p4", "p7", "medium", etc.
    void *d3d11_device;         // ID3D11Device* for hardware D3D11VA
    void *d3d11_context;        // ID3D11DeviceContext* for staging/fallback
    uint32_t src_width;         // Source capture width (0 = same as width)
    uint32_t src_height;        // Source capture height (0 = same as height)
    uint32_t color_range;       // 0 = Limited (MPEG), 1 = Full (JPEG)
    bool capture_cursor;
} VideoEncoderConfig;

typedef struct {
    uint8_t *data;
    size_t size;
    int64_t pts;
    int64_t dts;
    int is_keyframe;
} EncodedPacket;

typedef void (*PacketCallback)(void *user_data, const EncodedPacket *packet);

// Opaque context types
typedef struct VideoEncoderContext VideoEncoderContext;
typedef struct MuxerContext MuxerContext;

// 1. Hardware probe
uint32_t ffmpeg_bridge_probe_encoders(void);

// 2. Video Encoder
VideoEncoderContext* ffmpeg_encoder_create(const VideoEncoderConfig *config, PacketCallback callback, void *user_data);
int ffmpeg_encoder_send_d3d11_texture(VideoEncoderContext *ctx, void *d3d11_texture, int64_t pts_nanos);
int ffmpeg_encoder_repeat_last_frame(VideoEncoderContext *ctx, int64_t pts_nanos);
int ffmpeg_encoder_send_cpu_frame(VideoEncoderContext *ctx, const uint8_t *bgra_data, uint32_t linesize, int64_t pts_nanos);
int ffmpeg_encoder_flush(VideoEncoderContext *ctx);
void ffmpeg_encoder_destroy(VideoEncoderContext *ctx);

// 3. Container Muxer (MKV, MP4, MOV)
typedef struct {
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t bitrate_kbps;
    uint32_t enable_multi_track;
} MuxerAudioStreamConfig;

MuxerContext* ffmpeg_muxer_create(
    const char *output_file,
    const VideoEncoderContext *encoder_ctx,
    const MuxerAudioStreamConfig *audio_cfg
);

int ffmpeg_muxer_write_video_packet(MuxerContext *muxer, const EncodedPacket *pkt);
int ffmpeg_muxer_write_audio_samples(MuxerContext *muxer, const float *pcm_samples, uint32_t sample_count, int64_t pts_nanos);
int ffmpeg_muxer_write_audio_samples_track(MuxerContext *muxer, int track_index, const float *pcm_samples, uint32_t sample_count, int64_t pts_nanos);
int ffmpeg_muxer_close(MuxerContext *muxer);

#ifdef __cplusplus
}
#endif

#endif // LIFESCREEN_FFMPEG_BRIDGE_H
