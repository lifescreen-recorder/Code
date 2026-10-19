//! Process Injector for LifeScreen Game Capture DLL.
//! Injects lifescreen_hook64.dll into the target game process via CreateRemoteThread.
const std = @import("std");

const log = std.log.scoped(.injector);

extern "kernel32" fn OpenProcess(dwDesiredAccess: u32, bInheritHandle: i32, dwProcessId: u32) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn VirtualAllocEx(hProcess: *anyopaque, lpAddress: ?*anyopaque, dwSize: usize, flAllocationType: u32, flProtect: u32) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn VirtualFreeEx(hProcess: *anyopaque, lpAddress: *anyopaque, dwSize: usize, dwFreeType: u32) callconv(.winapi) i32;
extern "kernel32" fn WriteProcessMemory(hProcess: *anyopaque, lpBaseAddress: *anyopaque, lpBuffer: *const anyopaque, nSize: usize, lpNumberOfBytesWritten: ?*usize) callconv(.winapi) i32;
extern "kernel32" fn CreateRemoteThread(hProcess: *anyopaque, lpThreadAttributes: ?*anyopaque, dwStackSize: usize, lpStartAddress: *const anyopaque, lpParameter: ?*anyopaque, dwCreationFlags: u32, lpThreadId: ?*u32) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn WaitForSingleObject(hHandle: *anyopaque, dwMilliseconds: u32) callconv(.winapi) u32;
extern "kernel32" fn CloseHandle(hObject: *anyopaque) callconv(.winapi) i32;
extern "kernel32" fn GetModuleHandleA(lpModuleName: [*:0]const u8) callconv(.winapi) ?*anyopaque;
extern "kernel32" fn GetProcAddress(hModule: *anyopaque, lpProcName: [*:0]const u8) callconv(.winapi) ?*const anyopaque;
extern "user32" fn GetWindowThreadProcessId(hWnd: *anyopaque, lpdwProcessId: *u32) callconv(.winapi) u32;

const PROCESS_ALL_ACCESS: u32 = 0x001F0FFF;
const PROCESS_CREATE_THREAD: u32 = 0x0002;
const PROCESS_QUERY_INFORMATION: u32 = 0x0400;
const PROCESS_VM_OPERATION: u32 = 0x0008;
const PROCESS_VM_WRITE: u32 = 0x0020;
const PROCESS_VM_READ: u32 = 0x0010;

const MEM_COMMIT: u32 = 0x00001000;
const MEM_RESERVE: u32 = 0x00002000;
const MEM_RELEASE: u32 = 0x00008000;
const PAGE_READWRITE: u32 = 0x04;

pub fn getProcessIdFromHwnd(hwnd: *anyopaque) u32 {
    var pid: u32 = 0;
    _ = GetWindowThreadProcessId(hwnd, &pid);
    return pid;
}

pub fn injectHookDll(pid: u32, dll_path_z: [*:0]const u8) !void {
    if (pid == 0) return error.InvalidProcessId;

    const access_flags = PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ;
    const process_handle = OpenProcess(access_flags, 0, pid) orelse {
        log.warn("Failed to OpenProcess for PID {d}. Administrator privileges might be required.", .{pid});
        return error.OpenProcessFailed;
    };
    defer _ = CloseHandle(process_handle);

    const kernel32 = GetModuleHandleA("kernel32.dll") orelse return error.Kernel32NotFound;
    const load_library = GetProcAddress(kernel32, "LoadLibraryA") orelse return error.LoadLibraryNotFound;

    const path_len = std.mem.len(dll_path_z) + 1;
    const remote_mem = VirtualAllocEx(process_handle, null, path_len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE) orelse {
        log.warn("Failed to VirtualAllocEx in PID {d}", .{pid});
        return error.VirtualAllocFailed;
    };
    defer _ = VirtualFreeEx(process_handle, remote_mem, 0, MEM_RELEASE);

    var written: usize = 0;
    if (WriteProcessMemory(process_handle, remote_mem, dll_path_z, path_len, &written) == 0 or written < path_len) {
        log.warn("Failed to WriteProcessMemory into PID {d}", .{pid});
        return error.WriteProcessMemoryFailed;
    }

    const remote_thread = CreateRemoteThread(process_handle, null, 0, load_library, remote_mem, 0, null) orelse {
        log.warn("Failed to CreateRemoteThread in PID {d}", .{pid});
        return error.CreateRemoteThreadFailed;
    };
    defer _ = CloseHandle(remote_thread);

    // Wait up to 3000 ms for LoadLibraryA to finish in target process
    const wait_result = WaitForSingleObject(remote_thread, 3000);
    if (wait_result != 0) {
        log.warn("Remote thread wait returned {d}", .{wait_result});
        return error.InjectionTimeout;
    }

    log.info("Successfully injected hook DLL into target PID {d}", .{pid});
}
