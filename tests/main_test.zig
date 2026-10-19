//! Aggregated unit tests for LifeScreen
const std = @import("std");

test {
    _ = @import("config_test.zig");
    _ = @import("i18n_test.zig");
    _ = @import("filename_test.zig");
    _ = @import("ring_buffer_test.zig");
    _ = @import("platform_test.zig");
}
