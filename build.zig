const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const is_windows = target.result.os.tag == .windows;
    const is_linux = target.result.os.tag == .linux;
    const is_darwin = target.result.os.tag.isDarwin();

    // 1. Core LifeScreen module
    const lifescreen_mod = b.addModule("lifescreen", .{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    if (is_windows) {
        lifescreen_mod.addIncludePath(b.path("vendor/ffmpeg/include"));
        lifescreen_mod.addLibraryPath(b.path("vendor/ffmpeg/lib"));
    }
    lifescreen_mod.addIncludePath(b.path("src/encoders"));

    // 2. Main executable
    const exe = b.addExecutable(.{
        .name = "lifescreen",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .imports = &.{
                .{ .name = "lifescreen", .module = lifescreen_mod },
            },
        }),
    });

    // Add C bridges and FFmpeg options to root_module
    if (is_windows) {
        exe.root_module.addCSourceFile(.{
            .file = b.path("src/encoders/ffmpeg_bridge.c"),
            .flags = &[_][]const u8{ "-std=c11", "-O3" },
        });
    } else {
        exe.root_module.addCSourceFile(.{
            .file = b.path("src/encoders/ffmpeg_bridge_posix.c"),
            .flags = &[_][]const u8{ "-std=c11", "-O3" },
        });
    }
    exe.root_module.addIncludePath(b.path("src/encoders"));

    if (is_windows) {
        exe.root_module.addCSourceFile(.{
            .file = b.path("src/platform/windows/wgc_bridge.c"),
            .flags = &[_][]const u8{ "-std=c11", "-O2" },
        });

        const imgui_flags = &[_][]const u8{ "-std=c++17", "-O2" };
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/imgui/imgui.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/imgui/imgui_demo.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/imgui/imgui_draw.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/imgui/imgui_tables.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/imgui/imgui_widgets.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/imgui/backends/imgui_impl_win32.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/imgui/backends/imgui_impl_dx11.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("vendor/cimgui/cimgui.cpp"), .flags = imgui_flags });
        exe.root_module.addCSourceFile(.{ .file = b.path("src/ui/imgui_bridge.cpp"), .flags = imgui_flags });

        exe.root_module.addIncludePath(b.path("vendor/ffmpeg/include"));
        exe.root_module.addIncludePath(b.path("src/platform/windows"));
        exe.root_module.addIncludePath(b.path("vendor/cimgui"));
        exe.root_module.addIncludePath(b.path("vendor/cimgui/imgui"));
        exe.root_module.addIncludePath(b.path("vendor/cimgui/imgui/backends"));
        exe.root_module.addIncludePath(b.path("src/ui"));
        exe.root_module.addLibraryPath(b.path("vendor/ffmpeg/lib"));

        exe.root_module.link_libcpp = true;

        exe.root_module.linkSystemLibrary("avcodec", .{});
        exe.root_module.linkSystemLibrary("avformat", .{});
        exe.root_module.linkSystemLibrary("avutil", .{});
        exe.root_module.linkSystemLibrary("swresample", .{});
        exe.root_module.linkSystemLibrary("swscale", .{});

        // Windows system graphics, UI, and audio libraries
        exe.root_module.linkSystemLibrary("d3d11", .{});
        exe.root_module.linkSystemLibrary("dxgi", .{});
        exe.root_module.linkSystemLibrary("d3dcompiler_47", .{});
        exe.root_module.linkSystemLibrary("imm32", .{});
        exe.root_module.linkSystemLibrary("dwmapi", .{});
        exe.root_module.linkSystemLibrary("gdi32", .{});
        exe.root_module.linkSystemLibrary("ole32", .{});
        exe.root_module.linkSystemLibrary("kernel32", .{});
        exe.root_module.linkSystemLibrary("winmm", .{});

        // 2b. Game Hook 64-bit DLL (Windows only)
        const hook_dll = b.addLibrary(.{
            .name = "lifescreen_hook64",
            .linkage = .dynamic,
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
                .link_libc = true,
            }),
        });
        hook_dll.root_module.link_libcpp = true;
        hook_dll.root_module.addCSourceFile(.{
            .file = b.path("src/platform/windows/hook/hook_main.cpp"),
            .flags = &[_][]const u8{ "-std=c++17", "-O2" },
        });
        hook_dll.root_module.addIncludePath(b.path("src/platform/windows/hook"));
        hook_dll.root_module.linkSystemLibrary("d3d11", .{});
        hook_dll.root_module.linkSystemLibrary("d3d12", .{});
        hook_dll.root_module.linkSystemLibrary("dxgi", .{});
        hook_dll.root_module.linkSystemLibrary("kernel32", .{});
        hook_dll.root_module.linkSystemLibrary("user32", .{});

        b.installArtifact(hook_dll);
    } else if (is_linux) {
        // Linux platform linking
        exe.root_module.linkSystemLibrary("c", .{});
        exe.root_module.linkSystemLibrary("m", .{});
    } else if (is_darwin) {
        // macOS platform linking
        exe.root_module.linkSystemLibrary("c", .{});
    }

    b.installArtifact(exe);

    // Run step
    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    run_cmd.setCwd(b.path("zig-out/bin"));
    run_cmd.addPassthruArgs();

    const run_step = b.step("run", "Run LifeScreen");
    run_step.dependOn(&run_cmd.step);

    // 3. Unit tests step
    const unit_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("tests/main_test.zig"),
            .target = target,
            .optimize = optimize,
            .link_libc = true,
            .imports = &.{
                .{ .name = "lifescreen", .module = lifescreen_mod },
            },
        }),
    });

    if (is_windows) {
        unit_tests.root_module.addCSourceFile(.{
            .file = b.path("src/platform/windows/wgc_bridge.c"),
            .flags = &[_][]const u8{ "-std=c11", "-O2" },
        });
        unit_tests.root_module.addIncludePath(b.path("src/platform/windows"));

        unit_tests.root_module.linkSystemLibrary("d3d11", .{});
        unit_tests.root_module.linkSystemLibrary("dxgi", .{});
        unit_tests.root_module.linkSystemLibrary("ole32", .{});
        unit_tests.root_module.linkSystemLibrary("kernel32", .{});
        unit_tests.root_module.linkSystemLibrary("user32", .{});
        unit_tests.root_module.linkSystemLibrary("gdi32", .{});
        unit_tests.root_module.linkSystemLibrary("winmm", .{});
    }

    const run_unit_tests = b.addRunArtifact(unit_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_unit_tests.step);
}
