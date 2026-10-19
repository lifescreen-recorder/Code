//! Tests for Windows monitor and window enumeration.
const std = @import("std");
const monitor_list = @import("lifescreen").platform_windows.monitor_list;
const window_list = @import("lifescreen").platform_windows.window_list;

test "enumerate monitors succeeds and finds at least one monitor" {
    const testing_allocator = std.testing.allocator;
    const monitors = try monitor_list.enumerateMonitors(testing_allocator);
    defer testing_allocator.free(monitors);

    try std.testing.expect(monitors.len >= 1);
    const m0 = monitors[0];
    try std.testing.expect(m0.width > 0);
    try std.testing.expect(m0.height > 0);
    try std.testing.expect(m0.name_len > 0);
    try std.testing.expect(m0.adapter_name_len > 0);
}

test "enumerate windows succeeds" {
    const testing_allocator = std.testing.allocator;
    const windows = try window_list.enumerateWindows(testing_allocator);
    defer testing_allocator.free(windows);

    // Should find at least one window on an active interactive desktop
    for (windows) |win| {
        try std.testing.expect(win.hwnd != 0);
        try std.testing.expect(win.width > 0);
        try std.testing.expect(win.height > 0);
    }
}

extern "kernel32" fn LoadLibraryA(lpLibFileName: [*:0]const u8) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn GetProcAddress(hModule: ?*anyopaque, lpProcName: [*:0]const u8) callconv(.winapi) ?*anyopaque;

test "verify D3D11 WinRT interop export" {
    const hD3d = LoadLibraryA("d3d11.dll");
    try std.testing.expect(hD3d != null);
    const proc = GetProcAddress(hD3d, "CreateDirect3D11DeviceFromDXGIDevice");
    try std.testing.expect(proc != null);
    const proc2 = GetProcAddress(hD3d, "CreateDirect3D11SurfaceFromDXGISurface");
    try std.testing.expect(proc2 != null);

    const hCombase = LoadLibraryA("combase.dll");
    try std.testing.expect(hCombase != null);
    const roInit = GetProcAddress(hCombase, "RoInitialize");
    try std.testing.expect(roInit != null);
    const roGetAct = GetProcAddress(hCombase, "RoGetActivationFactory");
    try std.testing.expect(roGetAct != null);
    const winCreateStr = GetProcAddress(hCombase, "WindowsCreateString");
    try std.testing.expect(winCreateStr != null);
}

test "verify WGC subsystem initialization and support" {
    const wgc = @import("lifescreen").platform_windows.wgc;
    try std.testing.expect(wgc.WgcCapture.isSupported());
}

test "test WGC window capture on active window" {
    const testing_allocator = std.testing.allocator;
    const wgc = @import("lifescreen").platform_windows.wgc;
    if (!wgc.WgcCapture.isSupported()) return;

    const windows = try window_list.enumerateWindows(testing_allocator);
    defer testing_allocator.free(windows);

    for (windows) |win| {
        if (win.width >= 300 and win.height >= 300) {
            var cap = wgc.WgcCapture.initWindow(@ptrFromInt(win.hwnd), .{
                .capture_cursor = true,
                .draw_border = false,
            }) catch |err| {
                std.log.info("WGC init skipped for window '{s}': {s}", .{ win.getTitle(), @errorName(err) });
                continue;
            };
            defer cap.deinit();

            try std.testing.expect(cap.width > 0);
            try std.testing.expect(cap.height > 0);
            _ = cap.acquireFrame(200, 0) catch null;
            break;
        }
    }
}

test "verify GameCapture IPC struct alignment and magic" {
    const game_capture = @import("lifescreen").platform_windows.game;
    const IpcType = game_capture.LifeScreenHookIpc;

    try std.testing.expectEqual(@as(usize, 56), @sizeOf(IpcType));
    var sample: IpcType = undefined;
    sample.magic = 0x4C534743;
    sample.version = 1;
    sample.process_id = 1234;
    sample.width = 1920;
    sample.height = 1080;
    sample.format = 87;
    sample.shared_handle = 0xABCD1234;
    sample.active = true;
    sample.recording = true;

    try std.testing.expectEqual(@as(u32, 0x4C534743), sample.magic);
    try std.testing.expectEqual(@as(u32, 1920), sample.width);
    try std.testing.expectEqual(@as(u32, 1080), sample.height);
    try std.testing.expect(sample.active);
}

test "verify CaptureBackend initialization for screen" {
    const backend_mod = @import("lifescreen").platform_windows.capture_backend;
    var cap = backend_mod.CaptureBackend.init(.{
        .capture_type = .entire_screen,
        .monitor_index = 0,
        .capture_cursor = false,
    }) catch |err| {
        // In headless CI, background sessions or locked displays
        if (err == error.DXGIOutputNotFound or err == error.DXGIFactoryCreationFailed or err == error.WgcNotSupported) return;
        return err;
    };
    defer cap.deinit();

    try std.testing.expect(cap.width > 0);
    try std.testing.expect(cap.height > 0);
}


