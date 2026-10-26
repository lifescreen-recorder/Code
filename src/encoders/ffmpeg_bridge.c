#include "ffmpeg_bridge.h"

#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/audio_fifo.h>

#define STAGING_BUFFERS 4


struct VideoEncoderContext {
    AVCodecContext *codec_ctx;
    AVBufferRef *hw_device_ref;
    AVBufferRef *hw_frames_ref;
    AVFrame *sw_frame;
    AVFrame *hw_frame;
    AVPacket *packet;
    struct SwsContext *sws_ctx;
    PacketCallback callback;
    void *user_data;
    uint32_t width;
    uint32_t height;
    uint32_t src_width;
    uint32_t src_height;
    uint32_t fps;
    int is_hardware;
    ID3D11DeviceContext *d3d11_context;

    // Hardware D3D11 Video Processor (Zero-Copy GPU 4K BGRA -> NV12 color conversion)
    ID3D11VideoDevice *video_device;
    ID3D11VideoContext *video_context;
    ID3D11VideoProcessorEnumerator *video_enumerator;
    ID3D11VideoProcessor *video_processor;
    ID3D11Texture2D *gpu_nv12_texture;
    ID3D11VideoProcessorOutputView *video_output_view;
    ID3D11VideoProcessorInputView *video_input_view;
    ID3D11Resource *last_input_resource;
    bool use_gpu_nv12_staging;

    ID3D11Texture2D *staging_textures[STAGING_BUFFERS];
    int64_t staging_pts[STAGING_BUFFERS];
    int staging_index;
    int staging_count;
    int64_t last_pts;
    bool capture_cursor;
};

struct MuxerContext {
    AVFormatContext *fmt_ctx;
    AVStream *video_stream;
    AVPacket *video_packet;
    SRWLOCK lock;

    AVStream *audio_stream;     // Track 1: System Audio
    AVCodecContext *audio_codec_ctx;
    AVFrame *audio_frame;
    AVPacket *audio_packet;
    AVAudioFifo *audio_fifo;
    int64_t next_audio_pts;

    AVStream *audio_stream_mic; // Track 2: Microphone Audio
    AVCodecContext *audio_codec_ctx_mic;
    AVFrame *audio_frame_mic;
    AVPacket *audio_packet_mic;
    AVAudioFifo *audio_fifo_mic;
    int64_t next_audio_pts_mic;

    SwrContext *swr_ctx;
    uint32_t sample_rate;
    uint32_t channels;
    AVRational encoder_time_base;
    uint32_t fps;
};

static int test_hardware_encoder(const char* name) {
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) return 0;

    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) return 0;

    ctx->width = 128;
    ctx->height = 128;
    ctx->time_base = (AVRational){1, 30};
    ctx->framerate = (AVRational){30, 1};
    ctx->pix_fmt = AV_PIX_FMT_NV12;

    AVBufferRef *hw_dev = NULL;
    if (strstr(name, "qsv")) {
        av_hwdevice_ctx_create(&hw_dev, AV_HWDEVICE_TYPE_QSV, NULL, NULL, 0);
        if (hw_dev) ctx->hw_device_ctx = av_buffer_ref(hw_dev);
    }

    int old_level = av_log_get_level();
    av_log_set_level(AV_LOG_QUIET);
    int ret = avcodec_open2(ctx, codec, NULL);
    av_log_set_level(old_level);

    avcodec_free_context(&ctx);
    if (hw_dev) av_buffer_unref(&hw_dev);
    return (ret >= 0);
}

uint32_t ffmpeg_bridge_probe_encoders(void) {
    uint32_t caps = 0;
    if (test_hardware_encoder("h264_nvenc")) caps |= ENCODER_CAP_NVENC_H264;
    if (test_hardware_encoder("hevc_nvenc")) caps |= ENCODER_CAP_NVENC_HEVC;
    if (test_hardware_encoder("av1_nvenc"))  caps |= ENCODER_CAP_NVENC_AV1;

    if (test_hardware_encoder("h264_amf"))   caps |= ENCODER_CAP_AMF_H264;
    if (test_hardware_encoder("hevc_amf"))   caps |= ENCODER_CAP_AMF_HEVC;
    if (test_hardware_encoder("av1_amf"))    caps |= ENCODER_CAP_AMF_AV1;

    if (test_hardware_encoder("h264_qsv"))   caps |= ENCODER_CAP_QSV_H264;
    if (test_hardware_encoder("hevc_qsv"))   caps |= ENCODER_CAP_QSV_HEVC;
    if (test_hardware_encoder("av1_qsv"))    caps |= ENCODER_CAP_QSV_AV1;

    if (avcodec_find_encoder_by_name("libx264"))    caps |= ENCODER_CAP_SW_X264;
    if (avcodec_find_encoder_by_name("libx265"))    caps |= ENCODER_CAP_SW_X265;

    return caps;
}

static const char* select_encoder_name(BridgeCodecType codec, BridgeBackendType backend, int *is_hw) {
    if (backend == BACKEND_SOFTWARE) {
        *is_hw = 0;
        if (codec == CODEC_HEVC && avcodec_find_encoder_by_name("libx265")) return "libx265";
        return "libx264";
    }

    if (backend == BACKEND_NVENC) {
        *is_hw = 1;
        if (codec == CODEC_H264 && test_hardware_encoder("h264_nvenc")) return "h264_nvenc";
        if (codec == CODEC_HEVC && test_hardware_encoder("hevc_nvenc")) return "hevc_nvenc";
        if (codec == CODEC_AV1  && test_hardware_encoder("av1_nvenc"))  return "av1_nvenc";
    }

    if (backend == BACKEND_AMF) {
        *is_hw = 1;
        if (codec == CODEC_H264 && test_hardware_encoder("h264_amf")) return "h264_amf";
        if (codec == CODEC_HEVC && test_hardware_encoder("hevc_amf")) return "hevc_amf";
        if (codec == CODEC_AV1  && test_hardware_encoder("av1_amf"))  return "av1_amf";
    }

    if (backend == BACKEND_QSV) {
        *is_hw = 1;
        if (codec == CODEC_H264 && test_hardware_encoder("h264_qsv")) return "h264_qsv";
        if (codec == CODEC_HEVC && test_hardware_encoder("hevc_qsv")) return "hevc_qsv";
        if (codec == CODEC_AV1  && test_hardware_encoder("av1_qsv"))  return "av1_qsv";
    }

    // BACKEND_AUTO: Prioritize hardware encoders!
    if (backend == BACKEND_AUTO) {
        // 1. Try Intel QuickSync (omnipresent on Intel laptops & PCs)
        if (codec == CODEC_H264 && test_hardware_encoder("h264_qsv")) { *is_hw = 1; return "h264_qsv"; }
        if (codec == CODEC_HEVC && test_hardware_encoder("hevc_qsv")) { *is_hw = 1; return "hevc_qsv"; }
        if (codec == CODEC_AV1  && test_hardware_encoder("av1_qsv"))  { *is_hw = 1; return "av1_qsv"; }

        // 2. Try NVIDIA NVENC (for GTX/RTX GPUs)
        if (codec == CODEC_H264 && test_hardware_encoder("h264_nvenc")) { *is_hw = 1; return "h264_nvenc"; }
        if (codec == CODEC_HEVC && test_hardware_encoder("hevc_nvenc")) { *is_hw = 1; return "hevc_nvenc"; }
        if (codec == CODEC_AV1  && test_hardware_encoder("av1_nvenc"))  { *is_hw = 1; return "av1_nvenc"; }

        // 3. Try AMD AMF
        if (codec == CODEC_H264 && test_hardware_encoder("h264_amf"))   { *is_hw = 1; return "h264_amf"; }
        if (codec == CODEC_HEVC && test_hardware_encoder("hevc_amf"))   { *is_hw = 1; return "hevc_amf"; }
        if (codec == CODEC_AV1  && test_hardware_encoder("av1_amf"))    { *is_hw = 1; return "av1_amf"; }
    }

    // Software fallback if no GPU encoder is supported
    *is_hw = 0;
    if (codec == CODEC_HEVC && avcodec_find_encoder_by_name("libx265")) return "libx265";
    return "libx264";
}

VideoEncoderContext* ffmpeg_encoder_create(const VideoEncoderConfig *cfg, PacketCallback callback, void *user_data) {
    int is_hw = 0;
    const char *encoder_name = select_encoder_name(cfg->codec, cfg->backend, &is_hw);
    const AVCodec *codec = avcodec_find_encoder_by_name(encoder_name);
    if (!codec) {
        fprintf(stderr, "[FFmpeg] Could not find encoder: %s\n", encoder_name);
        return NULL;
    }

    VideoEncoderContext *ctx = (VideoEncoderContext*)calloc(1, sizeof(VideoEncoderContext));
    if (!ctx) return NULL;

    ctx->width = cfg->width;
    ctx->height = cfg->height;
    ctx->src_width = cfg->src_width ? cfg->src_width : cfg->width;
    ctx->src_height = cfg->src_height ? cfg->src_height : cfg->height;
    ctx->fps = cfg->fps;
    ctx->callback = callback;
    ctx->user_data = user_data;
    ctx->is_hardware = is_hw;
    ctx->capture_cursor = cfg->capture_cursor;
    ctx->last_pts = -1;

    ctx->codec_ctx = avcodec_alloc_context3(codec);
    if (!ctx->codec_ctx) {
        free(ctx);
        return NULL;
    }

    ctx->codec_ctx->width = cfg->width;
    ctx->codec_ctx->height = cfg->height;
    ctx->codec_ctx->time_base = (AVRational){1, (int)cfg->fps};
    ctx->codec_ctx->framerate = (AVRational){(int)cfg->fps, 1};
    ctx->codec_ctx->gop_size = (int)(cfg->fps * (cfg->keyframe_interval_secs ? cfg->keyframe_interval_secs : 2));
    ctx->codec_ctx->max_b_frames = 0; // Low latency: zero B-frames
    ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER; // Emit SPS/PPS in extradata for container muxers

    // Standard high-definition BT.709 color matrix, primaries and transfer function
    ctx->codec_ctx->colorspace = AVCOL_SPC_BT709;
    ctx->codec_ctx->color_range = (cfg->color_range == 1) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
    ctx->codec_ctx->color_primaries = AVCOL_PRI_BT709;
    ctx->codec_ctx->color_trc = AVCOL_TRC_BT709;

    if (strstr(encoder_name, "qsv")) {
        // Intel QuickSync Hardware Video Encoder (runs on Intel GPU ASIC)
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_NV12;
        int qsv_res = av_hwdevice_ctx_create(&ctx->hw_device_ref, AV_HWDEVICE_TYPE_QSV, NULL, NULL, 0);
        if (qsv_res >= 0 && ctx->hw_device_ref) {
            ctx->codec_ctx->hw_device_ctx = av_buffer_ref(ctx->hw_device_ref);
        }
        ctx->is_hardware = 0; // Fed via fast staging NV12 frames, encoded directly by GPU hardware!
    } else if (is_hw && cfg->d3d11_device) {
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_D3D11;

        // Initialize D3D11VA HW device context
        ctx->hw_device_ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (ctx->hw_device_ref) {
            AVHWDeviceContext *device_ctx = (AVHWDeviceContext*)ctx->hw_device_ref->data;
            AVD3D11VADeviceContext *d3d11_hwctx = (AVD3D11VADeviceContext*)device_ctx->hwctx;
            ID3D11Device *pDev = (struct ID3D11Device*)cfg->d3d11_device;
            pDev->lpVtbl->AddRef(pDev);
            d3d11_hwctx->device = pDev;
            if (av_hwdevice_ctx_init(ctx->hw_device_ref) < 0) {
                fprintf(stderr, "[FFmpeg] Failed to init D3D11VA hwdevice\n");
            } else {
                ctx->hw_frames_ref = av_hwframe_ctx_alloc(ctx->hw_device_ref);
                if (ctx->hw_frames_ref) {
                    AVHWFramesContext *frames_ctx = (AVHWFramesContext*)ctx->hw_frames_ref->data;
                    frames_ctx->format = AV_PIX_FMT_D3D11;
                    frames_ctx->sw_format = AV_PIX_FMT_NV12;
                    frames_ctx->width = cfg->width;
                    frames_ctx->height = cfg->height;
                    frames_ctx->initial_pool_size = 4;
                    if (av_hwframe_ctx_init(ctx->hw_frames_ref) >= 0) {
                        ctx->codec_ctx->hw_frames_ctx = av_buffer_ref(ctx->hw_frames_ref);
                    }
                }
            }
        }
    } else {
        ctx->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    }

    // Rate control options
    if (cfg->rate_control == RC_CQP) {
        char qp_str[16];
        snprintf(qp_str, sizeof(qp_str), "%u", cfg->quality_cqp ? cfg->quality_cqp : 23);
        av_opt_set(ctx->codec_ctx->priv_data, "qp", qp_str, 0);
        av_opt_set(ctx->codec_ctx->priv_data, "cq", qp_str, 0);
        av_opt_set(ctx->codec_ctx->priv_data, "cqp", qp_str, 0);
    } else if (cfg->rate_control == RC_CRF) {
        char crf_str[16];
        snprintf(crf_str, sizeof(crf_str), "%u", cfg->quality_cqp ? cfg->quality_cqp : 23);
        av_opt_set(ctx->codec_ctx->priv_data, "crf", crf_str, 0);
        av_opt_set(ctx->codec_ctx->priv_data, "cq", crf_str, 0);
    } else if (cfg->rate_control == RC_CBR) {
        uint32_t br = cfg->bitrate_kbps ? cfg->bitrate_kbps : 15000;
        ctx->codec_ctx->bit_rate = (int64_t)br * 1000;
        ctx->codec_ctx->rc_min_rate = ctx->codec_ctx->bit_rate;
        ctx->codec_ctx->rc_max_rate = ctx->codec_ctx->bit_rate;
        ctx->codec_ctx->rc_buffer_size = (int)(ctx->codec_ctx->bit_rate);
    } else { // RC_VBR
        uint32_t br = cfg->bitrate_kbps ? cfg->bitrate_kbps : 15000;
        ctx->codec_ctx->bit_rate = (int64_t)br * 1000;
        ctx->codec_ctx->rc_max_rate = (int64_t)(br * 1.5) * 1000;
        ctx->codec_ctx->rc_buffer_size = (int)(ctx->codec_ctx->bit_rate * 2);
    }

    // Backend-specific presets & low latency options
    // Backend-specific presets & low latency options
    const char *preset_val = "veryfast";
    if (strstr(encoder_name, "nvenc")) {
        if (cfg->preset && (strstr(cfg->preset, "slow") || strstr(cfg->preset, "quality"))) preset_val = "p6";
        else if (cfg->preset && (strstr(cfg->preset, "medium") || strstr(cfg->preset, "balanced"))) preset_val = "p4";
        else if (cfg->preset && (strstr(cfg->preset, "fast") || strstr(cfg->preset, "speed") || strstr(cfg->preset, "ultrafast"))) preset_val = "p2";
        else preset_val = "p4";
    } else if (strstr(encoder_name, "qsv")) {
        if (cfg->preset && (strstr(cfg->preset, "slow") || strstr(cfg->preset, "quality"))) preset_val = "slow";
        else if (cfg->preset && (strstr(cfg->preset, "medium") || strstr(cfg->preset, "balanced"))) preset_val = "medium";
        else if (cfg->preset && (strstr(cfg->preset, "ultrafast") || strstr(cfg->preset, "superfast"))) preset_val = "veryfast";
        else preset_val = "veryfast";
    } else if (strstr(encoder_name, "amf")) {
        if (cfg->preset && (strstr(cfg->preset, "slow") || strstr(cfg->preset, "quality"))) preset_val = "quality";
        else if (cfg->preset && (strstr(cfg->preset, "medium") || strstr(cfg->preset, "balanced"))) preset_val = "balanced";
        else preset_val = "speed";
    } else { // Software / libx264
        if (cfg->preset && strstr(cfg->preset, "ultrafast")) preset_val = "ultrafast";
        else if (cfg->preset && strstr(cfg->preset, "superfast")) preset_val = "superfast";
        else if (cfg->preset && strstr(cfg->preset, "veryfast")) preset_val = "veryfast";
        else if (cfg->preset && (strstr(cfg->preset, "slow") || strstr(cfg->preset, "quality"))) preset_val = "slow";
        else if (cfg->preset && (strstr(cfg->preset, "medium") || strstr(cfg->preset, "balanced"))) preset_val = "medium";
        else preset_val = "ultrafast";
    }

    av_opt_set(ctx->codec_ctx->priv_data, "preset", preset_val, 0);

    if (strstr(encoder_name, "nvenc")) {
        av_opt_set(ctx->codec_ctx->priv_data, "tune", "ull", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "zerolatency", "1", 0);
    } else if (strstr(encoder_name, "qsv")) {
        av_opt_set(ctx->codec_ctx->priv_data, "preset", "veryfast", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "low_power", "1", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "async_depth", "1", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "look_ahead", "0", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "forced_idr", "1", 0);
    } else if (!is_hw) {
        ctx->codec_ctx->thread_type = FF_THREAD_SLICE;
        av_opt_set(ctx->codec_ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "colorprim", "bt709", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "transfer", "bt709", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "colmatrix", "bt709", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "fullrange", (cfg->color_range == 1) ? "on" : "off", 0);
        av_opt_set(ctx->codec_ctx->priv_data, "x264-params", "no-mbtree=1:rc-lookahead=0:sync-lookahead=0:b-adapt=0", 0);
    }

    // Initialize D3D11 Video Processor & Staging Textures
    if (cfg->d3d11_device && cfg->d3d11_context) {
        ID3D11Device *dev = (ID3D11Device*)cfg->d3d11_device;
        ctx->d3d11_context = (ID3D11DeviceContext*)cfg->d3d11_context;

        // 1. Query ID3D11VideoDevice and ID3D11VideoContext for hardware GPU color conversion
        HRESULT hr_vdev = dev->lpVtbl->QueryInterface(dev, &IID_ID3D11VideoDevice, (void**)&ctx->video_device);
        HRESULT hr_vctx = ctx->d3d11_context->lpVtbl->QueryInterface(ctx->d3d11_context, &IID_ID3D11VideoContext, (void**)&ctx->video_context);

        if (SUCCEEDED(hr_vdev) && SUCCEEDED(hr_vctx) && ctx->video_device && ctx->video_context) {
            D3D11_VIDEO_PROCESSOR_CONTENT_DESC vdesc = {0};
            vdesc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
            vdesc.InputWidth = ctx->src_width;
            vdesc.InputHeight = ctx->src_height;
            vdesc.OutputWidth = ctx->width;
            vdesc.OutputHeight = ctx->height;
            vdesc.Usage = D3D11_VIDEO_USAGE_OPTIMAL_QUALITY;

            HRESULT hr_enum = ctx->video_device->lpVtbl->CreateVideoProcessorEnumerator(
                ctx->video_device, &vdesc, &ctx->video_enumerator);

            if (SUCCEEDED(hr_enum) && ctx->video_enumerator) {
                UINT bgra_flags = 0, nv12_flags = 0;
                ctx->video_enumerator->lpVtbl->CheckVideoProcessorFormat(ctx->video_enumerator, DXGI_FORMAT_B8G8R8A8_UNORM, &bgra_flags);
                ctx->video_enumerator->lpVtbl->CheckVideoProcessorFormat(ctx->video_enumerator, DXGI_FORMAT_NV12, &nv12_flags);

                if ((bgra_flags & 1) && (nv12_flags & 2)) {
                    HRESULT hr_vp = ctx->video_device->lpVtbl->CreateVideoProcessor(
                        ctx->video_device, ctx->video_enumerator, 0, &ctx->video_processor);

                    if (SUCCEEDED(hr_vp) && ctx->video_processor) {
                        // Create GPU NV12 destination texture in VRAM
                        D3D11_TEXTURE2D_DESC nv12_desc = {0};
                        nv12_desc.Width = ctx->width;
                        nv12_desc.Height = ctx->height;
                        nv12_desc.MipLevels = 1;
                        nv12_desc.ArraySize = 1;
                        nv12_desc.Format = DXGI_FORMAT_NV12;
                        nv12_desc.SampleDesc.Count = 1;
                        nv12_desc.Usage = D3D11_USAGE_DEFAULT;
                        nv12_desc.BindFlags = D3D11_BIND_RENDER_TARGET; // Required for VideoProcessorOutputView

                        HRESULT hr_tex = dev->lpVtbl->CreateTexture2D(dev, &nv12_desc, NULL, &ctx->gpu_nv12_texture);
                        if (SUCCEEDED(hr_tex) && ctx->gpu_nv12_texture) {
                            D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC ov_desc = {0};
                            ov_desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
                            ov_desc.Texture2D.MipSlice = 0;

                            HRESULT hr_ov = ctx->video_device->lpVtbl->CreateVideoProcessorOutputView(
                                ctx->video_device, (ID3D11Resource*)ctx->gpu_nv12_texture,
                                ctx->video_enumerator, &ov_desc, &ctx->video_output_view);

                            if (SUCCEEDED(hr_ov) && ctx->video_output_view) {
                                ctx->use_gpu_nv12_staging = true;
                                printf("[FFmpeg] Hardware D3D11 Video Processor initialized (0%% CPU 4K BGRA->NV12 GPU conversion enabled)\n");
                            }
                        }
                    }
                }
            }
        }

        // 2. Allocate staging textures (triple/quad buffered for asynchronous non-blocking readback)
        D3D11_TEXTURE2D_DESC sdesc = {0};
        sdesc.Width = ctx->use_gpu_nv12_staging ? ctx->width : ctx->src_width;
        sdesc.Height = ctx->use_gpu_nv12_staging ? ctx->height : ctx->src_height;
        sdesc.MipLevels = 1;
        sdesc.ArraySize = 1;
        sdesc.Format = ctx->use_gpu_nv12_staging ? DXGI_FORMAT_NV12 : DXGI_FORMAT_B8G8R8A8_UNORM;
        sdesc.SampleDesc.Count = 1;
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int i = 0; i < STAGING_BUFFERS; i++) {
            dev->lpVtbl->CreateTexture2D(dev, &sdesc, NULL, &ctx->staging_textures[i]);
        }
        ctx->staging_index = 0;
        ctx->staging_count = 0;
    }

    if (avcodec_open2(ctx->codec_ctx, codec, NULL) < 0) {
        if (is_hw) {
            fprintf(stderr, "[FFmpeg] Hardware encoder '%s' failed to open. Falling back to software libx264...\n", encoder_name);
            avcodec_free_context(&ctx->codec_ctx);
            if (ctx->hw_frames_ref) av_buffer_unref(&ctx->hw_frames_ref);
            if (ctx->hw_device_ref) av_buffer_unref(&ctx->hw_device_ref);

            ctx->is_hardware = 0;
            const AVCodec *sw_codec = avcodec_find_encoder_by_name("libx264");
            if (sw_codec) {
                ctx->codec_ctx = avcodec_alloc_context3(sw_codec);
                ctx->codec_ctx->width = cfg->width;
                ctx->codec_ctx->height = cfg->height;
                ctx->codec_ctx->time_base = (AVRational){1, (int)cfg->fps};
                ctx->codec_ctx->framerate = (AVRational){(int)cfg->fps, 1};
                ctx->codec_ctx->gop_size = (int)(cfg->fps * (cfg->keyframe_interval_secs ? cfg->keyframe_interval_secs : 2));
                ctx->codec_ctx->max_b_frames = 0;
                ctx->codec_ctx->pix_fmt = AV_PIX_FMT_YUV420P;
                ctx->codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
                ctx->codec_ctx->colorspace = AVCOL_SPC_BT709;
                ctx->codec_ctx->color_range = (cfg->color_range == 1) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
                ctx->codec_ctx->color_primaries = AVCOL_PRI_BT709;
                ctx->codec_ctx->color_trc = AVCOL_TRC_BT709;
                ctx->codec_ctx->thread_type = FF_THREAD_SLICE;
                av_opt_set(ctx->codec_ctx->priv_data, "preset", preset_val, 0);
                av_opt_set(ctx->codec_ctx->priv_data, "tune", "zerolatency", 0);
                av_opt_set(ctx->codec_ctx->priv_data, "colorprim", "bt709", 0);
                av_opt_set(ctx->codec_ctx->priv_data, "transfer", "bt709", 0);
                av_opt_set(ctx->codec_ctx->priv_data, "colmatrix", "bt709", 0);
                av_opt_set(ctx->codec_ctx->priv_data, "fullrange", (cfg->color_range == 1) ? "on" : "off", 0);
                av_opt_set(ctx->codec_ctx->priv_data, "x264-params", "no-mbtree=1:rc-lookahead=0:sync-lookahead=0:b-adapt=0", 0);
                if (cfg->rate_control == RC_CQP) {
                    char qp_str[16];
                    snprintf(qp_str, sizeof(qp_str), "%u", cfg->quality_cqp);
                    av_opt_set(ctx->codec_ctx->priv_data, "qp", qp_str, 0);
                } else {
                    ctx->codec_ctx->bit_rate = (int64_t)cfg->bitrate_kbps * 1000;
                }
                if (avcodec_open2(ctx->codec_ctx, sw_codec, NULL) < 0) {
                    fprintf(stderr, "[FFmpeg] Failed to open fallback codec libx264\n");
                    avcodec_free_context(&ctx->codec_ctx);
                    free(ctx);
                    return NULL;
                }
                encoder_name = "libx264";
            } else {
                free(ctx);
                return NULL;
            }
        } else {
            fprintf(stderr, "[FFmpeg] Failed to open codec %s\n", encoder_name);
            avcodec_free_context(&ctx->codec_ctx);
            free(ctx);
            return NULL;
        }
    }

    ctx->packet = av_packet_alloc();
    ctx->sw_frame = av_frame_alloc();
    ctx->hw_frame = av_frame_alloc();

    printf("[FFmpeg] Encoder initialized successfully: %s (%ux%u @ %ufps, HW=%d, time_base=%d/%d)\n",
           encoder_name, cfg->width, cfg->height, cfg->fps, ctx->is_hardware,
           ctx->codec_ctx->time_base.num, ctx->codec_ctx->time_base.den);
    return ctx;
}

static void drain_encoder_packets(VideoEncoderContext *ctx) {
    while (avcodec_receive_packet(ctx->codec_ctx, ctx->packet) == 0) {
        if (ctx->callback) {
            EncodedPacket pkt = {
                .data = ctx->packet->data,
                .size = (size_t)ctx->packet->size,
                .pts = ctx->packet->pts,
                .dts = ctx->packet->dts,
                .is_keyframe = (ctx->packet->flags & AV_PKT_FLAG_KEY) ? 1 : 0,
            };
            ctx->callback(ctx->user_data, &pkt);
        }
        av_packet_unref(ctx->packet);
    }
}

static int64_t get_next_monotonic_pts(VideoEncoderContext *ctx, int64_t pts_nanos) {
    int64_t pts = (int64_t)((double)pts_nanos / 1e9 * (double)ctx->fps + 0.5);
    if (pts <= ctx->last_pts) {
        pts = ctx->last_pts + 1;
    }
    ctx->last_pts = pts;
    return pts;
}



static inline void convert_bgra_to_nv12_fast(
    const uint8_t * __restrict src_bgra, uint32_t src_stride,
    uint8_t * __restrict dst_y, uint32_t dst_stride_y,
    uint8_t * __restrict dst_uv, uint32_t dst_stride_uv,
    uint32_t width, uint32_t height,
    int full_range
) {
    const int yr = full_range ? 54 : 47;
    const int yg = full_range ? 183 : 157;
    const int yb = full_range ? 18 : 16;
    const int y_off = full_range ? 0 : 16;

    const int ur = full_range ? -29 : -26;
    const int ug = full_range ? -99 : -87;
    const int ub = full_range ? 128 : 113;

    const int vr = full_range ? 128 : 113;
    const int vg = full_range ? -116 : -102;
    const int vb = full_range ? -12 : -11;

    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t *p0 = src_bgra + y * src_stride;
        const uint8_t *p1 = src_bgra + (y + 1) * src_stride;
        uint8_t *py0 = dst_y + y * dst_stride_y;
        uint8_t *py1 = dst_y + (y + 1) * dst_stride_y;
        uint8_t *puv = dst_uv + (y / 2) * dst_stride_uv;

        for (uint32_t x = 0; x < width; x += 2) {
            int b00 = p0[0]; int g00 = p0[1]; int r00 = p0[2];
            int b01 = p0[4]; int g01 = p0[5]; int r01 = p0[6];
            int b10 = p1[0]; int g10 = p1[1]; int r10 = p1[2];
            int b11 = p1[4]; int g11 = p1[5]; int r11 = p1[6];

            int y00 = y_off + ((yr * r00 + yg * g00 + yb * b00 + 128) >> 8);
            int y01 = y_off + ((yr * r01 + yg * g01 + yb * b01 + 128) >> 8);
            int y10 = y_off + ((yr * r10 + yg * g10 + yb * b10 + 128) >> 8);
            int y11 = y_off + ((yr * r11 + yg * g11 + yb * b11 + 128) >> 8);

            py0[0] = (uint8_t)(y00 < 0 ? 0 : (y00 > 255 ? 255 : y00));
            py0[1] = (uint8_t)(y01 < 0 ? 0 : (y01 > 255 ? 255 : y01));
            py1[0] = (uint8_t)(y10 < 0 ? 0 : (y10 > 255 ? 255 : y10));
            py1[1] = (uint8_t)(y11 < 0 ? 0 : (y11 > 255 ? 255 : y11));

            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;

            int u_val = 128 + ((ur * r_avg + ug * g_avg + ub * b_avg + 128) >> 8);
            int v_val = 128 + ((vr * r_avg + vg * g_avg + vb * b_avg + 128) >> 8);

            puv[0] = (uint8_t)(u_val < 0 ? 0 : (u_val > 255 ? 255 : u_val));
            puv[1] = (uint8_t)(v_val < 0 ? 0 : (v_val > 255 ? 255 : v_val));

            p0 += 8;
            p1 += 8;
            py0 += 2;
            py1 += 2;
            puv += 2;
        }
    }
}

static inline void convert_bgra_to_yuv420p_fast(
    const uint8_t * __restrict src_bgra, uint32_t src_stride,
    uint8_t * __restrict dst_y, uint32_t dst_stride_y,
    uint8_t * __restrict dst_u, uint32_t dst_stride_u,
    uint8_t * __restrict dst_v, uint32_t dst_stride_v,
    uint32_t width, uint32_t height,
    int full_range
) {
    const int yr = full_range ? 54 : 47;
    const int yg = full_range ? 183 : 157;
    const int yb = full_range ? 18 : 16;
    const int y_off = full_range ? 0 : 16;

    const int ur = full_range ? -29 : -26;
    const int ug = full_range ? -99 : -87;
    const int ub = full_range ? 128 : 113;

    const int vr = full_range ? 128 : 113;
    const int vg = full_range ? -116 : -102;
    const int vb = full_range ? -12 : -11;

    for (uint32_t y = 0; y < height; y += 2) {
        const uint8_t *p0 = src_bgra + y * src_stride;
        const uint8_t *p1 = src_bgra + (y + 1) * src_stride;
        uint8_t *py0 = dst_y + y * dst_stride_y;
        uint8_t *py1 = dst_y + (y + 1) * dst_stride_y;
        uint8_t *pu = dst_u + (y / 2) * dst_stride_u;
        uint8_t *pv = dst_v + (y / 2) * dst_stride_v;

        for (uint32_t x = 0; x < width; x += 2) {
            int b00 = p0[0]; int g00 = p0[1]; int r00 = p0[2];
            int b01 = p0[4]; int g01 = p0[5]; int r01 = p0[6];
            int b10 = p1[0]; int g10 = p1[1]; int r10 = p1[2];
            int b11 = p1[4]; int g11 = p1[5]; int r11 = p1[6];

            int y00 = y_off + ((yr * r00 + yg * g00 + yb * b00 + 128) >> 8);
            int y01 = y_off + ((yr * r01 + yg * g01 + yb * b01 + 128) >> 8);
            int y10 = y_off + ((yr * r10 + yg * g10 + yb * b10 + 128) >> 8);
            int y11 = y_off + ((yr * r11 + yg * g11 + yb * b11 + 128) >> 8);

            py0[0] = (uint8_t)(y00 < 0 ? 0 : (y00 > 255 ? 255 : y00));
            py0[1] = (uint8_t)(y01 < 0 ? 0 : (y01 > 255 ? 255 : y01));
            py1[0] = (uint8_t)(y10 < 0 ? 0 : (y10 > 255 ? 255 : y10));
            py1[1] = (uint8_t)(y11 < 0 ? 0 : (y11 > 255 ? 255 : y11));

            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;

            int u_val = 128 + ((ur * r_avg + ug * g_avg + ub * b_avg + 128) >> 8);
            int v_val = 128 + ((vr * r_avg + vg * g_avg + vb * b_avg + 128) >> 8);

            pu[0] = (uint8_t)(u_val < 0 ? 0 : (u_val > 255 ? 255 : u_val));
            pv[0] = (uint8_t)(v_val < 0 ? 0 : (v_val > 255 ? 255 : v_val));

            p0 += 8;
            p1 += 8;
            py0 += 2;
            py1 += 2;
            pu += 1;
            pv += 1;
        }
    }
}

static inline void convert_bgra_to_nv12_scaled_fast(
    const uint8_t *src_bgra, uint32_t src_stride,
    uint32_t src_width, uint32_t src_height,
    uint8_t *dst_y, uint32_t dst_stride_y,
    uint8_t *dst_uv, uint32_t dst_stride_uv,
    uint32_t dst_width, uint32_t dst_height,
    int full_range
) {
    const int yr = full_range ? 54 : 47;
    const int yg = full_range ? 183 : 157;
    const int yb = full_range ? 18 : 16;
    const int y_off = full_range ? 0 : 16;

    const int ur = full_range ? -29 : -26;
    const int ug = full_range ? -99 : -87;
    const int ub = full_range ? 128 : 113;

    const int vr = full_range ? 128 : 113;
    const int vg = full_range ? -116 : -102;
    const int vb = full_range ? -12 : -11;

    uint32_t step_x = (src_width << 16) / dst_width;
    uint32_t step_y = (src_height << 16) / dst_height;

    for (uint32_t dy = 0; dy < dst_height; dy += 2) {
        uint32_t sy0 = (dy * step_y) >> 16;
        uint32_t sy1 = ((dy + 1) * step_y) >> 16;
        if (sy0 >= src_height) sy0 = src_height - 1;
        if (sy1 >= src_height) sy1 = src_height - 1;

        const uint8_t *row0 = src_bgra + sy0 * src_stride;
        const uint8_t *row1 = src_bgra + sy1 * src_stride;

        uint8_t *py0 = dst_y + dy * dst_stride_y;
        uint8_t *py1 = dst_y + (dy + 1) * dst_stride_y;
        uint8_t *puv = dst_uv + (dy / 2) * dst_stride_uv;

        for (uint32_t dx = 0; dx < dst_width; dx += 2) {
            uint32_t sx0 = (dx * step_x) >> 16;
            uint32_t sx1 = ((dx + 1) * step_x) >> 16;
            if (sx0 >= src_width) sx0 = src_width - 1;
            if (sx1 >= src_width) sx1 = src_width - 1;

            const uint8_t *p00 = row0 + sx0 * 4;
            const uint8_t *p01 = row0 + sx1 * 4;
            const uint8_t *p10 = row1 + sx0 * 4;
            const uint8_t *p11 = row1 + sx1 * 4;

            int b00 = p00[0]; int g00 = p00[1]; int r00 = p00[2];
            int b01 = p01[4]; int g01 = p01[5]; int r01 = p01[6];
            int b10 = p10[0]; int g10 = p10[1]; int r10 = p10[2];
            int b11 = p11[4]; int g11 = p11[5]; int r11 = p11[6];

            int y00 = y_off + ((yr * r00 + yg * g00 + yb * b00 + 128) >> 8);
            int y01 = y_off + ((yr * r01 + yg * g01 + yb * b01 + 128) >> 8);
            int y10 = y_off + ((yr * r10 + yg * g10 + yb * b10 + 128) >> 8);
            int y11 = y_off + ((yr * r11 + yg * g11 + yb * b11 + 128) >> 8);

            py0[0] = (uint8_t)(y00 < 0 ? 0 : (y00 > 255 ? 255 : y00));
            py0[1] = (uint8_t)(y01 < 0 ? 0 : (y01 > 255 ? 255 : y01));
            py1[0] = (uint8_t)(y10 < 0 ? 0 : (y10 > 255 ? 255 : y10));
            py1[1] = (uint8_t)(y11 < 0 ? 0 : (y11 > 255 ? 255 : y11));

            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;

            int u_val = 128 + ((ur * r_avg + ug * g_avg + ub * b_avg + 128) >> 8);
            int v_val = 128 + ((vr * r_avg + vg * g_avg + vb * b_avg + 128) >> 8);

            puv[0] = (uint8_t)(u_val < 0 ? 0 : (u_val > 255 ? 255 : u_val));
            puv[1] = (uint8_t)(v_val < 0 ? 0 : (v_val > 255 ? 255 : v_val));

            py0 += 2;
            py1 += 2;
            puv += 2;
        }
    }
}

static inline void convert_bgra_to_yuv420p_scaled_fast(
    const uint8_t *src_bgra, uint32_t src_stride,
    uint32_t src_width, uint32_t src_height,
    uint8_t *dst_y, uint32_t dst_stride_y,
    uint8_t *dst_u, uint32_t dst_stride_u,
    uint8_t *dst_v, uint32_t dst_stride_v,
    uint32_t dst_width, uint32_t dst_height,
    int full_range
) {
    const int yr = full_range ? 54 : 47;
    const int yg = full_range ? 183 : 157;
    const int yb = full_range ? 18 : 16;
    const int y_off = full_range ? 0 : 16;

    const int ur = full_range ? -29 : -26;
    const int ug = full_range ? -99 : -87;
    const int ub = full_range ? 128 : 113;

    const int vr = full_range ? 128 : 113;
    const int vg = full_range ? -116 : -102;
    const int vb = full_range ? -12 : -11;

    uint32_t step_x = (src_width << 16) / dst_width;
    uint32_t step_y = (src_height << 16) / dst_height;

    for (uint32_t dy = 0; dy < dst_height; dy += 2) {
        uint32_t sy0 = (dy * step_y) >> 16;
        uint32_t sy1 = ((dy + 1) * step_y) >> 16;
        if (sy0 >= src_height) sy0 = src_height - 1;
        if (sy1 >= src_height) sy1 = src_height - 1;

        const uint8_t *row0 = src_bgra + sy0 * src_stride;
        const uint8_t *row1 = src_bgra + sy1 * src_stride;

        uint8_t *py0 = dst_y + dy * dst_stride_y;
        uint8_t *py1 = dst_y + (dy + 1) * dst_stride_y;
        uint8_t *pu = dst_u + (dy / 2) * dst_stride_u;
        uint8_t *pv = dst_v + (dy / 2) * dst_stride_v;

        for (uint32_t dx = 0; dx < dst_width; dx += 2) {
            uint32_t sx0 = (dx * step_x) >> 16;
            uint32_t sx1 = ((dx + 1) * step_x) >> 16;
            if (sx0 >= src_width) sx0 = src_width - 1;
            if (sx1 >= src_width) sx1 = src_width - 1;

            const uint8_t *p00 = row0 + sx0 * 4;
            const uint8_t *p01 = row0 + sx1 * 4;
            const uint8_t *p10 = row1 + sx0 * 4;
            const uint8_t *p11 = row1 + sx1 * 4;

            int b00 = p00[0]; int g00 = p00[1]; int r00 = p00[2];
            int b01 = p01[4]; int g01 = p01[5]; int r01 = p01[6];
            int b10 = p10[0]; int g10 = p10[1]; int r10 = p10[2];
            int b11 = p11[4]; int g11 = p11[5]; int r11 = p11[6];

            int y00 = y_off + ((yr * r00 + yg * g00 + yb * b00 + 128) >> 8);
            int y01 = y_off + ((yr * r01 + yg * g01 + yb * b01 + 128) >> 8);
            int y10 = y_off + ((yr * r10 + yg * g10 + yb * b10 + 128) >> 8);
            int y11 = y_off + ((yr * r11 + yg * g11 + yb * b11 + 128) >> 8);

            py0[0] = (uint8_t)(y00 < 0 ? 0 : (y00 > 255 ? 255 : y00));
            py0[1] = (uint8_t)(y01 < 0 ? 0 : (y01 > 255 ? 255 : y01));
            py1[0] = (uint8_t)(y10 < 0 ? 0 : (y10 > 255 ? 255 : y10));
            py1[1] = (uint8_t)(y11 < 0 ? 0 : (y11 > 255 ? 255 : y11));

            int b_avg = (b00 + b01 + b10 + b11 + 2) >> 2;
            int g_avg = (g00 + g01 + g10 + g11 + 2) >> 2;
            int r_avg = (r00 + r01 + r10 + r11 + 2) >> 2;

            int u_val = 128 + ((ur * r_avg + ug * g_avg + ub * b_avg + 128) >> 8);
            int v_val = 128 + ((vr * r_avg + vg * g_avg + vb * b_avg + 128) >> 8);

            pu[0] = (uint8_t)(u_val < 0 ? 0 : (u_val > 255 ? 255 : u_val));
            pv[0] = (uint8_t)(v_val < 0 ? 0 : (v_val > 255 ? 255 : v_val));

            py0 += 2;
            py1 += 2;
            pu += 1;
            pv += 1;
        }
    }
}

static HCURSOR s_cached_cursor = NULL;
static uint32_t s_cursor_cache[32 * 32];
static int s_cached_hot_x = 0;
static int s_cached_hot_y = 0;
static bool s_has_cursor = false;

static void update_cursor_cache(HCURSOR cur) {
    if (cur == s_cached_cursor && s_has_cursor) return;
    s_cached_cursor = cur;
    s_has_cursor = false;

    ICONINFO ii = {0};
    if (!GetIconInfo(cur, &ii)) return;
    s_cached_hot_x = (int)ii.xHotspot;
    s_cached_hot_y = (int)ii.yHotspot;

    HDC screen_dc = GetDC(NULL);
    HDC mem_dc = CreateCompatibleDC(screen_dc);

    BITMAPINFO bmi = {0};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = 32;
    bmi.bmiHeader.biHeight = -32; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint32_t *pixels = NULL;
    HBITMAP dib = CreateDIBSection(mem_dc, &bmi, DIB_RGB_COLORS, (void**)&pixels, NULL, 0);
    if (dib && pixels) {
        HBITMAP old_bmp = (HBITMAP)SelectObject(mem_dc, dib);

        // Pass 1: Draw on pure BLACK background (0x00000000)
        memset(pixels, 0, 32 * 32 * sizeof(uint32_t));
        DrawIconEx(mem_dc, 0, 0, cur, 32, 32, 0, NULL, DI_NORMAL);
        uint32_t black_pass[32 * 32];
        memcpy(black_pass, pixels, sizeof(black_pass));

        // Pass 2: Draw on pure WHITE background (0x00FFFFFF)
        for (int i = 0; i < 32 * 32; i++) pixels[i] = 0x00FFFFFF;
        DrawIconEx(mem_dc, 0, 0, cur, 32, 32, 0, NULL, DI_NORMAL);

        // Dual-pass transparency & alpha reconstruction
        for (int i = 0; i < 32 * 32; i++) {
            uint32_t b_px = black_pass[i] & 0x00FFFFFF;
            uint32_t w_px = pixels[i] & 0x00FFFFFF;

            if (b_px == w_px) {
                // Completely opaque pixel (standard border, body, or color)
                s_cursor_cache[i] = 0xFF000000 | b_px;
            } else if (b_px == 0x00000000 && w_px == 0x00FFFFFF) {
                // Completely transparent background
                s_cursor_cache[i] = 0;
            } else if (b_px == 0x00FFFFFF && w_px == 0x00000000) {
                // Inverted pixel (render as crisp white with dark shadow)
                s_cursor_cache[i] = 0xFFFFFFFF;
            } else {
                // Alpha-blended cursor pixel (Aero / custom cursor)
                int r_diff = ((w_px >> 16) & 0xFF) - ((b_px >> 16) & 0xFF);
                int alpha = 255 - r_diff;
                if (alpha < 0) alpha = 0;
                if (alpha > 255) alpha = 255;
                s_cursor_cache[i] = ((uint32_t)alpha << 24) | b_px;
            }
        }

        s_has_cursor = true;
        SelectObject(mem_dc, old_bmp);
        DeleteObject(dib);
    }

    DeleteDC(mem_dc);
    ReleaseDC(NULL, screen_dc);

    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

static void draw_cursor_onto_frame(VideoEncoderContext *ctx) {
    if (!ctx || !ctx->sw_frame || !ctx->sw_frame->data[0]) return;

    CURSORINFO ci = { sizeof(CURSORINFO) };
    if (!GetCursorInfo(&ci)) return;
    if (!(ci.flags & 1) || !ci.hCursor) return; // 1 = CURSOR_SHOWING

    if (ci.hCursor != s_cached_cursor || !s_has_cursor) {
        update_cursor_cache(ci.hCursor);
    }
    if (!s_has_cursor) return;

    int cur_x = ci.ptScreenPos.x - s_cached_hot_x;
    int cur_y = ci.ptScreenPos.y - s_cached_hot_y;

    if (ctx->src_width != ctx->width && ctx->src_width > 0) {
        cur_x = (cur_x * (int)ctx->width) / (int)ctx->src_width;
        cur_y = (cur_y * (int)ctx->height) / (int)ctx->src_height;
    }

    if (cur_x + 32 <= 0 || cur_y + 32 <= 0 || cur_x >= (int)ctx->width || cur_y >= (int)ctx->height) return;

    int full_range = (ctx->codec_ctx->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    const int yr = full_range ? 54 : 47;
    const int yg = full_range ? 183 : 157;
    const int yb = full_range ? 18 : 16;
    const int y_off = full_range ? 0 : 16;

    const int ur = full_range ? -29 : -26;
    const int ug = full_range ? -99 : -87;
    const int ub = full_range ? 128 : 113;

    const int vr = full_range ? 128 : 113;
    const int vg = full_range ? -116 : -102;
    const int vb = full_range ? -12 : -11;

    uint8_t *dst_y = ctx->sw_frame->data[0];
    uint32_t stride_y = (uint32_t)ctx->sw_frame->linesize[0];

    enum AVPixelFormat fmt = ctx->codec_ctx->pix_fmt;
    if (fmt == AV_PIX_FMT_NV12) {
        uint8_t *dst_uv = ctx->sw_frame->data[1];
        uint32_t stride_uv = (uint32_t)ctx->sw_frame->linesize[1];

        for (int cy = 0; cy < 32; cy++) {
            int ty = cur_y + cy;
            if (ty < 0 || ty >= (int)ctx->height) continue;

            const uint32_t *src_row = s_cursor_cache + cy * 32;
            for (int cx = 0; cx < 32; cx++) {
                int tx = cur_x + cx;
                if (tx < 0 || tx >= (int)ctx->width) continue;

                uint32_t pixel = src_row[cx];
                uint8_t a = (uint8_t)(pixel >> 24);
                if (a == 0) continue;

                uint8_t r = (uint8_t)(pixel >> 16);
                uint8_t g = (uint8_t)(pixel >> 8);
                uint8_t b = (uint8_t)(pixel);

                int y_val = y_off + ((yr * r + yg * g + yb * b + 128) >> 8);
                if (y_val < 0) y_val = 0; else if (y_val > 255) y_val = 255;

                uint8_t *py = dst_y + ty * stride_y + tx;
                if (a == 255) {
                    *py = (uint8_t)y_val;
                } else {
                    *py = (uint8_t)((y_val * a + (*py) * (255 - a)) / 255);
                }

                // UV plane (NV12: interleaved U, V)
                int uv_x = (tx & ~1);
                int uv_y = ty / 2;
                uint8_t *puv = dst_uv + uv_y * stride_uv + uv_x;

                int u_val = 128 + ((ur * r + ug * g + ub * b + 128) >> 8);
                int v_val = 128 + ((vr * r + vg * g + vb * b + 128) >> 8);
                if (u_val < 0) u_val = 0; else if (u_val > 255) u_val = 255;
                if (v_val < 0) v_val = 0; else if (v_val > 255) v_val = 255;

                if (a == 255) {
                    puv[0] = (uint8_t)u_val;
                    puv[1] = (uint8_t)v_val;
                } else {
                    puv[0] = (uint8_t)((u_val * a + puv[0] * (255 - a)) / 255);
                    puv[1] = (uint8_t)((v_val * a + puv[1] * (255 - a)) / 255);
                }
            }
        }
    } else { // YUV420P
        uint8_t *dst_u = ctx->sw_frame->data[1];
        uint32_t stride_u = (uint32_t)ctx->sw_frame->linesize[1];
        uint8_t *dst_v = ctx->sw_frame->data[2];
        uint32_t stride_v = (uint32_t)ctx->sw_frame->linesize[2];

        for (int cy = 0; cy < 32; cy++) {
            int ty = cur_y + cy;
            if (ty < 0 || ty >= (int)ctx->height) continue;

            const uint32_t *src_row = s_cursor_cache + cy * 32;
            for (int cx = 0; cx < 32; cx++) {
                int tx = cur_x + cx;
                if (tx < 0 || tx >= (int)ctx->width) continue;

                uint32_t pixel = src_row[cx];
                uint8_t a = (uint8_t)(pixel >> 24);
                if (a == 0) continue;

                uint8_t r = (uint8_t)(pixel >> 16);
                uint8_t g = (uint8_t)(pixel >> 8);
                uint8_t b = (uint8_t)(pixel);

                int y_val = y_off + ((yr * r + yg * g + yb * b + 128) >> 8);
                if (y_val < 0) y_val = 0; else if (y_val > 255) y_val = 255;

                uint8_t *py = dst_y + ty * stride_y + tx;
                if (a == 255) {
                    *py = (uint8_t)y_val;
                } else {
                    *py = (uint8_t)((y_val * a + (*py) * (255 - a)) / 255);
                }

                int u_val = 128 + ((ur * r + ug * g + ub * b + 128) >> 8);
                int v_val = 128 + ((vr * r + vg * g + vb * b + 128) >> 8);
                if (u_val < 0) u_val = 0; else if (u_val > 255) u_val = 255;
                if (v_val < 0) v_val = 0; else if (v_val > 255) v_val = 255;

                uint8_t *pu = dst_u + (ty / 2) * stride_u + (tx / 2);
                uint8_t *pv = dst_v + (ty / 2) * stride_v + (tx / 2);

                if (a == 255) {
                    *pu = (uint8_t)u_val;
                    *pv = (uint8_t)v_val;
                } else {
                    *pu = (uint8_t)((u_val * a + (*pu) * (255 - a)) / 255);
                    *pv = (uint8_t)((v_val * a + (*pv) * (255 - a)) / 255);
                }
            }
        }
    }
}

static inline void prepare_sw_frame_if_needed(VideoEncoderContext *ctx) {
    if (ctx->sw_frame->width == 0) {
        enum AVPixelFormat target_fmt = ctx->codec_ctx->pix_fmt;
        if (target_fmt == AV_PIX_FMT_NONE) target_fmt = AV_PIX_FMT_YUV420P;
        ctx->sw_frame->format = target_fmt;
        ctx->sw_frame->width = ctx->width;
        ctx->sw_frame->height = ctx->height;
        ctx->sw_frame->colorspace = AVCOL_SPC_BT709;
        ctx->sw_frame->color_range = ctx->codec_ctx->color_range;
        ctx->sw_frame->color_primaries = AVCOL_PRI_BT709;
        ctx->sw_frame->color_trc = AVCOL_TRC_BT709;
        av_frame_get_buffer(ctx->sw_frame, 0);
    }
}

static inline void convert_source_bgra_to_sw_frame(
    VideoEncoderContext *ctx,
    const uint8_t *src_bgra,
    uint32_t src_stride
) {
    prepare_sw_frame_if_needed(ctx);
    int full_range = (ctx->codec_ctx->color_range == AVCOL_RANGE_JPEG) ? 1 : 0;
    enum AVPixelFormat target_fmt = ctx->codec_ctx->pix_fmt;
    if (target_fmt == AV_PIX_FMT_NONE) target_fmt = AV_PIX_FMT_YUV420P;

    if (ctx->src_width == ctx->width && ctx->src_height == ctx->height) {
        if (target_fmt == AV_PIX_FMT_NV12) {
            convert_bgra_to_nv12_fast(
                src_bgra, src_stride,
                ctx->sw_frame->data[0], (uint32_t)ctx->sw_frame->linesize[0],
                ctx->sw_frame->data[1], (uint32_t)ctx->sw_frame->linesize[1],
                ctx->width, ctx->height, full_range
            );
        } else {
            convert_bgra_to_yuv420p_fast(
                src_bgra, src_stride,
                ctx->sw_frame->data[0], (uint32_t)ctx->sw_frame->linesize[0],
                ctx->sw_frame->data[1], (uint32_t)ctx->sw_frame->linesize[1],
                ctx->sw_frame->data[2], (uint32_t)ctx->sw_frame->linesize[2],
                ctx->width, ctx->height, full_range
            );
        }
    } else {
        if (target_fmt == AV_PIX_FMT_NV12) {
            convert_bgra_to_nv12_scaled_fast(
                src_bgra, src_stride,
                ctx->src_width, ctx->src_height,
                ctx->sw_frame->data[0], (uint32_t)ctx->sw_frame->linesize[0],
                ctx->sw_frame->data[1], (uint32_t)ctx->sw_frame->linesize[1],
                ctx->width, ctx->height, full_range
            );
        } else {
            convert_bgra_to_yuv420p_scaled_fast(
                src_bgra, src_stride,
                ctx->src_width, ctx->src_height,
                ctx->sw_frame->data[0], (uint32_t)ctx->sw_frame->linesize[0],
                ctx->sw_frame->data[1], (uint32_t)ctx->sw_frame->linesize[1],
                ctx->sw_frame->data[2], (uint32_t)ctx->sw_frame->linesize[2],
                ctx->width, ctx->height, full_range
            );
        }
    }
}

static int encode_sw_frame(VideoEncoderContext *ctx, int64_t pts_nanos) {
    if (ctx->capture_cursor) {
        draw_cursor_onto_frame(ctx);
    }
    int64_t pts = get_next_monotonic_pts(ctx, pts_nanos);
    ctx->sw_frame->pts = pts;
    int ret = avcodec_send_frame(ctx->codec_ctx, ctx->sw_frame);
    if (ret < 0) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[FFmpeg] avcodec_send_frame(sw) failed: %d (%s) at pts=%lld\n", ret, errbuf, (long long)pts);
        return ret;
    }
    drain_encoder_packets(ctx);
    return 0;
}

int ffmpeg_encoder_send_d3d11_texture(VideoEncoderContext *ctx, void *d3d11_texture, int64_t pts_nanos) {
    if (!ctx || !ctx->codec_ctx || !d3d11_texture) return -1;

    // Step 1: Hardware GPU Color Conversion via ID3D11VideoProcessor (0% CPU)
    if (ctx->video_processor && ctx->video_output_view && ctx->gpu_nv12_texture && ctx->video_context && ctx->video_device) {
        // Lazily create or update VideoProcessorInputView if input texture pointer changed
        if (ctx->last_input_resource != (ID3D11Resource*)d3d11_texture || !ctx->video_input_view) {
            if (ctx->video_input_view) {
                ctx->video_input_view->lpVtbl->Release(ctx->video_input_view);
                ctx->video_input_view = NULL;
            }
            D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC iv_desc = {0};
            iv_desc.FourCC = 0;
            iv_desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
            iv_desc.Texture2D.MipSlice = 0;
            iv_desc.Texture2D.ArraySlice = 0;
            HRESULT hr = ctx->video_device->lpVtbl->CreateVideoProcessorInputView(
                ctx->video_device, (ID3D11Resource*)d3d11_texture,
                ctx->video_enumerator, &iv_desc, &ctx->video_input_view);
            if (FAILED(hr)) {
                ctx->video_input_view = NULL;
                ctx->last_input_resource = NULL;
            } else {
                ctx->last_input_resource = (ID3D11Resource*)d3d11_texture;
            }
        }

        if (ctx->video_input_view) {
            D3D11_VIDEO_PROCESSOR_STREAM stream = {0};
            stream.Enable = TRUE;
            stream.OutputIndex = 0;
            stream.InputFrameOrField = 0;
            stream.pInputSurface = ctx->video_input_view;

            ctx->video_context->lpVtbl->VideoProcessorBlt(
                ctx->video_context, ctx->video_processor, ctx->video_output_view, 0, 1, &stream);
        }
    }

    // Step 2: Pure Hardware Zero-Copy Direct Path (NVENC, AMF, QSV D3D11VA)
    // Feeds the GPU NV12 texture directly into avcodec_send_frame in VRAM!
    if (ctx->is_hardware && ctx->hw_frames_ref && ctx->gpu_nv12_texture) {
        int64_t pts = get_next_monotonic_pts(ctx, pts_nanos);
        ctx->hw_frame->format = AV_PIX_FMT_D3D11;
        ctx->hw_frame->width = ctx->width;
        ctx->hw_frame->height = ctx->height;
        ctx->hw_frame->colorspace = AVCOL_SPC_BT709;
        ctx->hw_frame->color_range = ctx->codec_ctx->color_range;
        ctx->hw_frame->color_primaries = AVCOL_PRI_BT709;
        ctx->hw_frame->color_trc = AVCOL_TRC_BT709;
        ctx->hw_frame->pts = pts;
        ctx->hw_frame->data[0] = (uint8_t*)ctx->gpu_nv12_texture;
        ctx->hw_frame->data[1] = 0; // subresource index

        int ret = avcodec_send_frame(ctx->codec_ctx, ctx->hw_frame);
        if (ret == 0) {
            drain_encoder_packets(ctx);
            return 0;
        }
        static int s_logged_hw_err = 0;
        if (!s_logged_hw_err) {
            s_logged_hw_err = 1;
            char errbuf[128] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[FFmpeg] Direct hw send_frame returned %d (%s), using ultra-fast GPU NV12 staging pipeline\n", ret, errbuf);
        }
    }

    // Step 3: Ultra-Fast Asynchronous Ring-Buffer Staging Pipeline
    // Zero-CPU overhead if use_gpu_nv12_staging is true (only memcpy of already converted NV12 planes!)
    if (ctx->staging_textures[0] && ctx->d3d11_context) {
        int write_idx = ctx->staging_index;
        ctx->staging_pts[write_idx] = pts_nanos;

        // Source resource to copy: if GPU converted to NV12, copy that; otherwise copy BGRA texture
        ID3D11Resource *src_to_stage = (ctx->use_gpu_nv12_staging && ctx->gpu_nv12_texture)
            ? (ID3D11Resource*)ctx->gpu_nv12_texture
            : (ID3D11Resource*)d3d11_texture;

        ctx->d3d11_context->lpVtbl->CopyResource(
            ctx->d3d11_context,
            (ID3D11Resource*)ctx->staging_textures[write_idx],
            src_to_stage
        );

        ctx->staging_index = (ctx->staging_index + 1) % STAGING_BUFFERS;

        // Fill initial ring buffer pipeline (need STAGING_BUFFERS - 1 frames buffered before reading)
        if (ctx->staging_count < (STAGING_BUFFERS - 1)) {
            ctx->staging_count++;
            return 0;
        }

        // Read the oldest completed frame: GPU DMA has finished 3 ticks ago
        int read_idx = ctx->staging_index;
        D3D11_MAPPED_SUBRESOURCE mapped;
        // 0x00100000 = D3D11_MAP_FLAG_DO_NOT_WAIT (never stall game render thread!)
        HRESULT hr = ctx->d3d11_context->lpVtbl->Map(
            ctx->d3d11_context,
            (ID3D11Resource*)ctx->staging_textures[read_idx],
            0,
            D3D11_MAP_READ,
            0x00100000,
            &mapped
        );

        if (hr == (HRESULT)0x887A000A) {
            // DXGI_ERROR_WAS_STILL_DRAWING: GPU is under 100% load, do NOT stall the game!
            return 0;
        }

        if (FAILED(hr)) {
            hr = ctx->d3d11_context->lpVtbl->Map(
                ctx->d3d11_context,
                (ID3D11Resource*)ctx->staging_textures[read_idx],
                0,
                D3D11_MAP_READ,
                0,
                &mapped
            );
        }

        if (SUCCEEDED(hr)) {
            if (ctx->use_gpu_nv12_staging) {
                // GPU already converted BGRA -> NV12 in hardware!
                // Zero CPU math! Directly pass NV12 planes with fast contiguous memcpy:
                prepare_sw_frame_if_needed(ctx);
                const uint8_t *src_y = (const uint8_t*)mapped.pData;
                const uint8_t *src_uv = (const uint8_t*)mapped.pData + ctx->height * mapped.RowPitch;

                uint8_t *dst_y = ctx->sw_frame->data[0];
                uint8_t *dst_uv = ctx->sw_frame->data[1];
                uint32_t dst_stride_y = (uint32_t)ctx->sw_frame->linesize[0];
                uint32_t dst_stride_uv = (uint32_t)ctx->sw_frame->linesize[1];

                if (mapped.RowPitch == dst_stride_y && mapped.RowPitch == ctx->width) {
                    memcpy(dst_y, src_y, ctx->width * ctx->height);
                } else {
                    for (uint32_t r = 0; r < ctx->height; r++) {
                        memcpy(dst_y + r * dst_stride_y, src_y + r * mapped.RowPitch, ctx->width);
                    }
                }

                uint32_t uv_lines = ctx->height / 2;
                if (mapped.RowPitch == dst_stride_uv && mapped.RowPitch == ctx->width) {
                    memcpy(dst_uv, src_uv, ctx->width * uv_lines);
                } else {
                    for (uint32_t r = 0; r < uv_lines; r++) {
                        memcpy(dst_uv + r * dst_stride_uv, src_uv + r * mapped.RowPitch, ctx->width);
                    }
                }
            } else {
                // Legacy CPU conversion fallback
                convert_source_bgra_to_sw_frame(ctx, (const uint8_t*)mapped.pData, mapped.RowPitch);
            }

            // Immediately unmap to release GPU staging lock in < 0.1ms
            ctx->d3d11_context->lpVtbl->Unmap(ctx->d3d11_context, (ID3D11Resource*)ctx->staging_textures[read_idx], 0);

            // Encode frame
            return encode_sw_frame(ctx, ctx->staging_pts[read_idx]);
        }
    }

    return -1;
}

int ffmpeg_encoder_send_cpu_frame(VideoEncoderContext *ctx, const uint8_t *bgra_data, uint32_t linesize, int64_t pts_nanos) {
    if (!ctx || !ctx->codec_ctx || !bgra_data) return -1;
    convert_source_bgra_to_sw_frame(ctx, bgra_data, linesize);
    return encode_sw_frame(ctx, pts_nanos);
}

int ffmpeg_encoder_flush(VideoEncoderContext *ctx) {
    if (!ctx || !ctx->codec_ctx) return -1;
    // Process remaining buffered frames from staging pipeline
    while (ctx->staging_count > 0 && ctx->staging_textures[0] && ctx->d3d11_context) {
        int read_idx = ctx->staging_index;
        D3D11_MAPPED_SUBRESOURCE mapped;
        HRESULT hr = ctx->d3d11_context->lpVtbl->Map(
            ctx->d3d11_context,
            (ID3D11Resource*)ctx->staging_textures[read_idx],
            0,
            D3D11_MAP_READ,
            0,
            &mapped
        );
        if (SUCCEEDED(hr)) {
            if (ctx->use_gpu_nv12_staging) {
                prepare_sw_frame_if_needed(ctx);
                const uint8_t *src_y = (const uint8_t*)mapped.pData;
                const uint8_t *src_uv = (const uint8_t*)mapped.pData + ctx->height * mapped.RowPitch;

                uint8_t *dst_y = ctx->sw_frame->data[0];
                uint8_t *dst_uv = ctx->sw_frame->data[1];
                uint32_t dst_stride_y = (uint32_t)ctx->sw_frame->linesize[0];
                uint32_t dst_stride_uv = (uint32_t)ctx->sw_frame->linesize[1];

                if (mapped.RowPitch == dst_stride_y && mapped.RowPitch == ctx->width) {
                    memcpy(dst_y, src_y, ctx->width * ctx->height);
                } else {
                    for (uint32_t r = 0; r < ctx->height; r++) {
                        memcpy(dst_y + r * dst_stride_y, src_y + r * mapped.RowPitch, ctx->width);
                    }
                }

                uint32_t uv_lines = ctx->height / 2;
                if (mapped.RowPitch == dst_stride_uv && mapped.RowPitch == ctx->width) {
                    memcpy(dst_uv, src_uv, ctx->width * uv_lines);
                } else {
                    for (uint32_t r = 0; r < uv_lines; r++) {
                        memcpy(dst_uv + r * dst_stride_uv, src_uv + r * mapped.RowPitch, ctx->width);
                    }
                }
            } else {
                convert_source_bgra_to_sw_frame(ctx, (const uint8_t*)mapped.pData, mapped.RowPitch);
            }
            ctx->d3d11_context->lpVtbl->Unmap(ctx->d3d11_context, (ID3D11Resource*)ctx->staging_textures[read_idx], 0);
            encode_sw_frame(ctx, ctx->staging_pts[read_idx]);
        }
        ctx->staging_index = (ctx->staging_index + 1) % STAGING_BUFFERS;
        ctx->staging_count--;
    }
    avcodec_send_frame(ctx->codec_ctx, NULL);
    drain_encoder_packets(ctx);
    return 0;
}

void ffmpeg_encoder_destroy(VideoEncoderContext *ctx) {
    if (!ctx) return;
    if (ctx->video_input_view) {
        ctx->video_input_view->lpVtbl->Release(ctx->video_input_view);
        ctx->video_input_view = NULL;
    }
    if (ctx->video_output_view) {
        ctx->video_output_view->lpVtbl->Release(ctx->video_output_view);
        ctx->video_output_view = NULL;
    }
    if (ctx->gpu_nv12_texture) {
        ctx->gpu_nv12_texture->lpVtbl->Release(ctx->gpu_nv12_texture);
        ctx->gpu_nv12_texture = NULL;
    }
    if (ctx->video_processor) {
        ctx->video_processor->lpVtbl->Release(ctx->video_processor);
        ctx->video_processor = NULL;
    }
    if (ctx->video_enumerator) {
        ctx->video_enumerator->lpVtbl->Release(ctx->video_enumerator);
        ctx->video_enumerator = NULL;
    }
    if (ctx->video_context) {
        ctx->video_context->lpVtbl->Release(ctx->video_context);
        ctx->video_context = NULL;
    }
    if (ctx->video_device) {
        ctx->video_device->lpVtbl->Release(ctx->video_device);
        ctx->video_device = NULL;
    }
    for (int i = 0; i < STAGING_BUFFERS; i++) {
        if (ctx->staging_textures[i]) {
            ctx->staging_textures[i]->lpVtbl->Release(ctx->staging_textures[i]);
            ctx->staging_textures[i] = NULL;
        }
    }
    if (ctx->hw_frames_ref) av_buffer_unref(&ctx->hw_frames_ref);
    if (ctx->hw_device_ref) av_buffer_unref(&ctx->hw_device_ref);
    if (ctx->codec_ctx) avcodec_free_context(&ctx->codec_ctx);
    if (ctx->packet) av_packet_free(&ctx->packet);
    if (ctx->sw_frame) av_frame_free(&ctx->sw_frame);
    if (ctx->hw_frame) av_frame_free(&ctx->hw_frame);
    if (ctx->sws_ctx) sws_freeContext(ctx->sws_ctx);
    free(ctx);
}

// ----------------------------------------------------------------------------
// Container Muxer Implementation (MKV)
// ----------------------------------------------------------------------------

MuxerContext* ffmpeg_muxer_create(
    const char *output_file,
    const VideoEncoderContext *encoder_ctx,
    const MuxerAudioStreamConfig *audio_cfg
) {
    MuxerContext *m = (MuxerContext*)calloc(1, sizeof(MuxerContext));
    if (!m) return NULL;

    InitializeSRWLock(&m->lock);
    m->video_packet = av_packet_alloc();
    if (!m->video_packet) {
        free(m);
        return NULL;
    }

    int ret = avformat_alloc_output_context2(&m->fmt_ctx, NULL, NULL, output_file);
    if (ret < 0 || !m->fmt_ctx) {
        ret = avformat_alloc_output_context2(&m->fmt_ctx, NULL, "matroska", output_file);
        if (ret < 0 || !m->fmt_ctx) {
            av_packet_free(&m->video_packet);
            free(m);
            return NULL;
        }
    }

    // Cap maximum interleaving delay to 1 second (prevents unbounded memory growth)
    m->fmt_ctx->max_interleave_delta = 1000000;

    // Configure faststart for MP4/MOV containers
    if (m->fmt_ctx->oformat && (strstr(m->fmt_ctx->oformat->name, "mp4") || strstr(m->fmt_ctx->oformat->name, "mov"))) {
        av_opt_set(m->fmt_ctx->priv_data, "movflags", "faststart", 0);
    }

    // 1. Add video stream directly from encoder context
    if (encoder_ctx && encoder_ctx->codec_ctx) {
        m->video_stream = avformat_new_stream(m->fmt_ctx, NULL);
        if (m->video_stream) {
            m->video_stream->id = 0;
            m->video_stream->time_base = encoder_ctx->codec_ctx->time_base;
            avcodec_parameters_from_context(m->video_stream->codecpar, encoder_ctx->codec_ctx);
            m->video_stream->codecpar->color_range = encoder_ctx->codec_ctx->color_range;
            m->video_stream->codecpar->color_space = encoder_ctx->codec_ctx->colorspace;
            m->video_stream->codecpar->color_primaries = encoder_ctx->codec_ctx->color_primaries;
            m->video_stream->codecpar->color_trc = encoder_ctx->codec_ctx->color_trc;
        }
        m->encoder_time_base = encoder_ctx->codec_ctx->time_base;
        if (m->encoder_time_base.num == 0 || m->encoder_time_base.den == 0) {
            m->encoder_time_base = (AVRational){1, (int)(encoder_ctx->fps ? encoder_ctx->fps : 60)};
        }
        m->fps = encoder_ctx->fps ? encoder_ctx->fps : 60;
    }

    // 2. Add audio stream (Track 1: System Audio; Track 2: Microphone if multi-track)
    if (audio_cfg && audio_cfg->channels > 0) {
        m->sample_rate = audio_cfg->sample_rate ? audio_cfg->sample_rate : 48000;
        m->channels = audio_cfg->channels ? audio_cfg->channels : 2;

        const AVCodec *audio_codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
        if (audio_codec) {
            // Track 1: System Audio
            m->audio_codec_ctx = avcodec_alloc_context3(audio_codec);
            m->audio_codec_ctx->sample_rate = (int)m->sample_rate;
            av_channel_layout_default(&m->audio_codec_ctx->ch_layout, (int)m->channels);
            m->audio_codec_ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;
            m->audio_codec_ctx->bit_rate = (int64_t)(audio_cfg->bitrate_kbps ? audio_cfg->bitrate_kbps : 192) * 1000;
            m->audio_codec_ctx->time_base = (AVRational){1, (int)m->sample_rate};
            m->audio_codec_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            if (avcodec_open2(m->audio_codec_ctx, audio_codec, NULL) >= 0) {
                m->audio_stream = avformat_new_stream(m->fmt_ctx, NULL);
                m->audio_stream->id = 1;
                m->audio_stream->time_base = m->audio_codec_ctx->time_base;
                avcodec_parameters_from_context(m->audio_stream->codecpar, m->audio_codec_ctx);
                av_dict_set(&m->audio_stream->metadata, "title", "System Audio", 0);

                m->audio_frame = av_frame_alloc();
                m->audio_frame->nb_samples = m->audio_codec_ctx->frame_size ? m->audio_codec_ctx->frame_size : 1024;
                m->audio_frame->format = m->audio_codec_ctx->sample_fmt;
                av_channel_layout_copy(&m->audio_frame->ch_layout, &m->audio_codec_ctx->ch_layout);
                av_frame_get_buffer(m->audio_frame, 0);

                m->audio_packet = av_packet_alloc();
                m->audio_fifo = av_audio_fifo_alloc(
                    m->audio_codec_ctx->sample_fmt,
                    m->audio_codec_ctx->ch_layout.nb_channels,
                    (m->audio_codec_ctx->frame_size ? m->audio_codec_ctx->frame_size : 1024) * 4
                );
            }

            // Track 2: Microphone (if multi-track requested)
            if (audio_cfg->enable_multi_track) {
                m->audio_codec_ctx_mic = avcodec_alloc_context3(audio_codec);
                m->audio_codec_ctx_mic->sample_rate = (int)m->sample_rate;
                av_channel_layout_default(&m->audio_codec_ctx_mic->ch_layout, (int)m->channels);
                m->audio_codec_ctx_mic->sample_fmt = AV_SAMPLE_FMT_FLTP;
                m->audio_codec_ctx_mic->bit_rate = (int64_t)(audio_cfg->bitrate_kbps ? audio_cfg->bitrate_kbps : 192) * 1000;
                m->audio_codec_ctx_mic->time_base = (AVRational){1, (int)m->sample_rate};
                m->audio_codec_ctx_mic->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

                if (avcodec_open2(m->audio_codec_ctx_mic, audio_codec, NULL) >= 0) {
                    m->audio_stream_mic = avformat_new_stream(m->fmt_ctx, NULL);
                    m->audio_stream_mic->id = 2;
                    m->audio_stream_mic->time_base = m->audio_codec_ctx_mic->time_base;
                    avcodec_parameters_from_context(m->audio_stream_mic->codecpar, m->audio_codec_ctx_mic);
                    av_dict_set(&m->audio_stream_mic->metadata, "title", "Microphone", 0);

                    m->audio_frame_mic = av_frame_alloc();
                    m->audio_frame_mic->nb_samples = m->audio_codec_ctx_mic->frame_size ? m->audio_codec_ctx_mic->frame_size : 1024;
                    m->audio_frame_mic->format = m->audio_codec_ctx_mic->sample_fmt;
                    av_channel_layout_copy(&m->audio_frame_mic->ch_layout, &m->audio_codec_ctx_mic->ch_layout);
                    av_frame_get_buffer(m->audio_frame_mic, 0);

                    m->audio_packet_mic = av_packet_alloc();
                    m->audio_fifo_mic = av_audio_fifo_alloc(
                        m->audio_codec_ctx_mic->sample_fmt,
                        m->audio_codec_ctx_mic->ch_layout.nb_channels,
                        (m->audio_codec_ctx_mic->frame_size ? m->audio_codec_ctx_mic->frame_size : 1024) * 4
                    );
                }
            }
        }
    }

    if (!(m->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        AVDictionary *io_opts = NULL;
        // 2026 Next-Gen High-Throughput NVMe Direct Buffer (256 KB block write alignment)
        av_dict_set(&io_opts, "buffer_size", "262144", 0);
        ret = avio_open2(&m->fmt_ctx->pb, output_file, AVIO_FLAG_WRITE, NULL, &io_opts);
        av_dict_free(&io_opts);
        if (ret < 0) {
            char errbuf[128] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            fprintf(stderr, "[FFmpeg] Failed to open output file: %s (ret=%d, %s)\n", output_file, ret, errbuf);
            avformat_free_context(m->fmt_ctx);
            if (m->video_packet) av_packet_free(&m->video_packet);
            free(m);
            return NULL;
        }
    }

    ret = avformat_write_header(m->fmt_ctx, NULL);
    if (ret < 0) {
        char errbuf[128] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "[FFmpeg] Failed to write header to: %s (ret=%d, %s)\n", output_file, ret, errbuf);
        avio_closep(&m->fmt_ctx->pb);
        avformat_free_context(m->fmt_ctx);
        if (m->video_packet) av_packet_free(&m->video_packet);
        free(m);
        return NULL;
    }

    printf("[FFmpeg] Muxer created: %s (%s), encoder_tb=%d/%d, stream_tb=%d/%d\n",
           output_file, m->fmt_ctx->oformat ? m->fmt_ctx->oformat->name : "unknown",
           m->encoder_time_base.num, m->encoder_time_base.den,
           m->video_stream ? m->video_stream->time_base.num : 0,
           m->video_stream ? m->video_stream->time_base.den : 0);
    return m;
}

int ffmpeg_muxer_write_video_packet(MuxerContext *m, const EncodedPacket *pkt) {
    if (!m || !m->fmt_ctx || !m->video_stream || !m->video_packet || !pkt || !pkt->data || pkt->size == 0) return -1;

    AcquireSRWLockExclusive(&m->lock);

    av_packet_unref(m->video_packet);
    int ret = av_new_packet(m->video_packet, (int)pkt->size);
    if (ret < 0) {
        ReleaseSRWLockExclusive(&m->lock);
        return ret;
    }

    memcpy(m->video_packet->data, pkt->data, pkt->size);
    m->video_packet->stream_index = m->video_stream->index;
    m->video_packet->pts = pkt->pts;
    m->video_packet->dts = pkt->dts;
    if (pkt->is_keyframe) m->video_packet->flags |= AV_PKT_FLAG_KEY;

    if (m->encoder_time_base.num > 0 && m->encoder_time_base.den > 0 &&
        m->video_stream->time_base.num > 0 && m->video_stream->time_base.den > 0) {
        av_packet_rescale_ts(m->video_packet, m->encoder_time_base, m->video_stream->time_base);
    }

    ret = av_interleaved_write_frame(m->fmt_ctx, m->video_packet);
    av_packet_unref(m->video_packet);

    ReleaseSRWLockExclusive(&m->lock);
    return ret;
}

int ffmpeg_muxer_write_audio_samples_track(MuxerContext *m, int track_index, const float *pcm_samples, uint32_t sample_count, int64_t pts_nanos) {
    if (!m || !pcm_samples || sample_count == 0) return -1;
    (void)pts_nanos;

    AcquireSRWLockExclusive(&m->lock);

    AVCodecContext *codec_ctx = (track_index == 1 && m->audio_codec_ctx_mic) ? m->audio_codec_ctx_mic : m->audio_codec_ctx;
    AVStream *stream = (track_index == 1 && m->audio_stream_mic) ? m->audio_stream_mic : m->audio_stream;
    AVFrame *frame = (track_index == 1 && m->audio_frame_mic) ? m->audio_frame_mic : m->audio_frame;
    AVPacket *pkt = (track_index == 1 && m->audio_packet_mic) ? m->audio_packet_mic : m->audio_packet;
    AVAudioFifo *fifo = (track_index == 1 && m->audio_fifo_mic) ? m->audio_fifo_mic : m->audio_fifo;
    int64_t *next_pts = (track_index == 1 && m->audio_stream_mic) ? &m->next_audio_pts_mic : &m->next_audio_pts;

    if (!codec_ctx || !stream || !frame || !pkt || !fifo) {
        ReleaseSRWLockExclusive(&m->lock);
        return -1;
    }

    // Deinterleave float PCM into planar channel buffers for the FIFO
    float left_stack[4096];
    float right_stack[4096];
    float *planar_ptrs[2] = { left_stack, right_stack };
    float *heap_alloc = NULL;

    if (sample_count > 4096) {
        heap_alloc = (float*)malloc(sample_count * 2 * sizeof(float));
        if (heap_alloc) {
            planar_ptrs[0] = heap_alloc;
            planar_ptrs[1] = heap_alloc + sample_count;
        } else {
            ReleaseSRWLockExclusive(&m->lock);
            return -1;
        }
    }

    for (uint32_t i = 0; i < sample_count; i++) {
        planar_ptrs[0][i] = pcm_samples[i * m->channels];
        planar_ptrs[1][i] = (m->channels > 1) ? pcm_samples[i * m->channels + 1] : planar_ptrs[0][i];
    }

    av_audio_fifo_write(fifo, (void**)planar_ptrs, (int)sample_count);
    if (heap_alloc) free(heap_alloc);

    int frame_size = codec_ctx->frame_size ? codec_ctx->frame_size : 1024;
    while (av_audio_fifo_size(fifo) >= frame_size) {
        av_audio_fifo_read(fifo, (void**)frame->data, frame_size);
        frame->nb_samples = frame_size;
        frame->pts = *next_pts;
        *next_pts += frame_size;

        int ret = avcodec_send_frame(codec_ctx, frame);
        if (ret < 0) break;

        while (avcodec_receive_packet(codec_ctx, pkt) == 0) {
            pkt->stream_index = stream->index;
            av_packet_rescale_ts(pkt, codec_ctx->time_base, stream->time_base);
            av_interleaved_write_frame(m->fmt_ctx, pkt);
            av_packet_unref(pkt);
        }
    }

    ReleaseSRWLockExclusive(&m->lock);
    return 0;
}

int ffmpeg_muxer_write_audio_samples(MuxerContext *m, const float *pcm_samples, uint32_t sample_count, int64_t pts_nanos) {
    return ffmpeg_muxer_write_audio_samples_track(m, 0, pcm_samples, sample_count, pts_nanos);
}

int ffmpeg_muxer_close(MuxerContext *m) {
    if (!m) return -1;

    AcquireSRWLockExclusive(&m->lock);

    // Flush remaining samples in Track 1 FIFO with silence padding
    if (m->audio_fifo && m->audio_codec_ctx && m->audio_stream && m->audio_frame && m->audio_packet) {
        int rem = av_audio_fifo_size(m->audio_fifo);
        if (rem > 0) {
            int frame_size = m->audio_codec_ctx->frame_size ? m->audio_codec_ctx->frame_size : 1024;
            av_audio_fifo_read(m->audio_fifo, (void**)m->audio_frame->data, rem);
            for (int ch = 0; ch < m->audio_codec_ctx->ch_layout.nb_channels; ch++) {
                memset((float*)m->audio_frame->data[ch] + rem, 0, (size_t)(frame_size - rem) * sizeof(float));
            }
            m->audio_frame->nb_samples = frame_size;
            m->audio_frame->pts = m->next_audio_pts;
            m->next_audio_pts += frame_size;
            if (avcodec_send_frame(m->audio_codec_ctx, m->audio_frame) >= 0) {
                while (avcodec_receive_packet(m->audio_codec_ctx, m->audio_packet) == 0) {
                    m->audio_packet->stream_index = m->audio_stream->index;
                    av_packet_rescale_ts(m->audio_packet, m->audio_codec_ctx->time_base, m->audio_stream->time_base);
                    av_interleaved_write_frame(m->fmt_ctx, m->audio_packet);
                    av_packet_unref(m->audio_packet);
                }
            }
        }
    }

    if (m->audio_codec_ctx && m->audio_stream && m->audio_packet) {
        avcodec_send_frame(m->audio_codec_ctx, NULL);
        while (avcodec_receive_packet(m->audio_codec_ctx, m->audio_packet) == 0) {
            m->audio_packet->stream_index = m->audio_stream->index;
            av_packet_rescale_ts(m->audio_packet, m->audio_codec_ctx->time_base, m->audio_stream->time_base);
            av_interleaved_write_frame(m->fmt_ctx, m->audio_packet);
            av_packet_unref(m->audio_packet);
        }
    }

    // Flush remaining samples in Track 2 (mic) FIFO with silence padding
    if (m->audio_fifo_mic && m->audio_codec_ctx_mic && m->audio_stream_mic && m->audio_frame_mic && m->audio_packet_mic) {
        int rem = av_audio_fifo_size(m->audio_fifo_mic);
        if (rem > 0) {
            int frame_size = m->audio_codec_ctx_mic->frame_size ? m->audio_codec_ctx_mic->frame_size : 1024;
            av_audio_fifo_read(m->audio_fifo_mic, (void**)m->audio_frame_mic->data, rem);
            for (int ch = 0; ch < m->audio_codec_ctx_mic->ch_layout.nb_channels; ch++) {
                memset((float*)m->audio_frame_mic->data[ch] + rem, 0, (size_t)(frame_size - rem) * sizeof(float));
            }
            m->audio_frame_mic->nb_samples = frame_size;
            m->audio_frame_mic->pts = m->next_audio_pts_mic;
            m->next_audio_pts_mic += frame_size;
            if (avcodec_send_frame(m->audio_codec_ctx_mic, m->audio_frame_mic) >= 0) {
                while (avcodec_receive_packet(m->audio_codec_ctx_mic, m->audio_packet_mic) == 0) {
                    m->audio_packet_mic->stream_index = m->audio_stream_mic->index;
                    av_packet_rescale_ts(m->audio_packet_mic, m->audio_codec_ctx_mic->time_base, m->audio_stream_mic->time_base);
                    av_interleaved_write_frame(m->fmt_ctx, m->audio_packet_mic);
                    av_packet_unref(m->audio_packet_mic);
                }
            }
        }
    }

    if (m->audio_codec_ctx_mic && m->audio_stream_mic && m->audio_packet_mic) {
        avcodec_send_frame(m->audio_codec_ctx_mic, NULL);
        while (avcodec_receive_packet(m->audio_codec_ctx_mic, m->audio_packet_mic) == 0) {
            m->audio_packet_mic->stream_index = m->audio_stream_mic->index;
            av_packet_rescale_ts(m->audio_packet_mic, m->audio_codec_ctx_mic->time_base, m->audio_stream_mic->time_base);
            av_interleaved_write_frame(m->fmt_ctx, m->audio_packet_mic);
            av_packet_unref(m->audio_packet_mic);
        }
    }

    if (m->fmt_ctx) {
        av_write_trailer(m->fmt_ctx);
        if (!(m->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&m->fmt_ctx->pb);
        }
        avformat_free_context(m->fmt_ctx);
        m->fmt_ctx = NULL;
    }

    if (m->video_packet) av_packet_free(&m->video_packet);
    if (m->audio_fifo) av_audio_fifo_free(m->audio_fifo);
    if (m->audio_fifo_mic) av_audio_fifo_free(m->audio_fifo_mic);
    if (m->audio_codec_ctx) avcodec_free_context(&m->audio_codec_ctx);
    if (m->audio_frame) av_frame_free(&m->audio_frame);
    if (m->audio_packet) av_packet_free(&m->audio_packet);

    if (m->audio_codec_ctx_mic) avcodec_free_context(&m->audio_codec_ctx_mic);
    if (m->audio_frame_mic) av_frame_free(&m->audio_frame_mic);
    if (m->audio_packet_mic) av_packet_free(&m->audio_packet_mic);

    if (m->swr_ctx) swr_free(&m->swr_ctx);

    ReleaseSRWLockExclusive(&m->lock);
    free(m);
    return 0;
}
