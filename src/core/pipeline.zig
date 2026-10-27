const std = @import("std");
const types = @import("types.zig");
const stats = @import("stats.zig");
const command = @import("command.zig");
const ring_buffer = @import("ring_buffer.zig");
const clock = @import("clock.zig");
const config = @import("../config/config.zig");

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

        // 3. Initialize Container Muxer with Encoder's stream parameters
        const out_path_z: [*:0]const u8 = @ptrCast(&self.output_path);
        const has_audio = self.cfg.audio.system_audio_enabled or self.cfg.audio.mic_enabled;
        const enable_multi_track = self.cfg.audio.separate_tracks and self.cfg.audio.system_audio_enabled and self.cfg.audio.mic_enabled;
        var mkv_mux = try muxer.Muxer.init(
            out_path_z,
            video_enc.handle,
            has_audio,
            enable_multi_track,
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

                // Update real-time screen preview (throttled to 30 FPS to save GPU/CPU bandwidth)
                if (frame_idx % 2 == 0 and app_window.imgui_is_preview_enabled()) {
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

    // 3. Audio Thread: WASAPI Loopback & Microphone Capture (HIGHEST priority)
    fn audioWorker(self: *Pipeline) void {
        _ = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

        var sys_audio: ?wasapi.WasapiCapture = null;
        if (self.cfg.audio.system_audio_enabled) {
            sys_audio = wasapi.WasapiCapture.initLoopback(self.cfg.audio.selected_audio_output) catch |err| blk: {
                log.warn("WASAPI loopback not available: {s}", .{@errorName(err)});
                break :blk null;
            };
        }
        defer if (sys_audio) |*a| a.deinit();

        var mic_audio: ?wasapi.WasapiCapture = null;
        if (self.cfg.audio.mic_enabled) {
            mic_audio = wasapi.WasapiCapture.initCapture(self.cfg.audio.selected_audio_input) catch |err| blk: {
                log.warn("WASAPI mic capture not available: {s}", .{@errorName(err)});
                break :blk null;
            };
        }
        defer if (mic_audio) |*m| m.deinit();

        var sys_buffer: [4096]f32 = undefined;
        var mic_buffer: [4096]f32 = undefined;
        var sys_peak: f32 = 0.0;
        var mic_peak: f32 = 0.0;

        const multi_track = self.cfg.audio.separate_tracks and (sys_audio != null) and (mic_audio != null);
        var total_frames_written: u64 = 0;
        const sample_rate: u64 = 48000;

        var sys_fifo = AudioFifo{};
        var mic_fifo = AudioFifo{};

        var chunk_sys: [2048]f32 = undefined;
        var chunk_mic: [2048]f32 = undefined;
        var chunk_mix: [2048]f32 = undefined;

        while (self.audio_active.load(.acquire)) {
            var did_read = false;
            const now_ns = clock.MonotonicClock.nowNanos();
            const elapsed_ns = if (now_ns > self.master_start_nanos) now_ns - self.master_start_nanos else 0;
            const expected_frames = (elapsed_ns * sample_rate) / std.time.ns_per_s;
            // Max allowed audio frames sent to muxer (hard cap at 100ms ahead of real time)
            const max_allowed_frames = expected_frames + 4800;

            // 1. Read System Audio
            if (sys_audio) |*a| {
                const sys_count = a.readSamples(&sys_buffer, &sys_peak);
                if (sys_count > 0) {
                    did_read = true;
                    const scaled_peak: u32 = @intFromFloat(sys_peak * 10000.0);
                    self.stats.audio_peak_system.store(scaled_peak, .monotonic);

                    if (self.cfg.audio.system_audio_volume != 1.0) {
                        for (sys_buffer[0..sys_count]) |*s| {
                            s.* *= self.cfg.audio.system_audio_volume;
                        }
                    }
                    if (mic_audio != null) {
                        sys_fifo.write(sys_buffer[0..sys_count]);
                    } else {
                        // System audio only mode
                        if (total_frames_written < max_allowed_frames) {
                            if (self.mux) |*m| m.writeAudioSamples(sys_buffer[0..sys_count], 0) catch {};
                            if (self.wav_out) |*w| w.writeSamplesFloat(sys_buffer[0..sys_count]);
                            total_frames_written += sys_count / 2;
                        }
                    }
                }
            }

            // 2. Read Microphone Audio
            if (mic_audio) |*m_cap| {
                const mic_count = m_cap.readSamples(&mic_buffer, &mic_peak);
                if (mic_count > 0) {
                    did_read = true;
                    const scaled_peak: u32 = @intFromFloat(mic_peak * 10000.0);
                    self.stats.audio_peak_mic.store(scaled_peak, .monotonic);

                    if (self.cfg.audio.mic_volume != 1.0) {
                        for (mic_buffer[0..mic_count]) |*s| {
                            s.* *= self.cfg.audio.mic_volume;
                        }
                    }
                    if (sys_audio != null) {
                        mic_fifo.write(mic_buffer[0..mic_count]);
                    } else {
                        // Microphone only mode
                        if (total_frames_written < max_allowed_frames) {
                            if (self.mux) |*m| m.writeAudioSamples(mic_buffer[0..mic_count], 0) catch {};
                            if (self.wav_out) |*w| w.writeSamplesFloat(mic_buffer[0..mic_count]);
                            total_frames_written += mic_count / 2;
                        }
                    }
                }
            }

            // 3. Dual-track / Mixed routing
            if (sys_audio != null and mic_audio != null) {
                // Balance FIFOs only when one source is lagging significantly (>100ms = 9600 samples)
                if (sys_fifo.count > mic_fifo.count + 9600) {
                    const pad = (sys_fifo.count - mic_fifo.count) & ~@as(usize, 1);
                    mic_fifo.padZeros(pad);
                } else if (mic_fifo.count > sys_fifo.count + 9600) {
                    const pad = (mic_fifo.count - sys_fifo.count) & ~@as(usize, 1);
                    sys_fifo.padZeros(pad);
                }

                while (sys_fifo.count >= 2 and mic_fifo.count >= 2 and total_frames_written < max_allowed_frames) {
                    const pair_count = @min(@min(sys_fifo.count, mic_fifo.count), 2048) & ~@as(usize, 1);
                    if (pair_count == 0) break;
                    _ = sys_fifo.read(chunk_sys[0..pair_count]);
                    _ = mic_fifo.read(chunk_mic[0..pair_count]);
                    for (0..pair_count) |i| {
                        chunk_mix[i] = std.math.clamp(chunk_sys[i] + chunk_mic[i], -1.0, 1.0);
                    }
                    if (self.mux) |*m| {
                        m.writeAudioSamplesTrack(0, chunk_mix[0..pair_count], 0) catch {};
                        if (multi_track) {
                            m.writeAudioSamplesTrack(1, chunk_mic[0..pair_count], 0) catch {};
                        }
                    }
                    if (self.wav_out) |*w| w.writeSamplesFloat(chunk_mix[0..pair_count]);
                    total_frames_written += pair_count / 2;
                    did_read = true;
                }
            }

            // 4. Silence keeper: if audio is lagging behind real-time by > 100ms (idle sound card / no audio playing)
            if (expected_frames > total_frames_written + 4800) {
                const diff_frames = expected_frames - total_frames_written;
                const chunk_frames: usize = @intCast(@min(diff_frames, 960));
                const float_count: usize = chunk_frames * 2;
                @memset(chunk_mix[0..float_count], 0.0);
                if (self.mux) |*m| {
                    if (sys_audio != null and mic_audio != null) {
                        m.writeAudioSamplesTrack(0, chunk_mix[0..float_count], 0) catch {};
                        if (multi_track) {
                            m.writeAudioSamplesTrack(1, chunk_mix[0..float_count], 0) catch {};
                        }
                    } else {
                        m.writeAudioSamples(chunk_mix[0..float_count], 0) catch {};
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
