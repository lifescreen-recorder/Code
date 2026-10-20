//! Aggregated unit tests for LifeScreen
const std = @import("std");
const builtin = @import("builtin");

test {
    _ = @import("config_test.zig");
    _ = @import("i18n_test.zig");
    _ = @import("filename_test.zig");
    _ = @import("ring_buffer_test.zig");
    if (builtin.os.tag == .windows) {
        _ = @import("platform_test.zig");
    }
}
