//! Monotonic reference clock for audio/video synchronization using QueryPerformanceCounter.
const std = @import("std");

extern "kernel32" fn QueryPerformanceCounter(lpPerformanceCount: *i64) callconv(.winapi) i32;
extern "kernel32" fn QueryPerformanceFrequency(lpFrequency: *i64) callconv(.winapi) i32;
extern "kernel32" fn Sleep(dwMilliseconds: u32) callconv(.winapi) void;
extern "kernel32" fn CreateWaitableTimerExW(lpTimerAttributes: ?*anyopaque, lpTimerName: ?[*:0]const u16, dwFlags: u32, dwDesiredAccess: u32) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn SetWaitableTimer(hTimer: *anyopaque, lpDueTime: *const i64, lPeriod: i32, pfnCompletionRoutine: ?*const anyopaque, lpArgToCompletionRoutine: ?*anyopaque, fResume: i32) callconv(.winapi) i32;
extern "kernel32" fn WaitForSingleObject(hHandle: *anyopaque, dwMilliseconds: u32) callconv(.winapi) u32;
extern "winmm" fn timeBeginPeriod(uPeriod: u32) callconv(.winapi) u32;
extern "winmm" fn timeEndPeriod(uPeriod: u32) callconv(.winapi) u32;

var g_cached_qpc_freq: std.atomic.Value(i64) = .init(0);
threadlocal var tls_timer_init: bool = false;
threadlocal var tls_highres_timer: ?*anyopaque = null;

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
    if (!tls_timer_init) {
        tls_timer_init = true;
        tls_highres_timer = CreateWaitableTimerExW(null, null, 0x00000002, 0x1F0003);
    }

    while (true) {
        const now = MonotonicClock.nowNanos();
        if (now >= target_time_nanos) break;
        const diff_ns = target_time_nanos - now;
        if (tls_highres_timer) |timer| {
            if (diff_ns >= 450_000) {
                const wait_ns = diff_ns - 250_000;
                var due_100ns: i64 = -@as(i64, @intCast(wait_ns / 100));
                if (due_100ns < 0 and SetWaitableTimer(timer, &due_100ns, 0, null, null, 0) != 0) {
                    _ = WaitForSingleObject(timer, 50);
                    continue;
                }
            }
        } else if (diff_ns >= 2_000_000) {
            Sleep(1);
            continue;
        }
        if (diff_ns >= 120_000) {
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
        _ = QueryPerformanceCounter(&count);
        var freq = g_cached_qpc_freq.load(.monotonic);
        if (freq == 0) {
            _ = QueryPerformanceFrequency(&freq);
            if (freq == 0) return 0;
            g_cached_qpc_freq.store(freq, .monotonic);
        }
        const nanos: u128 = (@as(u128, @intCast(count)) * std.time.ns_per_s) / @as(u128, @intCast(freq));
        return @intCast(nanos);
    }
};
