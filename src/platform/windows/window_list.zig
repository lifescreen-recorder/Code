//! Window enumeration and filtering for Windows (Win32 & DWM).
const std = @import("std");

pub const HWND = ?*anyopaque;
pub const BOOL = i32;
pub const LPARAM = isize;
pub const DWORD = u32;

pub const RECT = extern struct {
    left: i32,
    top: i32,
    right: i32,
    bottom: i32,
};

pub const WindowInfo = struct {
    hwnd: usize,
    title: [256]u8 = std.mem.zeroes([256]u8),
    title_len: usize = 0,
    process_name: [128]u8 = std.mem.zeroes([128]u8),
    process_name_len: usize = 0,
    pid: u32 = 0,
    x: i32 = 0,
    y: i32 = 0,
    width: u32 = 0,
    height: u32 = 0,

    pub fn getTitle(self: *const WindowInfo) []const u8 {
        return self.title[0..self.title_len];
    }

    pub fn getProcessName(self: *const WindowInfo) []const u8 {
        return self.process_name[0..self.process_name_len];
    }
};

// Win32 External declarations
extern "user32" fn EnumWindows(lpEnumFunc: *const fn (hwnd: HWND, lParam: LPARAM) callconv(.winapi) BOOL, lParam: LPARAM) callconv(.winapi) BOOL;
extern "user32" fn IsWindowVisible(hWnd: HWND) callconv(.winapi) BOOL;
extern "user32" fn IsIconic(hWnd: HWND) callconv(.winapi) BOOL;
extern "user32" fn GetWindowTextW(hWnd: HWND, lpString: [*]u16, nMaxCount: c_int) callconv(.winapi) c_int;
extern "user32" fn GetWindowRect(hWnd: HWND, lpRect: *RECT) callconv(.winapi) BOOL;
extern "user32" fn GetWindowThreadProcessId(hWnd: HWND, lpdwProcessId: ?*DWORD) callconv(.winapi) DWORD;
extern "user32" fn GetWindowLongW(hWnd: HWND, nIndex: c_int) callconv(.winapi) i32;
extern "user32" fn GetClassNameW(hWnd: HWND, lpClassName: [*]u16, nMaxCount: c_int) callconv(.winapi) c_int;

extern "kernel32" fn OpenProcess(dwDesiredAccess: DWORD, bInheritHandle: BOOL, dwProcessId: DWORD) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn CloseHandle(hObject: ?*anyopaque) callconv(.winapi) BOOL;
extern "kernel32" fn QueryFullProcessImageNameW(hProcess: ?*anyopaque, dwFlags: DWORD, lpExeName: [*]u16, lpdwSize: *DWORD) callconv(.winapi) BOOL;

extern "kernel32" fn LoadLibraryA(lpLibFileName: [*:0]const u8) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn GetProcAddress(hModule: ?*anyopaque, lpProcName: [*:0]const u8) callconv(.winapi) ?*anyopaque;

const GWL_EXSTYLE = -20;
const WS_EX_TOOLWINDOW = 0x00000080;
const PROCESS_QUERY_LIMITED_INFORMATION = 0x1000;
const DWMWA_CLOAKED = 14;

const EnumContext = struct {
    allocator: std.mem.Allocator,
    windows: std.ArrayListUnmanaged(WindowInfo),
    pfnDwmGetWindowAttribute: ?*const fn (hwnd: HWND, dwAttribute: DWORD, pvAttribute: *anyopaque, cbAttribute: DWORD) callconv(.winapi) i32,
};

fn enumWindowsProc(hwnd: HWND, lParam: LPARAM) callconv(.winapi) BOOL {
    const ctx: *EnumContext = @ptrFromInt(@as(usize, @bitCast(lParam)));

    if (hwnd == null) return 1;
    if (IsWindowVisible(hwnd) == 0) return 1;

    // Filter out tool windows
    const ex_style = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if ((ex_style & WS_EX_TOOLWINDOW) != 0) return 1;

    // Check window rectangle
    var rect: RECT = undefined;
    if (GetWindowRect(hwnd, &rect) == 0) return 1;
    const width = rect.right - rect.left;
    const height = rect.bottom - rect.top;
    if (width <= 16 or height <= 16) return 1;

    // Check class name (ignore Program Manager, Shell tray, etc.)
    var class_name_w: [128]u16 = undefined;
    const class_len = GetClassNameW(hwnd, &class_name_w, class_name_w.len);
    if (class_len > 0) {
        var class_buf: [128]u8 = undefined;
        var clen: usize = 0;
        for (class_name_w[0..@intCast(class_len)]) |c| {
            if (clen >= class_buf.len) break;
            class_buf[clen] = if (c < 128) @intCast(c) else '?';
            clen += 1;
        }
        const cls = class_buf[0..clen];
        if (std.mem.eql(u8, cls, "Progman") or
            std.mem.eql(u8, cls, "Shell_TrayWnd") or
            std.mem.eql(u8, cls, "Windows.UI.Core.CoreWindow"))
        {
            return 1;
        }
    }

    // Check cloaked state (hidden UWP apps or apps on other virtual desktops)
    if (ctx.pfnDwmGetWindowAttribute) |dwmGetAttr| {
        var cloaked: u32 = 0;
        const hr = dwmGetAttr(hwnd, DWMWA_CLOAKED, &cloaked, @sizeOf(u32));
        if (hr == 0 and cloaked != 0) return 1;
    }

    // Get window title
    var title_w: [256]u16 = undefined;
    const title_len = GetWindowTextW(hwnd, &title_w, title_w.len);
    if (title_len <= 0) return 1;

    var win_info: WindowInfo = .{
        .hwnd = @intFromPtr(hwnd.?),
        .x = rect.left,
        .y = rect.top,
        .width = @intCast(width),
        .height = @intCast(height),
    };

    // Convert title from UTF-16 to UTF-8
    const slice_w = title_w[0..@intCast(title_len)];
    const converted_len = std.unicode.utf16LeToUtf8(&win_info.title, slice_w) catch blk: {
        // Fallback ASCII conversion if invalid UTF-16
        var i: usize = 0;
        for (slice_w) |c| {
            if (i >= win_info.title.len) break;
            win_info.title[i] = if (c < 128) @intCast(c) else '?';
            i += 1;
        }
        break :blk i;
    };
    win_info.title_len = converted_len;

    // Get PID & Process image name
    var pid: DWORD = 0;
    _ = GetWindowThreadProcessId(hwnd, &pid);
    win_info.pid = pid;

    if (pid != 0) {
        const hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, 0, pid);
        if (hProc) |proc| {
            defer _ = CloseHandle(proc);
            var exe_w: [260]u16 = undefined;
            var size: DWORD = exe_w.len;
            if (QueryFullProcessImageNameW(proc, 0, &exe_w, &size) != 0 and size > 0) {
                // Find basename of exe
                var last_slash: usize = 0;
                for (exe_w[0..size], 0..) |c, idx| {
                    if (c == '\\' or c == '/') last_slash = idx + 1;
                }
                const name_slice_w = exe_w[last_slash..size];
                const proc_len = std.unicode.utf16LeToUtf8(&win_info.process_name, name_slice_w) catch blk: {
                    var i: usize = 0;
                    for (name_slice_w) |c| {
                        if (i >= win_info.process_name.len) break;
                        win_info.process_name[i] = if (c < 128) @intCast(c) else '?';
                        i += 1;
                    }
                    break :blk i;
                };
                win_info.process_name_len = proc_len;
            }
        }
    }

    ctx.windows.append(ctx.allocator, win_info) catch return 0;
    return 1;
}

/// Enumerate all visible, interactive top-level application windows.
pub fn enumerateWindows(allocator: std.mem.Allocator) ![]WindowInfo {
    var pfnDwm: ?*const fn (hwnd: HWND, dwAttribute: DWORD, pvAttribute: *anyopaque, cbAttribute: DWORD) callconv(.winapi) i32 = null;
    const hDwmapi = LoadLibraryA("dwmapi.dll");
    if (hDwmapi) |hMod| {
        if (GetProcAddress(hMod, "DwmGetWindowAttribute")) |proc| {
            pfnDwm = @ptrCast(proc);
        }
    }

    var ctx = EnumContext{
        .allocator = allocator,
        .windows = .empty,
        .pfnDwmGetWindowAttribute = pfnDwm,
    };
    errdefer ctx.windows.deinit(allocator);

    const lparam: LPARAM = @bitCast(@intFromPtr(&ctx));
    _ = EnumWindows(enumWindowsProc, lparam);

    return ctx.windows.toOwnedSlice(allocator);
}
