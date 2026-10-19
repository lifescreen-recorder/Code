//! DXGI Desktop Duplication capture backend for Windows (Windows 8, 10, 11).
//! Direct Zero-Copy capture pipeline: frames stay in GPU VRAM (D3D11Texture2D).
const std = @import("std");
pub const d3d11 = @import("d3d11_utils.zig");
const types = @import("../../core/types.zig");

pub const HRESULT = i32;
pub const S_OK = 0;

pub const DXGI_OUTDUPL_FRAME_INFO = extern struct {
    LastPresentTime: i64,
    LastMouseUpdateTime: i64,
    AccumulatedFrames: u32,
    RectsCoalesced: i32,
    ProtectedContentMaskedOut: i32,
    PointerPosition: extern struct {
        Position: extern struct { x: i32, y: i32 },
        Visible: i32,
    },
    TotalMetadataBufferSize: u32,
    PointerShapeBufferSize: u32,
};

pub const DXGI_OUTPUT_DESC = extern struct {
    DeviceName: [32]u16,
    DesktopCoordinates: extern struct {
        left: i32,
        top: i32,
        right: i32,
        bottom: i32,
    },
    AttachedToDesktop: i32,
    Rotation: u32,
    Monitor: ?*anyopaque,
};

// COM GUIDs
const IID_IDXGIFactory1 = std.os.windows.GUID{
    .Data1 = 0x770aae78,
    .Data2 = 0xf26f,
    .Data3 = 0x4dba,
    .Data4 = .{ 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 },
};

const IID_IDXGIOutput1 = std.os.windows.GUID{
    .Data1 = 0x00cddea8,
    .Data2 = 0x939b,
    .Data3 = 0x4b83,
    .Data4 = .{ 0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc },
};

const IID_ID3D11Texture2D = std.os.windows.GUID{
    .Data1 = 0x6f15aaf2,
    .Data2 = 0xd208,
    .Data3 = 0x4e89,
    .Data4 = .{ 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c },
};

const IID_IDXGISurface1 = std.os.windows.GUID{
    .Data1 = 0x4AE63092,
    .Data2 = 0x6327,
    .Data3 = 0x4829,
    .Data4 = .{ 0x88, 0x2C, 0x2E, 0x6A, 0x83, 0xFD, 0x5D, 0x3E },
};

pub const IDXGISurface1 = extern struct {
    vtable: *const VTable,
    pub const VTable = extern struct {
        // IUnknown (3)
        base: d3d11.IUnknown.VTable,
        // IDXGIObject (4)
        SetPrivateData: ?*const anyopaque,
        SetPrivateDataInterface: ?*const anyopaque,
        GetPrivateData: ?*const anyopaque,
        GetParent: ?*const anyopaque,
        // IDXGIDeviceSubObject (1)
        GetDevice: ?*const anyopaque,
        // IDXGISurface (3)
        GetDesc: ?*const anyopaque,
        Map: ?*const anyopaque,
        Unmap: ?*const anyopaque,
        // IDXGISurface1 (2)
        GetDC: *const fn (self: *IDXGISurface1, Discard: i32, phdc: *?*anyopaque) callconv(.winapi) HRESULT,
        ReleaseDC: *const fn (self: *IDXGISurface1, pDirtyRect: ?*anyopaque) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IDXGISurface1) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

pub const CURSORINFO = extern struct {
    cbSize: u32 = @sizeOf(CURSORINFO),
    flags: u32 = 0,
    hCursor: ?*anyopaque = null,
    ptScreenPos: extern struct { x: i32, y: i32 } = .{ .x = 0, .y = 0 },
};

pub const ICONINFO = extern struct {
    fIcon: i32 = 0,
    xHotspot: u32 = 0,
    yHotspot: u32 = 0,
    hbmMask: ?*anyopaque = null,
    hbmColor: ?*anyopaque = null,
};

pub extern "user32" fn GetCursorInfo(pci: *CURSORINFO) callconv(.winapi) i32;
pub extern "user32" fn GetIconInfo(hIcon: ?*anyopaque, piconinfo: *ICONINFO) callconv(.winapi) i32;
pub extern "gdi32" fn DeleteObject(ho: ?*anyopaque) callconv(.winapi) i32;
pub extern "user32" fn DrawIconEx(
    hdc: ?*anyopaque,
    xLeft: i32,
    yTop: i32,
    hIcon: ?*anyopaque,
    cxWidth: i32,
    cyWidth: i32,
    istepIfAniCur: u32,
    hbrFlickerFreeDraw: ?*anyopaque,
    diFlags: u32,
) callconv(.winapi) i32;

pub const IDXGIOutputDuplication = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        // IUnknown (3)
        base: d3d11.IUnknown.VTable,
        // IDXGIObject (4)
        SetPrivateData: ?*const anyopaque,
        SetPrivateDataInterface: ?*const anyopaque,
        GetPrivateData: ?*const anyopaque,
        GetParent: ?*const anyopaque,
        // IDXGIOutputDuplication (8)
        GetDesc: ?*const anyopaque,
        AcquireNextFrame: *const fn (
            self: *IDXGIOutputDuplication,
            TimeoutInMilliseconds: u32,
            pFrameInfo: *DXGI_OUTDUPL_FRAME_INFO,
            ppDesktopResource: *?*d3d11.ID3D11Resource,
        ) callconv(.winapi) HRESULT,
        GetFrameDirtyRects: ?*const anyopaque,
        GetFrameMoveRects: ?*const anyopaque,
        GetFramePointerShape: ?*const anyopaque,
        MapDesktopSurface: ?*const anyopaque,
        UnMapDesktopSurface: ?*const anyopaque,
        ReleaseFrame: *const fn (self: *IDXGIOutputDuplication) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IDXGIOutputDuplication) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

pub const IDXGIOutput = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        // IUnknown (3)
        base: d3d11.IUnknown.VTable,
        // IDXGIObject (4)
        SetPrivateData: ?*const anyopaque,
        SetPrivateDataInterface: ?*const anyopaque,
        GetPrivateData: ?*const anyopaque,
        GetParent: ?*const anyopaque,
        // IDXGIOutput (12)
        GetDesc: *const fn (self: *IDXGIOutput, pDesc: *DXGI_OUTPUT_DESC) callconv(.winapi) HRESULT,
        GetDisplayModeList: ?*const anyopaque,
        FindClosestMatchingMode: ?*const anyopaque,
        WaitForVBlank: ?*const anyopaque,
        TakeOwnership: ?*const anyopaque,
        ReleaseOwnership: ?*const anyopaque,
        GetGammaControlCapabilities: ?*const anyopaque,
        SetGammaControl: ?*const anyopaque,
        GetGammaControl: ?*const anyopaque,
        SetDisplaySurface: ?*const anyopaque,
        GetDisplaySurfaceData: ?*const anyopaque,
        GetFrameStatistics: ?*const anyopaque,
    };

    pub inline fn release(self: *IDXGIOutput) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

pub const IDXGIOutput1 = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        // IUnknown (3)
        base: d3d11.IUnknown.VTable,
        // IDXGIObject (4)
        SetPrivateData: ?*const anyopaque,
        SetPrivateDataInterface: ?*const anyopaque,
        GetPrivateData: ?*const anyopaque,
        GetParent: ?*const anyopaque,
        // IDXGIOutput (12)
        GetDesc: ?*const anyopaque,
        GetDisplayModeList: ?*const anyopaque,
        FindClosestMatchingMode: ?*const anyopaque,
        WaitForVBlank: ?*const anyopaque,
        TakeOwnership: ?*const anyopaque,
        ReleaseOwnership: ?*const anyopaque,
        GetGammaControlCapabilities: ?*const anyopaque,
        SetGammaControl: ?*const anyopaque,
        GetGammaControl: ?*const anyopaque,
        SetDisplaySurface: ?*const anyopaque,
        GetDisplaySurfaceData: ?*const anyopaque,
        GetFrameStatistics: ?*const anyopaque,
        // IDXGIOutput1 (4)
        GetDisplayModeList1: ?*const anyopaque,
        FindClosestMatchingMode1: ?*const anyopaque,
        GetDisplaySurfaceData1: ?*const anyopaque,
        DuplicateOutput: *const fn (
            self: *IDXGIOutput1,
            pDevice: *d3d11.IUnknown,
            ppOutputDuplication: *?*IDXGIOutputDuplication,
        ) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IDXGIOutput1) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

pub const DXGI_ADAPTER_DESC1 = extern struct {
    Description: [128]u16,
    VendorId: u32,
    DeviceId: u32,
    SubSysId: u32,
    Revision: u32,
    DedicatedVideoMemory: usize,
    DedicatedSystemMemory: usize,
    SharedSystemMemory: usize,
    AdapterLuid: extern struct { LowPart: u32, HighPart: i32 },
    Flags: u32,
};

pub const IDXGIAdapter1 = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        // IUnknown (3)
        base: d3d11.IUnknown.VTable,
        // IDXGIObject (4)
        SetPrivateData: ?*const anyopaque,
        SetPrivateDataInterface: ?*const anyopaque,
        GetPrivateData: ?*const anyopaque,
        GetParent: ?*const anyopaque,
        // IDXGIAdapter (3)
        EnumOutputs: *const fn (self: *IDXGIAdapter1, Output: u32, ppOutput: *?*IDXGIOutput) callconv(.winapi) HRESULT,
        GetDesc: ?*const anyopaque,
        CheckInterfaceSupport: ?*const anyopaque,
        // IDXGIAdapter1 (1)
        GetDesc1: *const fn (self: *IDXGIAdapter1, pDesc: *DXGI_ADAPTER_DESC1) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IDXGIAdapter1) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

pub const IDXGIFactory1 = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        // IUnknown (3)
        base: d3d11.IUnknown.VTable,
        // IDXGIObject (4)
        SetPrivateData: ?*const anyopaque,
        SetPrivateDataInterface: ?*const anyopaque,
        GetPrivateData: ?*const anyopaque,
        GetParent: ?*const anyopaque,
        // IDXGIFactory (5)
        EnumAdapters: ?*const anyopaque,
        MakeWindowAssociation: ?*const anyopaque,
        GetWindowAssociation: ?*const anyopaque,
        CreateSwapChain: ?*const anyopaque,
        CreateSoftwareAdapter: ?*const anyopaque,
        // IDXGIFactory1 (2)
        EnumAdapters1: *const fn (self: *IDXGIFactory1, Adapter: u32, ppAdapter: *?*IDXGIAdapter1) callconv(.winapi) HRESULT,
        IsCurrent: ?*const anyopaque,
    };

    pub inline fn release(self: *IDXGIFactory1) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

pub extern "dxgi" fn CreateDXGIFactory1(riid: *const std.os.windows.GUID, ppFactory: *?*IDXGIFactory1) callconv(.winapi) HRESULT;
pub extern "user32" fn OpenInputDesktop(dwFlags: u32, fInherit: i32, dwDesiredAccess: u32) callconv(.winapi) ?*anyopaque;
pub extern "user32" fn SetThreadDesktop(hDesktop: ?*anyopaque) callconv(.winapi) i32;
pub extern "user32" fn CloseDesktop(hDesktop: ?*anyopaque) callconv(.winapi) i32;

fn getAdapterName(adapter: *IDXGIAdapter1, buf: *[128]u8) []const u8 {
    var desc: DXGI_ADAPTER_DESC1 = undefined;
    if (adapter.vtable.GetDesc1(adapter, &desc) != S_OK) return "Unknown Adapter";
    var len: usize = 0;
    for (desc.Description) |c| {
        if (c == 0 or len >= buf.len) break;
        buf[len] = if (c < 128) @intCast(c) else '?';
        len += 1;
    }
    return buf[0..len];
}

pub const DxgiCapture = struct {
    d3d: d3d11.D3D11Context,
    duplication: *IDXGIOutputDuplication,
    dest_texture: *d3d11.ID3D11Texture2D,
    desktop_handle: ?*anyopaque = null,
    width: u32,
    height: u32,
    monitor_index: u32,
    offset_x: i32 = 0,
    offset_y: i32 = 0,
    capture_cursor: bool = true,
    frame_count: u64 = 0,

    pub fn init(monitor_index: u32, capture_cursor: bool) !DxgiCapture {
        // Switch thread to active interactive input desktop (required when running via IDE / services / background tasks)
        const input_desk = OpenInputDesktop(0, 0, 0x10000000);
        if (input_desk) |desk| {
            _ = SetThreadDesktop(desk);
        }

        var factory: ?*IDXGIFactory1 = null;
        if (CreateDXGIFactory1(&IID_IDXGIFactory1, &factory) != S_OK or factory == null) {
            return error.DXGIFactoryCreationFailed;
        }
        defer _ = factory.?.release();

        var name_buf: [128]u8 = undefined;

        // Iterate through all adapters and outputs to find matching global monitor index
        var global_mon_idx: u32 = 0;
        var a_idx: u32 = 0;
        var curr_adapter: ?*IDXGIAdapter1 = null;
        while (factory.?.vtable.EnumAdapters1(factory.?, a_idx, &curr_adapter) == S_OK) : (a_idx += 1) {
            defer {
                if (curr_adapter) |a| _ = a.release();
                curr_adapter = null;
            }

            const adapter = curr_adapter.?;
            const adapter_name = getAdapterName(adapter, &name_buf);

            var o_idx: u32 = 0;
            var curr_output: ?*IDXGIOutput = null;
            while (adapter.vtable.EnumOutputs(adapter, o_idx, &curr_output) == S_OK) : (o_idx += 1) {
                defer {
                    if (curr_output) |o| _ = o.release();
                    curr_output = null;
                }

                if (global_mon_idx != monitor_index) {
                    global_mon_idx += 1;
                    continue;
                }
                global_mon_idx += 1;

                std.log.scoped(.dxgi).info("Checking Adapter [{d}]: '{s}', Output [{d}]", .{ a_idx, adapter_name, o_idx });

                const output = curr_output.?;
                var desc: DXGI_OUTPUT_DESC = undefined;
                _ = output.vtable.GetDesc(output, &desc);
                const width: u32 = @intCast(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left);
                const height: u32 = @intCast(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top);
                std.log.scoped(.dxgi).info("  -> Found Monitor [{d}]: {d}x{d}", .{ monitor_index, width, height });

                // Create D3D11 device bound to this adapter
                var d3d_ctx = d3d11.D3D11Context.initWithAdapter(@ptrCast(adapter)) catch |err| {
                    std.log.scoped(.dxgi).warn("  -> Failed to create D3D11 device on adapter: {s}", .{@errorName(err)});
                    continue;
                };

                var output1_ptr: ?*anyopaque = null;
                if (output.vtable.base.QueryInterface(@ptrCast(output), &IID_IDXGIOutput1, &output1_ptr) != S_OK or output1_ptr == null) {
                    d3d_ctx.deinit();
                    continue;
                }
                const output1: *IDXGIOutput1 = @ptrCast(@alignCast(output1_ptr.?));
                defer _ = output1.release();

                var duplication: ?*IDXGIOutputDuplication = null;
                const hr = output1.vtable.DuplicateOutput(output1, @ptrCast(d3d_ctx.device), &duplication);
                if (hr != S_OK or duplication == null) {
                    std.log.scoped(.dxgi).warn("  -> DuplicateOutput returned HRESULT=0x{x} on adapter '{s}'", .{
                        @as(u32, @bitCast(hr)),
                        adapter_name,
                    });
                    d3d_ctx.deinit();
                    continue;
                }

                std.log.scoped(.dxgi).info("  -> DuplicateOutput SUCCEEDED on adapter '{s}'!", .{adapter_name});

                // Create persistent destination texture for Zero-Copy pass (Standard fast GPU texture)
                const tex_desc = d3d11.D3D11_TEXTURE2D_DESC{
                    .Width = width,
                    .Height = height,
                    .Format = d3d11.DXGI_FORMAT_B8G8R8A8_UNORM,
                    .Usage = d3d11.D3D11_USAGE_DEFAULT,
                    .BindFlags = d3d11.D3D11_BIND_RENDER_TARGET,
                    .MiscFlags = 0,
                };

                var dest_tex: *d3d11.ID3D11Texture2D = undefined;
                if (d3d_ctx.device.createTexture2D(&tex_desc)) |tex| {
                    dest_tex = tex;
                } else |err| {
                    _ = duplication.?.release();
                    d3d_ctx.deinit();
                    return err;
                }

                return .{
                    .d3d = d3d_ctx,
                    .duplication = duplication.?,
                    .dest_texture = dest_tex,
                    .desktop_handle = input_desk,
                    .width = width,
                    .height = height,
                    .offset_x = desc.DesktopCoordinates.left,
                    .offset_y = desc.DesktopCoordinates.top,
                    .monitor_index = monitor_index,
                    .capture_cursor = capture_cursor,
                };
            }
        }

        if (input_desk) |desk| {
            _ = CloseDesktop(desk);
        }

        return error.DXGIOutputNotFound;
    }

    fn drawCursor(self: *DxgiCapture) void {
        var ci = CURSORINFO{};
        if (GetCursorInfo(&ci) == 0) return;
        if ((ci.flags & 1) == 0 or ci.hCursor == null) return; // 1 = CURSOR_SHOWING

        var surf_ptr: ?*anyopaque = null;
        if (self.dest_texture.vtable.base.base.QueryInterface(@ptrCast(self.dest_texture), &IID_IDXGISurface1, &surf_ptr) != S_OK or surf_ptr == null) {
            return;
        }
        const surf: *IDXGISurface1 = @ptrCast(@alignCast(surf_ptr.?));
        defer _ = surf.vtable.base.Release(@ptrCast(surf));

        var hdc: ?*anyopaque = null;
        if (surf.vtable.GetDC(surf, 0, &hdc) != S_OK or hdc == null) return;
        defer _ = surf.vtable.ReleaseDC(surf, null);

        var cursor_x = ci.ptScreenPos.x - self.offset_x;
        var cursor_y = ci.ptScreenPos.y - self.offset_y;

        var ii = ICONINFO{};
        if (GetIconInfo(ci.hCursor, &ii) != 0) {
            cursor_x -= @intCast(ii.xHotspot);
            cursor_y -= @intCast(ii.yHotspot);
            if (ii.hbmMask) |m| _ = DeleteObject(m);
            if (ii.hbmColor) |c| _ = DeleteObject(c);
        }

        _ = DrawIconEx(hdc, cursor_x, cursor_y, ci.hCursor, 0, 0, 0, null, 0x0003); // DI_NORMAL
    }

    /// Acquires next desktop frame directly into GPU texture with Zero-Copy.
    /// Returns null on timeout (screen hasn't changed), or VideoFrame with gpu_texture_ptr.
    pub fn acquireFrame(self: *DxgiCapture, timeout_ms: u32, pts_nanos: u64) !?types.VideoFrame {
        var frame_info: DXGI_OUTDUPL_FRAME_INFO = undefined;
        var resource: ?*d3d11.ID3D11Resource = null;

        const hr = self.duplication.vtable.AcquireNextFrame(self.duplication, timeout_ms, &frame_info, &resource);
        if (hr == d3d11.DXGI_ERROR_WAIT_TIMEOUT) {
            // Screen didn't update within timeout - not an error
            return null;
        }
        if (hr != S_OK or resource == null) {
            return error.AcquireNextFrameFailed;
        }
        defer {
            _ = resource.?.release();
            _ = self.duplication.vtable.ReleaseFrame(self.duplication);
        }

        var acquired_tex_ptr: ?*anyopaque = null;
        if (resource.?.vtable.base.QueryInterface(@ptrCast(resource.?), &IID_ID3D11Texture2D, &acquired_tex_ptr) != S_OK or acquired_tex_ptr == null) {
            return error.TextureInterfaceQueryFailed;
        }
        const acquired_tex: *d3d11.ID3D11Texture2D = @ptrCast(@alignCast(acquired_tex_ptr.?));
        defer _ = acquired_tex.release();

        // GPU-to-GPU Direct copy (Hardware blit)
        self.d3d.context.copyResource(@ptrCast(self.dest_texture), @ptrCast(acquired_tex));

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

    pub fn deinit(self: *DxgiCapture) void {
        _ = self.dest_texture.release();
        _ = self.duplication.release();
        self.d3d.deinit();
        if (self.desktop_handle) |desk| {
            _ = CloseDesktop(desk);
        }
    }
};
