const std = @import("std");
const types = @import("types.zig");
const stats = @import("stats.zig");
const command = @import("command.zig");
const ring_buffer = @import("ring_buffer.zig");
const clock = @import("clock.zig");
const config = @import("../config/config.zig");
const audio_dsp = @import("audio_dsp.zig");

const capture_backend = @import("../platform/windows/capture_backend.zig");
const wasapi = @import("../platform/windows/wasapi_audio.zig");
const encoder = @import("../encoders/ffmpeg_encoder.zig");
const muxer = @import("../encoders/muxer.zig");
const wav_writer = @import("../encoders/wav_writer.zig");
const app_window = @import("../ui/app_window.zig");

const log = std.log.scoped(.pipeline);
const THREAD_PRIORITY_HIGHEST = 2;
const THREAD_PRIORITY_ABOVE_NORMAL = 1;
const THREAD_PRIORITY_NORMAL = 0;
const THREAD_PRIORITY_BELOW_NORMAL = -1;
const THREAD_PRIORITY_LOWEST = -2;

extern "kernel32" fn SetThreadPriority(hThread: ?*anyopaque, nPriority: i32) callconv(.winapi) i32;
extern "kernel32" fn GetCurrentThread() callconv(.winapi) ?*anyopaque;
extern "user32" fn OpenInputDesktop(dwFlags: u32, fInherit: i32, dwDesiredAccess: u32) callconv(.winapi) ?*anyopaque;
extern "user32" fn SetThreadDesktop(hDesktop: ?*anyopaque) callconv(.winapi) i32;
extern "user32" fn CloseDesktop(hDesktop: ?*anyopaque) callconv(.winapi) i32;

pub const Pipeline = struct {
    pub const FrameRing = ring_buffer.SpscRingBuffer(types.VideoFrame, 16);

    allocator: std.mem.Allocator,
    cfg: config.Config,
    stats: *stats.AtomicStats,
    cmd_queue: *command.CommandQueue,
    master_start_nanos: u64 = 0,
    is_running: std.atomic.Value(bool) = .init(false),
    is_paused: std.atomic.Value(bool) = .init(false),
    capture_active: std.atomic.Value(bool) = .init(false),
    encoder_active: std.atomic.Value(bool) = .init(false),
    audio_active: std.atomic.Value(bool) = .init(false),

    // Threads
    capture_thread: ?std.Thread = null,
    encoder_thread: ?std.Thread = null,
    audio_thread: ?std.Thread = null,

    // Buffers
    frame_ring: FrameRing = .init(),

    // Pipeline engine objects
    cap: ?capture_backend.CaptureBackend = null,
    enc: ?encoder.FfmpegEncoder = null,
    mux: ?muxer.Muxer = null,
    wav_out: ?wav_writer.WavWriter = null,

    output_path: [260]u8 = undefined,
    output_path_len: usize = 0,

    pub fn init(allocator: std.mem.Allocator, cfg: config.Config, global_stats: *stats.AtomicStats, cmd_queue: *command.CommandQueue) Pipeline {
        return .{
            .allocator = allocator,
            .cfg = cfg,
            .stats = global_stats,
            .cmd_queue = cmd_queue,
        };
    }

    pub fn start(self: *Pipeline, target: types.CaptureTarget, output_file: []const u8) !void {
        if (self.is_running.load(.monotonic)) return;

        const copy_len = @min(output_file.len, self.output_path.len - 1);
        @memcpy(self.output_path[0..copy_len], output_file[0..copy_len]);
        self.output_path[copy_len] = 0;
        self.output_path_len = copy_len;

        self.stats.reset();
        self.stats.state.store(.starting, .release);
        errdefer self.stats.state.store(.idle, .release);

        self.frame_ring.reset();

        // 1. Initialize Capture Backend (DXGI for screen, WGC for window/monitor)
        var capture = try capture_backend.CaptureBackend.init(target);
        errdefer capture.deinit();

        // 2. Determine output dimensions (scaled if requested, e.g. 720p for low-end PCs)
        var enc_width = capture.width;
        var enc_height = capture.height;
        if (self.cfg.video.resolution_width > 0 and self.cfg.video.resolution_height > 0) {
            enc_width = self.cfg.video.resolution_width;
            enc_height = self.cfg.video.resolution_height;
        }

        // Initialize FFmpeg Video Encoder using the EXACT same D3D11 device and context
        var video_enc = try encoder.FfmpegEncoder.init(
            self.cfg,
            enc_width,
            enc_height,
            capture.width,
            capture.height,
            capture.getD3DDevice(),
            capture.getD3DContext(),
            onPacketEncoded,
            self,
        );
        errdefer video_enc.deinit();

        const out_path_z: [*:0]const u8 = @ptrCast(&self.output_path);
        const audio_source_count: u32 =
            @as(u32, @intFromBool(self.cfg.audio.system_audio_enabled)) +
            @as(u32, @intFromBool(self.cfg.audio.system_audio2_enabled)) +
            @as(u32, @intFromBool(self.cfg.audio.mic_enabled)) +
            @as(u32, @intFromBool(self.cfg.audio.mic2_enabled));
        const has_audio = audio_source_count > 0;
        const individual_track_count: u32 = if (self.cfg.audio.separate_tracks) audio_source_count else 0;
        var mkv_mux = try muxer.Muxer.init(
            out_path_z,
            video_enc.handle,
            has_audio,
            individual_track_count,
            self.cfg.audio.audio_bitrate_kbps,
        );
        errdefer mkv_mux.deinit();

        var wav_out_inst: ?wav_writer.WavWriter = null;
        if (self.cfg.audio.save_separate_audio_file) {
            var wav_path_buf: [512]u8 = undefined;
            const stem = if (std.mem.lastIndexOfScalar(u8, self.output_path[0..copy_len], '.')) |idx|
                self.output_path[0..idx]
            else
                self.output_path[0..copy_len];
            if (std.fmt.bufPrint(&wav_path_buf, "{s}.wav\x00", .{stem})) |wp| {
                const wp_z: [*:0]const u8 = @ptrCast(wp.ptr);
                if (wav_writer.WavWriter.create(wp_z, 48000, 2)) |w| {
                    wav_out_inst = w;
                    log.info("Initialized standalone WAV file: {s}", .{wp[0 .. wp.len - 1]});
                } else |err| {
                    log.warn("Failed to create standalone WAV audio file: {s}", .{@errorName(err)});
                }
            } else |_| {}
        }

        self.cap = capture;
        self.enc = video_enc;
        self.mux = mkv_mux;
        self.wav_out = wav_out_inst;

        clock.enableHighResolutionTimer();
        self.master_start_nanos = clock.MonotonicClock.nowNanos();

        self.is_running.store(true, .release);
        self.is_paused.store(false, .release);
        self.capture_active.store(true, .release);
        self.encoder_active.store(true, .release);
        self.audio_active.store(true, .release);

        // Spawn Pipeline Threads
        self.capture_thread = try std.Thread.spawn(.{}, captureWorker, .{self});
        self.encoder_thread = try std.Thread.spawn(.{}, encoderWorker, .{self});
        self.audio_thread = try std.Thread.spawn(.{}, audioWorker, .{self});

        self.stats.state.store(.recording, .release);
        log.info("Pipeline started successfully: recording to {s}", .{self.output_path[0..copy_len]});
    }

    pub fn stop(self: *Pipeline) void {
        if (!self.is_running.load(.monotonic)) return;
        self.stats.state.store(.stopping, .release);

        // Waterfall Phase 1: Stop capture & audio
        self.capture_active.store(false, .release);
        self.audio_active.store(false, .release);
        if (self.capture_thread) |t| t.join();
        self.capture_thread = null;
        if (self.audio_thread) |t| t.join();
        self.audio_thread = null;

        // Waterfall Phase 2: Stop encoder & wait for all remaining frames + flush
        self.encoder_active.store(false, .release);
        if (self.encoder_thread) |t| t.join();
        self.encoder_thread = null;

        self.is_running.store(false, .release);
        clock.disableHighResolutionTimer();

        // Waterfall Phase 3: Finalize container & standalone audio
        if (self.wav_out) |*w| w.close();
        self.wav_out = null;

        if (self.mux) |*m| m.deinit();
        if (self.enc) |*e| e.deinit();
        if (self.cap) |*c| c.deinit();

        self.mux = null;
        self.enc = null;
        self.cap = null;

        self.stats.state.store(.idle, .release);
        log.info("Pipeline stopped and finalized.", .{});
    }

    // Callback invoked when FFmpeg produces an encoded video packet
    fn onPacketEncoded(user_data: ?*anyopaque, packet: *const encoder.EncodedPacket) callconv(.c) void {
        const self: *Pipeline = @ptrCast(@alignCast(user_data.?));
        if (self.mux) |*m| {
            m.writeVideoPacket(packet) catch |err| {
                log.warn("Muxer write video failed: {s}", .{@errorName(err)});
            };
            _ = self.stats.file_bytes.fetchAdd(packet.size, .monotonic);
        }
        _ = self.stats.encoded_frames.fetchAdd(1, .monotonic);
    }

    // 1. Capture Thread: DXGI Desktop Duplication (NORMAL priority to not starve games)
    fn captureWorker(self: *Pipeline) void {
        _ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        const input_desk = OpenInputDesktop(0, 0, 0x01FF);
        if (input_desk) |desk| {
            _ = SetThreadDesktop(desk);
        }
        defer if (input_desk) |desk| {
            _ = CloseDesktop(desk);
        };
        const frame_interval_ns = std.time.ns_per_s / self.cfg.video.fps;
        var next_target_time = self.master_start_nanos + frame_interval_ns;
        var frame_idx: u64 = 0;
        var last_valid_frame: ?types.VideoFrame = null;

        while (self.capture_active.load(.acquire)) {
            if (self.is_paused.load(.monotonic)) {
                std.Thread.yield() catch {};
                continue;
            }

            // High-precision hybrid sleep/yield to hit exact microsecond target
            clock.sleepPreciseUntil(next_target_time);

            const now_ns = clock.MonotonicClock.nowNanos();
            const real_pts_nanos = if (now_ns > self.master_start_nanos) now_ns - self.master_start_nanos else 0;
            self.stats.duration_nanos.store(real_pts_nanos, .monotonic);

            // Wait up to 1ms for GPU flip (zero delay to keep game smooth)
            const maybe_frame = if (self.cap) |*c| c.acquireFrame(1, real_pts_nanos) catch |err| blk: {
                log.warn("Capture frame error: {s}", .{@errorName(err)});
                break :blk null;
            } else null;

            if (maybe_frame) |frame| {
                last_valid_frame = frame;
                _ = self.stats.captured_frames.fetchAdd(1, .monotonic);

                if (frame_idx % 2 == 0 and !self.cfg.video.low_end_mode and app_window.imgui_is_preview_enabled()) {
                    app_window.imgui_update_preview_d3d11(frame.gpu_texture_ptr, @intCast(frame.width), @intCast(frame.height));
                }

                // Real-time low-RAM live pacing: cap queue depth at 4 frames
                while (self.frame_ring.available() >= 4) {
                    _ = self.frame_ring.tryPop();
                    _ = self.stats.dropped_frames.fetchAdd(1, .monotonic);
                }

                if (!self.frame_ring.tryPush(frame)) {
                    _ = self.stats.dropped_frames.fetchAdd(1, .monotonic);
                }
                frame_idx += 1;
            } else if (last_valid_frame) |prev| {
                // Zero-cost CFR pacing: emit lightweight repeat token (0 GPU/DMA copy) so 30/60 FPS stays 100% locked
                if (self.frame_ring.available() == 0) {
                    var repeat_frame = prev;
                    repeat_frame.pts_nanos = real_pts_nanos;
                    repeat_frame.frame_index = frame_idx;
                    repeat_frame.is_repeat = true;

                    last_valid_frame.?.pts_nanos = real_pts_nanos;
                    _ = self.stats.captured_frames.fetchAdd(1, .monotonic);
                    if (!self.frame_ring.tryPush(repeat_frame)) {
                        _ = self.stats.dropped_frames.fetchAdd(1, .monotonic);
                    }
                    frame_idx += 1;
                }
            }

            // Advance target time to next tick
            const current_now = clock.MonotonicClock.nowNanos();
            if (current_now >= next_target_time + (frame_interval_ns * 2)) {
                next_target_time = current_now + frame_interval_ns;
            } else {
                next_target_time += frame_interval_ns;
            }
        }
    }

    // 2. Encoder Thread: FFmpeg NVENC/QSV/x264 (NORMAL priority)
    fn encoderWorker(self: *Pipeline) void {
        _ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);

        while (self.encoder_active.load(.acquire) or self.frame_ring.available() > 0) {

            if (self.frame_ring.tryPop()) |frame| {
                if (frame.is_repeat) {
                    if (self.enc) |*e| {
                        e.repeatLastFrame(frame.pts_nanos) catch {};
                    }
                } else if (frame.gpu_texture_ptr) |tex| {
                    if (self.enc) |*e| {
                        e.sendD3D11Texture(tex, frame.pts_nanos) catch |err| {
                            log.warn("Send D3D11 texture failed: {s}", .{@errorName(err)});
                        };
                    }
                }
            } else {
                clock.sleepMs(1);
            }
        }

        if (self.enc) |*e| {
            e.flush();
        }
    }

    const AudioFifo = struct {
        buffer: [16384]f32 = undefined,
        head: usize = 0,
        tail: usize = 0,
        count: usize = 0,

        fn write(self: *AudioFifo, samples: []const f32) void {
            const even_len = samples.len & ~@as(usize, 1);
            for (samples[0..even_len]) |s| {
                if (self.count >= self.buffer.len) break;
                self.buffer[self.tail] = s;
                self.tail = (self.tail + 1) % self.buffer.len;
                self.count += 1;
            }
        }

        fn read(self: *AudioFifo, out: []f32) usize {
            const even_out = out.len & ~@as(usize, 1);
            const to_read = @min(even_out, self.count) & ~@as(usize, 1);
            for (0..to_read) |i| {
                out[i] = self.buffer[self.head];
                self.head = (self.head + 1) % self.buffer.len;
            }
            self.count -= to_read;
            return to_read;
        }

        fn padZeros(self: *AudioFifo, num_zeros: usize) void {
            const even_zeros = num_zeros & ~@as(usize, 1);
            for (0..even_zeros) |_| {
                if (self.count >= self.buffer.len) break;
                self.buffer[self.tail] = 0.0;
                self.tail = (self.tail + 1) % self.buffer.len;
                self.count += 1;
            }
        }
    };

    const MAX_AUDIO_SOURCES = 4;
    const SOURCE_MIX_TRACK = 0;
    const MIX_SAMPLE_RATE: u32 = 48000;

    const AudioSource = struct {
        is_mic: bool = false,
        enabled: bool = false,
        device_index: u32 = 0,
        volume: f32 = 1.0,
        track: i32 = -1,
        peak: f32 = 0.0,
        capture: ?wasapi.WasapiCapture = null,
        fifo: AudioFifo = .{},
        dsp: audio_dsp.VoiceChain = audio_dsp.VoiceChain.init(48000.0),
        buffer: [8192]f32 = undefined,
        resample_buffer: [8192]f32 = undefined,
    };

    fn resampleLinear(
        src: []const f32,
        src_rate: u32,
        dst: []f32,
        dst_rate: u32,
        dst_channels: u32,
    ) u32 {
        if (src_rate == dst_rate or src.len < 2) {
            const n = @min(src.len, dst.len);
            @memcpy(dst[0..n], src[0..n]);
            return @intCast(n);
        }
        const ch = if (dst_channels == 0) 2 else dst_channels;
        const src_frames = src.len / ch;
        const ratio = @as(f64, @floatFromInt(src_rate)) / @as(f64, @floatFromInt(dst_rate));
        var dst_frames: usize = 0;
        while (dst_frames < dst.len / ch) : (dst_frames += 1) {
            const src_pos = @as(f64, @floatFromInt(dst_frames)) * ratio;
            const pos_int: usize = @intFromFloat(src_pos);
            if (pos_int + 1 >= src_frames) break;
            const frac: f32 = @floatCast(src_pos - @as(f64, @floatFromInt(pos_int)));
            for (0..ch) |c| {
                const s0 = src[pos_int * ch + c];
                const s1 = src[(pos_int + 1) * ch + c];
                dst[dst_frames * ch + c] = s0 + (s1 - s0) * frac;
            }
        }
        return @intCast(dst_frames * ch);
    }

    fn audioWorker(self: *Pipeline) void {
        _ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        var sources: [MAX_AUDIO_SOURCES]AudioSource = .{
            .{
                .enabled = self.cfg.audio.system_audio_enabled,
                .device_index = self.cfg.audio.selected_audio_output,
                .volume = self.cfg.audio.system_audio_volume,
            },
            .{
                .is_mic = false,
                .enabled = self.cfg.audio.system_audio2_enabled,
                .device_index = self.cfg.audio.selected_audio_output2,
                .volume = self.cfg.audio.system_audio2_volume,
            },
            .{
                .is_mic = true,
                .enabled = self.cfg.audio.mic_enabled,
                .device_index = self.cfg.audio.selected_audio_input,
                .volume = self.cfg.audio.mic_volume,
            },
            .{
                .is_mic = true,
                .enabled = self.cfg.audio.mic2_enabled,
                .device_index = self.cfg.audio.selected_audio_input2,
                .volume = self.cfg.audio.mic2_volume,
            },
        };

        var next_track: i32 = 1;
        for (&sources) |*src| {
            if (src.enabled and self.cfg.audio.separate_tracks) {
                src.track = next_track;
                next_track += 1;
            }
        }

        for (&sources) |*src| {
            if (!src.enabled) continue;
            const cap = if (src.is_mic)
                wasapi.WasapiCapture.initCapture(src.device_index)
            else
                wasapi.WasapiCapture.initLoopback(src.device_index);
            if (cap) |c| {
                src.capture = c;
                if (src.is_mic) {
                    src.dsp = audio_dsp.VoiceChain.init(@floatFromInt(c.sample_rate));
                    src.dsp.configureMicChain(
                        @floatFromInt(c.sample_rate),
                        self.cfg.audio.mic_noise_suppression,
                        self.cfg.audio.mic_ns_strength,
                        self.cfg.audio.mic_compressor,
                        self.cfg.audio.mic_comp_amount,
                        self.cfg.audio.mic_gain_db,
                        src.volume,
                    );
                }
            } else |err| {
                src.enabled = false;
                src.track = -1;
                log.warn("WASAPI {s} source {d} not available: {s}", .{
                    if (src.is_mic) "mic" else "loopback",
                    src.device_index,
                    @errorName(err),
                });
            }
        }
        defer for (&sources) |*src| {
            if (src.capture) |*cap| cap.deinit();
        };

        var total_frames_written: u64 = 0;
        const sample_rate: u64 = MIX_SAMPLE_RATE;

        var chunk_src: [MAX_AUDIO_SOURCES][2048]f32 = undefined;
        var chunk_mix: [2048]f32 = undefined;

        while (self.audio_active.load(.acquire)) {
            var did_read = false;
            const now_ns = clock.MonotonicClock.nowNanos();
            const elapsed_ns = if (now_ns > self.master_start_nanos) now_ns - self.master_start_nanos else 0;
            const expected_frames = (elapsed_ns * sample_rate) / std.time.ns_per_s;
            const max_allowed_frames = expected_frames + 4800;

            sources[0].volume = self.cfg.audio.system_audio_volume;
            sources[1].volume = self.cfg.audio.system_audio2_volume;
            sources[2].volume = self.cfg.audio.mic_volume;
            sources[3].volume = self.cfg.audio.mic2_volume;

            var sys_peak: f32 = 0.0;
            var mic_peak: f32 = 0.0;
            for (&sources) |*src| {
                if (!src.enabled) continue;
                if (src.capture == null) continue;
                const capture = &src.capture.?;
                const raw_count = capture.readSamples(&src.buffer, &src.peak);
                if (raw_count == 0) continue;
                did_read = true;

                if (src.is_mic) {
                    src.dsp.configureMicChain(
                        @floatFromInt(capture.sample_rate),
                        self.cfg.audio.mic_noise_suppression,
                        self.cfg.audio.mic_ns_strength,
                        self.cfg.audio.mic_compressor,
                        self.cfg.audio.mic_comp_amount,
                        self.cfg.audio.mic_gain_db,
                        src.volume,
                    );
                    src.dsp.processBuffer(src.buffer[0..raw_count], 2);
                } else {
                    const vol = src.volume;
                    if (vol != 1.0) {
                        for (src.buffer[0..raw_count]) |*s| {
                            s.* = std.math.clamp(s.* * vol, -1.0, 1.0);
                        }
                    }
                }

                var out_samples: []f32 = undefined;
                if (capture.sample_rate != MIX_SAMPLE_RATE) {
                    const converted = resampleLinear(
                        src.buffer[0..raw_count],
                        capture.sample_rate,
                        &src.resample_buffer,
                        MIX_SAMPLE_RATE,
                        2,
                    );
                    out_samples = src.resample_buffer[0..converted];
                } else {
                    out_samples = src.buffer[0..raw_count];
                }

                src.fifo.write(out_samples);

                if (src.is_mic) {
                    if (src.peak > mic_peak) mic_peak = src.peak;
                } else {
                    if (src.peak > sys_peak) sys_peak = src.peak;
                }
            }

            const scaled_sys: u32 = @intFromFloat(@min(sys_peak, 1.0) * 10000.0);
            self.stats.audio_peak_system.store(scaled_sys, .monotonic);
            const scaled_mic: u32 = @intFromFloat(@min(mic_peak, 1.0) * 10000.0);
            self.stats.audio_peak_mic.store(scaled_mic, .monotonic);

            var max_count: usize = 0;
            for (&sources) |*src| {
                if (!src.enabled) continue;
                if (src.fifo.count > max_count) max_count = src.fifo.count;
            }
            for (&sources) |*src| {
                if (!src.enabled) continue;
                if (max_count > src.fifo.count + 1920) {
                    src.fifo.padZeros((max_count - src.fifo.count) & ~@as(usize, 1));
                }
            }

            var min_count: usize = if (max_count > 0) max_count else 0;
            for (&sources) |*src| {
                if (!src.enabled) continue;
                if (src.fifo.count < min_count) min_count = src.fifo.count;
            }

            while (min_count >= 2 and total_frames_written < max_allowed_frames) {
                const pair_count = @min(min_count, 2048) & ~@as(usize, 1);
                if (pair_count == 0) break;

                @memset(chunk_mix[0..pair_count], 0.0);
                for (&sources, 0..) |*src, src_idx| {
                    if (!src.enabled) continue;
                    _ = src.fifo.read(chunk_src[src_idx][0..pair_count]);
                    if (self.mux) |*m| {
                        if (src.track >= 0) {
                            m.writeAudioSamplesTrack(@intCast(src.track), chunk_src[src_idx][0..pair_count], 0) catch {};
                        }
                    }
                    for (0..pair_count) |i| {
                        chunk_mix[i] += chunk_src[src_idx][i];
                    }
                }
                for (chunk_mix[0..pair_count]) |*s_ptr| {
                    s_ptr.* = std.math.clamp(s_ptr.*, -1.0, 1.0);
                }

                if (self.mux) |*m| {
                    m.writeAudioSamplesTrack(SOURCE_MIX_TRACK, chunk_mix[0..pair_count], 0) catch {};
                }
                if (self.wav_out) |*w| w.writeSamplesFloat(chunk_mix[0..pair_count]);
                total_frames_written += pair_count / 2;
                did_read = true;

                min_count = 2048;
                for (&sources) |*src| {
                    if (!src.enabled) continue;
                    if (src.fifo.count < min_count) min_count = src.fifo.count;
                }
            }

            if (expected_frames > total_frames_written + 4800) {
                const diff_frames = expected_frames - total_frames_written;
                const chunk_frames: usize = @intCast(@min(diff_frames, 960));
                const float_count: usize = chunk_frames * 2;
                @memset(chunk_mix[0..float_count], 0.0);
                if (self.mux) |*m| {
                    m.writeAudioSamplesTrack(SOURCE_MIX_TRACK, chunk_mix[0..float_count], 0) catch {};
                    for (&sources) |*src| {
                        if (src.enabled and src.track >= 0) {
                            m.writeAudioSamplesTrack(@intCast(src.track), chunk_mix[0..float_count], 0) catch {};
                        }
                    }
                }
                if (self.wav_out) |*w| w.writeSamplesFloat(chunk_mix[0..float_count]);
                total_frames_written += chunk_frames;
                did_read = true;
            }

            if (!did_read) {
                clock.sleepMs(5);
            } else {
                clock.sleepMs(1);
            }
        }
    }
};
