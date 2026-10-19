//! Windows Graphics Capture (WGC) wrapper in Zig.
//! Hardware-accelerated Zero-Copy window and monitor capture for Windows 10/11.
const std = @import("std");
pub const d3d11 = @import("d3d11_utils.zig");
const types = @import("../../core/types.zig");

pub const WgcConfig = extern struct {
    width: u32 = 0,
    height: u32 = 0,
    capture_cursor: bool = true,
    draw_border: bool = false,
};

extern fn wgc_is_supported() callconv(.c) bool;
extern fn wgc_capture_create_window(hwnd: ?*anyopaque, d3d11_device: ?*anyopaque, d3d11_context: ?*anyopaque, config: *const WgcConfig) callconv(.c) ?*anyopaque;
extern fn wgc_capture_create_monitor(hmonitor: ?*anyopaque, d3d11_device: ?*anyopaque, d3d11_context: ?*anyopaque, config: *const WgcConfig) callconv(.c) ?*anyopaque;
extern fn wgc_capture_acquire_frame(cap: ?*anyopaque, dest_texture: ?*anyopaque, timeout_ms: u32, out_width: *u32, out_height: *u32) callconv(.c) bool;
extern fn wgc_capture_get_size(cap: ?*anyopaque, out_width: *u32, out_height: *u32) callconv(.c) void;
extern fn wgc_capture_destroy(cap: ?*anyopaque) callconv(.c) void;

pub const WgcCapture = struct {
    handle: *anyopaque,
    d3d: d3d11.D3D11Context,
    dest_texture: *d3d11.ID3D11Texture2D,
    width: u32,
    height: u32,
    frame_count: u64 = 0,

    pub fn isSupported() bool {
        return wgc_is_supported();
    }

    pub fn initWindow(hwnd: ?*anyopaque, config: WgcConfig) !WgcCapture {
        if (!isSupported()) return error.WgcNotSupported;
        if (hwnd == null) return error.InvalidWindowHandle;

        var d3d_ctx = try d3d11.D3D11Context.init();
        errdefer d3d_ctx.deinit();

        const handle = wgc_capture_create_window(hwnd, @ptrCast(d3d_ctx.device), @ptrCast(d3d_ctx.context), &config);
        if (handle == null) {
            return error.WgcSessionCreationFailed;
        }
        errdefer wgc_capture_destroy(handle);

        var actual_w: u32 = config.width;
        var actual_h: u32 = config.height;
        wgc_capture_get_size(handle, &actual_w, &actual_h);
        if (actual_w == 0 or actual_h == 0) {
            actual_w = 1920;
            actual_h = 1080;
        }

        const tex_desc = d3d11.D3D11_TEXTURE2D_DESC{
            .Width = actual_w,
            .Height = actual_h,
            .Format = d3d11.DXGI_FORMAT_B8G8R8A8_UNORM,
            .Usage = d3d11.D3D11_USAGE_DEFAULT,
            .BindFlags = d3d11.D3D11_BIND_RENDER_TARGET | d3d11.D3D11_BIND_SHADER_RESOURCE,
            .MiscFlags = 0x2, // D3D11_RESOURCE_MISC_SHARED
        };

        const dest_tex = try d3d_ctx.device.createTexture2D(&tex_desc);

        return .{
            .handle = handle.?,
            .d3d = d3d_ctx,
            .dest_texture = dest_tex,
            .width = actual_w,
            .height = actual_h,
        };
    }

    pub fn initMonitor(hmonitor: ?*anyopaque, config: WgcConfig) !WgcCapture {
        if (!isSupported()) return error.WgcNotSupported;
        if (hmonitor == null) return error.InvalidMonitorHandle;

        var d3d_ctx = try d3d11.D3D11Context.init();
        errdefer d3d_ctx.deinit();

        const handle = wgc_capture_create_monitor(hmonitor, @ptrCast(d3d_ctx.device), @ptrCast(d3d_ctx.context), &config);
        if (handle == null) {
            return error.WgcSessionCreationFailed;
        }
        errdefer wgc_capture_destroy(handle);

        var actual_w: u32 = config.width;
        var actual_h: u32 = config.height;
        wgc_capture_get_size(handle, &actual_w, &actual_h);
        if (actual_w == 0 or actual_h == 0) {
            actual_w = 1920;
            actual_h = 1080;
        }

        const tex_desc = d3d11.D3D11_TEXTURE2D_DESC{
            .Width = actual_w,
            .Height = actual_h,
            .Format = d3d11.DXGI_FORMAT_B8G8R8A8_UNORM,
            .Usage = d3d11.D3D11_USAGE_DEFAULT,
            .BindFlags = d3d11.D3D11_BIND_RENDER_TARGET | d3d11.D3D11_BIND_SHADER_RESOURCE,
            .MiscFlags = 0x2,
        };

        const dest_tex = try d3d_ctx.device.createTexture2D(&tex_desc);

        return .{
            .handle = handle.?,
            .d3d = d3d_ctx,
            .dest_texture = dest_tex,
            .width = actual_w,
            .height = actual_h,
        };
    }

    pub fn acquireFrame(self: *WgcCapture, timeout_ms: u32, pts_nanos: u64) !?types.VideoFrame {
        var w: u32 = self.width;
        var h: u32 = self.height;

        const ok = wgc_capture_acquire_frame(self.handle, @ptrCast(self.dest_texture), timeout_ms, &w, &h);
        if (!ok) return null;

        self.width = w;
        self.height = h;
        self.frame_count += 1;

        return types.VideoFrame{
            .width = self.width,
            .height = self.height,
            .format = .bgra32,
            .pts_nanos = pts_nanos,
            .frame_index = self.frame_count,
            .gpu_texture_ptr = @ptrCast(self.dest_texture),
        };
    }

    pub fn deinit(self: *WgcCapture) void {
        wgc_capture_destroy(self.handle);
        _ = self.dest_texture.release();
        self.d3d.deinit();
    }
};
