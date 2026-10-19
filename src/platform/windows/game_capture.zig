//! High-Performance Game Capture Backend.
//! Zero-copy GPU texture acquisition from target game via D3D11 shared handles and hook DLL IPC.
const std = @import("std");
const types = @import("../../core/types.zig");
const d3d11 = @import("d3d11_utils.zig");
const clock = @import("../../core/clock.zig");
const injector = @import("hook/injector.zig");

const log = std.log.scoped(.game_capture);

pub const LifeScreenHookIpc = extern struct {
    magic: u32,
    version: u32,
    process_id: u32,
    width: u32,
    height: u32,
    format: u32,
    shared_handle: u64,
    frame_index: u64,
    timestamp_qpc: u64,
    active: bool,
    recording: bool,
};

extern "kernel32" fn OpenFileMappingA(dwDesiredAccess: u32, bInheritHandle: i32, lpName: [*:0]const u8) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn MapViewOfFile(hFileMappingObject: *anyopaque, dwDesiredAccess: u32, dwFileOffsetHigh: u32, dwFileOffsetLow: u32, dwNumberOfBytesToMap: usize) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn UnmapViewOfFile(lpBaseAddress: *const anyopaque) callconv(.winapi) i32;
extern "kernel32" fn OpenEventA(dwDesiredAccess: u32, bInheritHandle: i32, lpName: [*:0]const u8) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn WaitForSingleObject(hHandle: *anyopaque, dwMilliseconds: u32) callconv(.winapi) u32;
extern "kernel32" fn CloseHandle(hObject: *anyopaque) callconv(.winapi) i32;

const FILE_MAP_ALL_ACCESS: u32 = 0xF001F;
const SYNCHRONIZE: u32 = 0x00100000;
const WAIT_OBJECT_0: u32 = 0;

pub const GameCapture = struct {
    d3d_ctx: d3d11.D3D11Context,
    map_handle: ?*anyopaque = null,
    ipc_ptr: ?*LifeScreenHookIpc = null,
    frame_event: ?*anyopaque = null,
    shared_texture: ?*d3d11.ID3D11Texture2D = null,
    current_shared_handle: u64 = 0,
    width: u32 = 0,
    height: u32 = 0,
    frame_index: u64 = 0,
    pid: u32 = 0,

    pub fn init(pid: u32, timeout_ms: u32) !GameCapture {
        if (pid == 0) return error.InvalidProcessId;

        var self = GameCapture{
            .d3d_ctx = try d3d11.D3D11Context.init(),
            .pid = pid,
        };
        errdefer self.deinit();

        var map_name_buf: [128]u8 = undefined;
        const map_name_slice = try std.fmt.bufPrint(&map_name_buf, "LifeScreen_GameHook_IPC_{d}\x00", .{pid});
        const map_name_z: [*:0]const u8 = @ptrCast(map_name_slice.ptr);

        var event_name_buf: [128]u8 = undefined;
        const event_name_slice = try std.fmt.bufPrint(&event_name_buf, "LifeScreen_GameHook_Frame_{d}\x00", .{pid});
        const event_name_z: [*:0]const u8 = @ptrCast(event_name_slice.ptr);

        // Try opening existing IPC (in case hook was already injected)
        self.map_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, 0, map_name_z);
        if (self.map_handle == null) {
            // Attempt to inject DLL into target process
            const dll_name_z: [*:0]const u8 = "lifescreen_hook64.dll";
            log.info("Injecting hook DLL into PID {d}...", .{pid});
            injector.injectHookDll(pid, dll_name_z) catch |err| {
                log.warn("Injection failed: {s}", .{@errorName(err)});
                return err;
            };

            // Wait briefly for target process to load DLL and initialize IPC
            clock.sleepMs(200);
            self.map_handle = OpenFileMappingA(FILE_MAP_ALL_ACCESS, 0, map_name_z);
        }

        if (self.map_handle == null) {
            log.warn("Could not open IPC mapping for PID {d}", .{pid});
            return error.HookIpcNotFound;
        }

        const view = MapViewOfFile(self.map_handle.?, FILE_MAP_ALL_ACCESS, 0, 0, @sizeOf(LifeScreenHookIpc));
        if (view == null) return error.MapViewFailed;
        self.ipc_ptr = @alignCast(@ptrCast(view.?));

        self.frame_event = OpenEventA(SYNCHRONIZE, 0, event_name_z);
        if (self.frame_event == null) return error.OpenEventFailed;

        // Signal to game hook that LifeScreen is ready and recording
        self.ipc_ptr.?.recording = true;

        // Wait up to timeout_ms for hook to produce its first shared texture
        const start_ms = clock.MonotonicClock.nowNanos() / 1_000_000;
        while (true) {
            if (self.ipc_ptr.?.shared_handle != 0 and self.ipc_ptr.?.active) {
                break;
            }
            const now_ms = clock.MonotonicClock.nowNanos() / 1_000_000;
            if (now_ms - start_ms >= timeout_ms) {
                log.warn("Timed out waiting for game hook shared texture in PID {d}", .{pid});
                return error.GameHookTimeout;
            }
            clock.sleepMs(10);
        }

        try self.reopenSharedTexture();

        log.info("Game capture initialized successfully for PID {d}: {d}x{d}, handle=0x{x}", .{
            pid,
            self.width,
            self.height,
            self.current_shared_handle,
        });

        return self;
    }

    fn reopenSharedTexture(self: *GameCapture) !void {
        if (self.shared_texture) |tex| {
            _ = tex.release();
            self.shared_texture = null;
        }

        const handle_val = self.ipc_ptr.?.shared_handle;
        if (handle_val == 0) return error.InvalidSharedHandle;

        const shared_handle: ?*anyopaque = @ptrFromInt(handle_val);
        const tex = try self.d3d_ctx.device.openSharedResource(shared_handle);

        self.shared_texture = tex;
        self.current_shared_handle = handle_val;
        self.width = self.ipc_ptr.?.width;
        self.height = self.ipc_ptr.?.height;
    }

    pub fn acquireFrame(self: *GameCapture, timeout_ms: u32, pts_nanos: u64) !?types.VideoFrame {
        if (self.frame_event == null or self.ipc_ptr == null) return null;

        const wait_res = WaitForSingleObject(self.frame_event.?, timeout_ms);
        if (wait_res != WAIT_OBJECT_0) {
            return null; // Timeout / skipped frame
        }

        // Check if game resized swapchain or recreated shared texture
        if (self.ipc_ptr.?.shared_handle != self.current_shared_handle) {
            log.info("Game texture resized to {d}x{d}, reopening shared handle...", .{
                self.ipc_ptr.?.width,
                self.ipc_ptr.?.height,
            });
            try self.reopenSharedTexture();
        }

        const tex = self.shared_texture orelse return null;
        self.frame_index += 1;

        return types.VideoFrame{
            .width = self.width,
            .height = self.height,
            .format = .bgra32,
            .pts_nanos = pts_nanos,
            .frame_index = self.frame_index,
            .gpu_texture_ptr = @ptrCast(tex),
            .gpu_shared_handle = @intCast(self.current_shared_handle),
            .cpu_data = null,
        };
    }

    pub fn deinit(self: *GameCapture) void {
        if (self.ipc_ptr) |ipc| {
            ipc.recording = false;
            _ = UnmapViewOfFile(@ptrCast(ipc));
            self.ipc_ptr = null;
        }
        if (self.map_handle) |h| {
            _ = CloseHandle(h);
            self.map_handle = null;
        }
        if (self.frame_event) |h| {
            _ = CloseHandle(h);
            self.frame_event = null;
        }
        if (self.shared_texture) |tex| {
            _ = tex.release();
            self.shared_texture = null;
        }
        self.d3d_ctx.deinit();
    }
};
