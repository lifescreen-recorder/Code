//! Monitor enumeration for Windows (DXGI & Display Devices).
const std = @import("std");
const dxgi = @import("dxgi_capture.zig");
const d3d11 = @import("d3d11_utils.zig");

pub const MonitorInfo = struct {
    id: u32,
    adapter_index: u32,
    output_index: u32,
    hmonitor: ?*anyopaque = null,
    name: [32]u8 = std.mem.zeroes([32]u8),
    name_len: usize = 0,
    adapter_name: [128]u8 = std.mem.zeroes([128]u8),
    adapter_name_len: usize = 0,
    width: u32,
    height: u32,
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
    is_primary: bool,

    pub fn getName(self: *const MonitorInfo) []const u8 {
        return self.name[0..self.name_len];
    }

    pub fn getAdapterName(self: *const MonitorInfo) []const u8 {
        return self.adapter_name[0..self.adapter_name_len];
    }
};

const IID_IDXGIFactory1 = std.os.windows.GUID{
    .Data1 = 0x770aae78,
    .Data2 = 0xf26f,
    .Data3 = 0x4dba,
    .Data4 = .{ 0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87 },
};

/// Enumerates all active displays attached to desktop outputs across all DXGI adapters.
pub fn enumerateMonitors(allocator: std.mem.Allocator) ![]MonitorInfo {
    var factory: ?*dxgi.IDXGIFactory1 = null;
    if (dxgi.CreateDXGIFactory1(&IID_IDXGIFactory1, &factory) != d3d11.S_OK or factory == null) {
        return error.DXGIFactoryCreationFailed;
    }
    defer _ = factory.?.release();

    var list: std.ArrayListUnmanaged(MonitorInfo) = .empty;
    errdefer list.deinit(allocator);

    var a_idx: u32 = 0;
    var global_monitor_id: u32 = 0;
    var curr_adapter: ?*dxgi.IDXGIAdapter1 = null;

    while (factory.?.vtable.EnumAdapters1(factory.?, a_idx, &curr_adapter) == d3d11.S_OK) : (a_idx += 1) {
        defer {
            if (curr_adapter) |a| _ = a.release();
            curr_adapter = null;
        }

        const adapter = curr_adapter.?;
        var adapter_desc: dxgi.DXGI_ADAPTER_DESC1 = undefined;
        _ = adapter.vtable.GetDesc1(adapter, &adapter_desc);

        var o_idx: u32 = 0;
        var curr_output: ?*dxgi.IDXGIOutput = null;

        while (adapter.vtable.EnumOutputs(adapter, o_idx, &curr_output) == d3d11.S_OK) : (o_idx += 1) {
            defer {
                if (curr_output) |o| _ = o.release();
                curr_output = null;
            }

            const output = curr_output.?;
            var desc: dxgi.DXGI_OUTPUT_DESC = undefined;
            if (output.vtable.GetDesc(output, &desc) != d3d11.S_OK) continue;

            if (desc.AttachedToDesktop == 0) continue;

            var info: MonitorInfo = .{
                .id = global_monitor_id,
                .adapter_index = a_idx,
                .output_index = o_idx,
                .hmonitor = desc.Monitor,
                .width = @intCast(desc.DesktopCoordinates.right - desc.DesktopCoordinates.left),
                .height = @intCast(desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top),
                .left = desc.DesktopCoordinates.left,
                .top = desc.DesktopCoordinates.top,
                .right = desc.DesktopCoordinates.right,
                .bottom = desc.DesktopCoordinates.bottom,
                .is_primary = (desc.DesktopCoordinates.left == 0 and desc.DesktopCoordinates.top == 0),
            };

            // Copy device name (e.g. \\.\DISPLAY1) from UTF-16 to UTF-8
            var name_len: usize = 0;
            for (desc.DeviceName) |c| {
                if (c == 0 or name_len >= info.name.len) break;
                info.name[name_len] = if (c < 128) @intCast(c) else '?';
                name_len += 1;
            }
            info.name_len = name_len;

            // Copy adapter description from UTF-16 to UTF-8
            var a_len: usize = 0;
            for (adapter_desc.Description) |c| {
                if (c == 0 or a_len >= info.adapter_name.len) break;
                info.adapter_name[a_len] = if (c < 128) @intCast(c) else '?';
                a_len += 1;
            }
            info.adapter_name_len = a_len;

            try list.append(allocator, info);
            global_monitor_id += 1;
        }
    }

    return list.toOwnedSlice(allocator);
}
