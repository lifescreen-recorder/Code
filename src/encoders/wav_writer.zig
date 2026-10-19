//! Lossless 48kHz WAV audio writer using Win32 file APIs.
const std = @import("std");

extern "kernel32" fn CreateFileA(
    lpFileName: [*:0]const u8,
    dwDesiredAccess: u32,
    dwShareMode: u32,
    lpSecurityAttributes: ?*anyopaque,
    dwCreationDisposition: u32,
    dwFlagsAndAttributes: u32,
    hTemplateFile: ?*anyopaque,
) callconv(.winapi) ?*anyopaque;

extern "kernel32" fn WriteFile(
    hFile: ?*anyopaque,
    lpBuffer: [*]const u8,
    nNumberOfBytesToWrite: u32,
    lpNumberOfBytesWritten: ?*u32,
    lpOverlapped: ?*anyopaque,
) callconv(.winapi) i32;

extern "kernel32" fn SetFilePointer(
    hFile: ?*anyopaque,
    lDistanceToMove: i32,
    lpDistanceToMoveHigh: ?*i32,
    dwMoveMethod: u32,
) callconv(.winapi) u32;

extern "kernel32" fn CloseHandle(hObject: ?*anyopaque) callconv(.winapi) i32;

const GENERIC_WRITE: u32 = 0x40000000;
const FILE_SHARE_READ: u32 = 1;
const CREATE_ALWAYS: u32 = 2;
const FILE_ATTRIBUTE_NORMAL: u32 = 0x80;
const FILE_BEGIN: u32 = 0;
const INVALID_HANDLE_VALUE: ?*anyopaque = @ptrFromInt(std.math.maxInt(usize));

pub const WavWriter = struct {
    handle: ?*anyopaque = null,
    total_data_bytes: u32 = 0,
    channels: u16 = 2,
    sample_rate: u32 = 48000,

    pub fn create(path_z: [*:0]const u8, sample_rate: u32, channels: u16) !WavWriter {
        const h = CreateFileA(
            path_z,
            GENERIC_WRITE,
            FILE_SHARE_READ,
            null,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            null,
        );

        if (h == null or h == INVALID_HANDLE_VALUE) {
            return error.WavFileCreationFailed;
        }

        var writer = WavWriter{
            .handle = h,
            .sample_rate = sample_rate,
            .channels = channels,
            .total_data_bytes = 0,
        };

        writer.writeHeader(0);
        return writer;
    }

    fn writeHeader(self: *WavWriter, data_size: u32) void {
        const h = self.handle orelse return;

        var header: [44]u8 = undefined;
        const riff_size: u32 = data_size + 36;
        const byte_rate: u32 = self.sample_rate * @as(u32, self.channels) * 2;
        const block_align: u16 = self.channels * 2;

        @memcpy(header[0..4], "RIFF");
        std.mem.writeInt(u32, header[4..8], riff_size, .little);
        @memcpy(header[8..12], "WAVE");
        @memcpy(header[12..16], "fmt ");
        std.mem.writeInt(u32, header[16..20], 16, .little); // Subchunk1Size
        std.mem.writeInt(u16, header[20..22], 1, .little);  // PCM format
        std.mem.writeInt(u16, header[22..24], self.channels, .little);
        std.mem.writeInt(u32, header[24..28], self.sample_rate, .little);
        std.mem.writeInt(u32, header[28..32], byte_rate, .little);
        std.mem.writeInt(u16, header[32..34], block_align, .little);
        std.mem.writeInt(u16, header[34..36], 16, .little); // 16 bits per sample
        @memcpy(header[36..40], "data");
        std.mem.writeInt(u32, header[40..44], data_size, .little);

        _ = SetFilePointer(h, 0, null, FILE_BEGIN);
        var written: u32 = 0;
        _ = WriteFile(h, &header, 44, &written, null);
    }

    pub fn writeSamplesFloat(self: *WavWriter, float_samples: []const f32) void {
        const h = self.handle orelse return;

        var pcm_buf: [2048]u8 = undefined;
        var out_idx: usize = 0;

        for (float_samples) |sample| {
            const clamped = @max(-1.0, @min(1.0, sample));
            const s16: i16 = @intFromFloat(clamped * 32767.0);
            std.mem.writeInt(i16, pcm_buf[out_idx..][0..2], s16, .little);
            out_idx += 2;

            if (out_idx + 2 > pcm_buf.len) {
                var written: u32 = 0;
                _ = WriteFile(h, &pcm_buf, @intCast(out_idx), &written, null);
                self.total_data_bytes += written;
                out_idx = 0;
            }
        }

        if (out_idx > 0) {
            var written: u32 = 0;
            _ = WriteFile(h, &pcm_buf, @intCast(out_idx), &written, null);
            self.total_data_bytes += written;
        }
    }

    pub fn finish(self: *WavWriter) void {
        if (self.handle) |h| {
            self.writeHeader(self.total_data_bytes);
            _ = CloseHandle(h);
            self.handle = null;
        }
    }

    pub fn close(self: *WavWriter) void {
        self.finish();
    }

    pub fn deinit(self: *WavWriter) void {
        self.finish();
    }
};
