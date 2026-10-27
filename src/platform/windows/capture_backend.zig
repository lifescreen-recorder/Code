//! Unified Capture Backend for Windows: DXGI Desktop Duplication, Windows Graphics Capture,
//! and High-Performance Zero-Copy Game Hook Capture.
//! Abstracts display, window, and game capture into a single zero-copy GPU pipeline interface.
const std = @import("std");
const types = @import("../../core/types.zig");
const dxgi = @import("dxgi_capture.zig");
const wgc = @import("wgc_capture.zig");
const game = @import("game_capture.zig");
const injector = @import("hook/injector.zig");
const d3d11 = @import("d3d11_utils.zig");

const log = std.log.scoped(.capture_backend);

pub const POINT = extern struct { x: i32, y: i32 };
pub extern "user32" fn MonitorFromPoint(pt: POINT, dwFlags: u32) callconv(.winapi) ?*anyopaque;

pub const CaptureBackend = struct {
    pub const BackendType = enum {
        dxgi,
        wgc,
        game,
    };

    backend_type: BackendType,
    dxgi_cap: ?dxgi.DxgiCapture = null,
    wgc_cap: ?wgc.WgcCapture = null,
    game_cap: ?game.GameCapture = null,
    width: u32,
    height: u32,

    pub fn init(target: types.CaptureTarget) !CaptureBackend {
        switch (target.capture_type) {
            .entire_screen => {
                var hmon: ?*anyopaque = null;
                if (target.monitor_handle) |h| {
                    hmon = @ptrFromInt(h);
                } else {
                    const pt = POINT{ .x = 0, .y = 0 };
                    hmon = MonitorFromPoint(pt, 1); // MONITOR_DEFAULTTOPRIMARY = 1
                }

                if (hmon != null and wgc.WgcCapture.isSupported()) {
                    if (wgc.WgcCapture.initMonitor(hmon, .{
                        .capture_cursor = target.capture_cursor,
                        .draw_border = false,
                    })) |cap| {
                        log.info("WGC Screen capture active: {d}x{d} (cursor: {any})", .{ cap.width, cap.height, target.capture_cursor });
                        return CaptureBackend{
                            .backend_type = .wgc,
                            .wgc_cap = cap,
                            .width = cap.width,
                            .height = cap.height,
                        };
                    } else |err| {
                        log.warn("WGC Monitor capture failed ({s}), falling back to DXGI Desktop Duplication...", .{@errorName(err)});
                    }
                }

                const cap = try dxgi.DxgiCapture.init(target.monitor_index, target.capture_cursor);
                return CaptureBackend{
                    .backend_type = .dxgi,
                    .dxgi_cap = cap,
                    .width = cap.width,
                    .height = cap.height,
                };
            },
            .window => {
                if (target.window_handle) |hwnd_val| {
                    const hwnd: ?*anyopaque = @ptrFromInt(hwnd_val);
                    if (wgc.WgcCapture.initWindow(hwnd, .{
                        .capture_cursor = target.capture_cursor,
                        .draw_border = false,
                    })) |cap| {
                        return CaptureBackend{
                            .backend_type = .wgc,
                            .wgc_cap = cap,
                            .width = cap.width,
                            .height = cap.height,
                        };
                    } else |err| {
                        log.warn("WGC Window capture failed ({s}), falling back to Entire Screen (DXGI)...", .{@errorName(err)});
                        const cap = try dxgi.DxgiCapture.init(0, target.capture_cursor);
                        return CaptureBackend{
                            .backend_type = .dxgi,
                            .dxgi_cap = cap,
                            .width = cap.width,
                            .height = cap.height,
                        };
                    }
                } else {
                    log.warn("No window handle specified, falling back to Entire Screen...", .{});
                    var screen_target = target;
                    screen_target.capture_type = .entire_screen;
                    return init(screen_target);
                }
            },
            .game => {
                if (target.window_handle) |hwnd_val| {
                    const hwnd: *anyopaque = @ptrFromInt(hwnd_val);
                    const pid = injector.getProcessIdFromHwnd(hwnd);
                    if (pid != 0) {
                        log.info("Attempting Game Hook capture on target PID {d}...", .{pid});
                        if (game.GameCapture.init(pid, 1500)) |gcap| {
                            log.info("Game Hook attached successfully!", .{});
                            return CaptureBackend{
                                .backend_type = .game,
                                .game_cap = gcap,
                                .width = gcap.width,
                                .height = gcap.height,
                            };
                        } else |err| {
                            log.warn("Game Hook failed ({s}), falling back to Windows Graphics Capture...", .{@errorName(err)});
                        }
                    }

                    // Graceful fallback to WGC, and if WGC fails, fallback to DXGI
                    if (wgc.WgcCapture.initWindow(hwnd, .{
                        .capture_cursor = target.capture_cursor,
                        .draw_border = false,
                    })) |cap| {
                        return CaptureBackend{
                            .backend_type = .wgc,
                            .wgc_cap = cap,
                            .width = cap.width,
                            .height = cap.height,
                        };
                    } else |err| {
                        log.warn("WGC fallback failed ({s}), falling back to Entire Screen (DXGI)...", .{@errorName(err)});
                        const cap = try dxgi.DxgiCapture.init(0, target.capture_cursor);
                        return CaptureBackend{
                            .backend_type = .dxgi,
                            .dxgi_cap = cap,
                            .width = cap.width,
                            .height = cap.height,
                        };
                    }
                } else {
                    log.warn("No game window handle specified, falling back to Entire Screen...", .{});
                    var screen_target = target;
                    screen_target.capture_type = .entire_screen;
                    return init(screen_target);
                }
            },
        }
    }

    pub fn acquireFrame(self: *CaptureBackend, timeout_ms: u32, pts_nanos: u64) !?types.VideoFrame {
        switch (self.backend_type) {
            .dxgi => return self.dxgi_cap.?.acquireFrame(timeout_ms, pts_nanos),
            .wgc => return self.wgc_cap.?.acquireFrame(timeout_ms, pts_nanos),
            .game => return self.game_cap.?.acquireFrame(timeout_ms, pts_nanos),
        }
    }

    pub fn getD3DDevice(self: *CaptureBackend) *d3d11.ID3D11Device {
        switch (self.backend_type) {
            .dxgi => return self.dxgi_cap.?.d3d.device,
            .wgc => return self.wgc_cap.?.d3d.device,
            .game => return self.game_cap.?.d3d_ctx.device,
        }
    }

    pub fn getD3DContext(self: *CaptureBackend) *d3d11.ID3D11DeviceContext {
        switch (self.backend_type) {
            .dxgi => return self.dxgi_cap.?.d3d.context,
            .wgc => return self.wgc_cap.?.d3d.context,
            .game => return self.game_cap.?.d3d_ctx.context,
        }
    }

    pub fn deinit(self: *CaptureBackend) void {
        switch (self.backend_type) {
            .dxgi => if (self.dxgi_cap) |*c| c.deinit(),
            .wgc => if (self.wgc_cap) |*c| c.deinit(),
            .game => if (self.game_cap) |*g| g.deinit(),
        }
    }
};
