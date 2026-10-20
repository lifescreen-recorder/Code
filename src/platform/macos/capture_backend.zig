//! Unified macOS Capture Backend Factory.

const std = @import("std");
pub const sck = @import("screencapturekit.zig");
pub const coreaudio = @import("coreaudio.zig");
pub const display_list = @import("display_list.zig");
