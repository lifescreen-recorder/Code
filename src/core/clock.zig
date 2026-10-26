//! Monotonic reference clock for audio/video synchronization using QueryPerformanceCounter.
const std = @import("std");

extern "kernel32" fn QueryPerformanceCounter(lpPerformanceCount: *i64) callconv(.winapi) i32;
extern "kernel32" fn QueryPerformanceFrequency(lpFrequency: *i64) callconv(.winapi) i32;
extern "kernel32" fn Sleep(dwMilliseconds: u32) callconv(.winapi) void;
extern "winmm" fn timeBeginPeriod(uPeriod: u32) callconv(.winapi) u32;
extern "winmm" fn timeEndPeriod(uPeriod: u32) callconv(.winapi) u32;

pub fn enableHighResolutionTimer() void {
    _ = timeBeginPeriod(1);
}

pub fn disableHighResolutionTimer() void {
    _ = timeEndPeriod(1);
}

pub fn sleepMs(ms: u32) void {
    if (ms > 0) {
        Sleep(ms);
    }
}

pub fn sleepPreciseUntil(target_time_nanos: u64) void {
    while (true) {
        const now = MonotonicClock.nowNanos();
        if (now >= target_time_nanos) break;
        const diff_ns = target_time_nanos - now;
        if (diff_ns >= 1_200_000) {
            Sleep(1);
        } else if (diff_ns >= 150_000) {
            std.Thread.yield() catch {};
        } else {
            std.atomic.spinLoopHint();
        }
    }
}

pub const MonotonicClock = struct {
    start_time_nanos: u64 = 0,
    pause_start_nanos: u64 = 0,
    total_paused_nanos: u64 = 0,
    is_paused: bool = false,

    pub fn start() MonotonicClock {
        return .{
            .start_time_nanos = nowNanos(),
            .pause_start_nanos = 0,
            .total_paused_nanos = 0,
            .is_paused = false,
        };
    }

    pub fn pause(self: *MonotonicClock) void {
        if (!self.is_paused) {
            self.pause_start_nanos = nowNanos();
            self.is_paused = true;
        }
    }

    pub fn @"resume"(self: *MonotonicClock) void {
        if (self.is_paused) {
            const now = nowNanos();
            if (now > self.pause_start_nanos) {
                self.total_paused_nanos += (now - self.pause_start_nanos);
            }
            self.is_paused = false;
            self.pause_start_nanos = 0;
        }
    }

    /// Returns presentation timestamp (PTS) in nanoseconds relative to start, excluding paused durations.
    pub fn getPtsNanos(self: *const MonotonicClock) u64 {
        const now = if (self.is_paused) self.pause_start_nanos else nowNanos();
        if (now <= self.start_time_nanos + self.total_paused_nanos) return 0;
        return now - self.start_time_nanos - self.total_paused_nanos;
    }

    pub fn nowNanos() u64 {
        var count: i64 = 0;
        var freq: i64 = 0;
        _ = QueryPerformanceCounter(&count);
        _ = QueryPerformanceFrequency(&freq);
        if (freq == 0) return 0;
        const nanos: u128 = (@as(u128, @intCast(count)) * std.time.ns_per_s) / @as(u128, @intCast(freq));
        return @intCast(nanos);
    }
};
