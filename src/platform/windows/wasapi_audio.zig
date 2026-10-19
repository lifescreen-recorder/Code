//! WASAPI (Windows Audio Session API) loopback audio capture for system sounds.
const std = @import("std");
const types = @import("../../core/types.zig");

pub const HRESULT = i32;
pub const S_OK = 0;

pub const AUDCLNT_STREAMFLAGS_LOOPBACK = 0x00020000;
pub const AUDCLNT_SHAREMODE_SHARED = 0;

// COM GUIDs
pub const CLSID_MMDeviceEnumerator = std.os.windows.GUID{
    .Data1 = 0xbcde0395,
    .Data2 = 0xe52f,
    .Data3 = 0x467c,
    .Data4 = .{ 0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e },
};

pub const IID_IMMDeviceEnumerator = std.os.windows.GUID{
    .Data1 = 0xa95664d2,
    .Data2 = 0x9614,
    .Data3 = 0x4f35,
    .Data4 = .{ 0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6 },
};

pub const IID_IAudioClient = std.os.windows.GUID{
    .Data1 = 0x1cb9ad4c,
    .Data2 = 0xdbfa,
    .Data3 = 0x4c32,
    .Data4 = .{ 0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2 },
};

pub const IID_IAudioCaptureClient = std.os.windows.GUID{
    .Data1 = 0xc8adbd64,
    .Data2 = 0xe71e,
    .Data3 = 0x48a0,
    .Data4 = .{ 0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17 },
};

pub const IID_IAudioMeterInformation = std.os.windows.GUID{
    .Data1 = 0xc02216f6,
    .Data2 = 0x0388,
    .Data3 = 0x4445,
    .Data4 = .{ 0x9a, 0xc5, 0xe7, 0xeb, 0x8c, 0x0b, 0x16, 0x5e },
};

pub const IAudioMeterInformation = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: ?*const anyopaque,
        AddRef: ?*const anyopaque,
        Release: *const fn (self: *IAudioMeterInformation) callconv(.winapi) u32,
        GetPeakValue: *const fn (self: *IAudioMeterInformation, pfPeak: *f32) callconv(.winapi) HRESULT,
        GetMeteringChannelCount: ?*const anyopaque,
        GetChannelsPeakValues: ?*const anyopaque,
        QueryHardwareSupport: ?*const anyopaque,
    };

    pub inline fn release(self: *IAudioMeterInformation) u32 {
        return self.vtable.Release(self);
    }
};

pub const WAVEFORMATEX = extern struct {
    wFormatTag: u16,
    nChannels: u16,
    nSamplesPerSec: u32,
    nAvgBytesPerSec: u32,
    nBlockAlign: u16,
    wBitsPerSample: u16,
    cbSize: u16,
};

pub const WAVEFORMATEXTENSIBLE = extern struct {
    Format: WAVEFORMATEX,
    Samples: extern union {
        wValidBitsPerSample: u16,
        wSamplesPerBlock: u16,
    },
    dwChannelMask: u32,
    SubFormat: std.os.windows.GUID,
};

pub const IAudioCaptureClient = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: ?*const anyopaque,
        AddRef: ?*const anyopaque,
        Release: *const fn (self: *IAudioCaptureClient) callconv(.winapi) u32,
        GetBuffer: *const fn (
            self: *IAudioCaptureClient,
            ppData: *?[*]u8,
            pNumFramesToRead: *u32,
            pdwFlags: *u32,
            pu64DevicePosition: ?*u64,
            pu64QPCPosition: ?*u64,
        ) callconv(.winapi) HRESULT,
        ReleaseBuffer: *const fn (self: *IAudioCaptureClient, NumFramesRead: u32) callconv(.winapi) HRESULT,
        GetNextPacketSize: *const fn (self: *IAudioCaptureClient, pNumFramesInNextPacket: *u32) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IAudioCaptureClient) u32 {
        return self.vtable.Release(self);
    }
};

pub const IAudioClient = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: ?*const anyopaque,
        AddRef: ?*const anyopaque,
        Release: *const fn (self: *IAudioClient) callconv(.winapi) u32,
        Initialize: *const fn (
            self: *IAudioClient,
            ShareMode: u32,
            StreamFlags: u32,
            hnsBufferDuration: i64,
            hnsPeriodicity: i64,
            pFormat: *const WAVEFORMATEX,
            AudioSessionGuid: ?*const std.os.windows.GUID,
        ) callconv(.winapi) HRESULT,
        GetBufferSize: *const fn (self: *IAudioClient, pNumBufferFrames: *u32) callconv(.winapi) HRESULT,
        GetStreamLatency: ?*const anyopaque,
        GetCurrentPadding: ?*const anyopaque,
        IsFormatSupported: ?*const anyopaque,
        GetMixFormat: *const fn (self: *IAudioClient, ppDeviceFormat: *?*WAVEFORMATEX) callconv(.winapi) HRESULT,
        GetDevicePeriod: ?*const anyopaque,
        Start: *const fn (self: *IAudioClient) callconv(.winapi) HRESULT,
        Stop: *const fn (self: *IAudioClient) callconv(.winapi) HRESULT,
        Reset: *const fn (self: *IAudioClient) callconv(.winapi) HRESULT,
        SetEventHandle: ?*const anyopaque,
        GetService: *const fn (self: *IAudioClient, riid: *const std.os.windows.GUID, ppv: *?*anyopaque) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IAudioClient) u32 {
        return self.vtable.Release(self);
    }
};

pub const IMMDevice = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: ?*const anyopaque,
        AddRef: ?*const anyopaque,
        Release: *const fn (self: *IMMDevice) callconv(.winapi) u32,
        Activate: *const fn (
            self: *IMMDevice,
            iid: *const std.os.windows.GUID,
            dwClsCtx: u32,
            pActivationParams: ?*anyopaque,
            ppInterface: *?*anyopaque,
        ) callconv(.winapi) HRESULT,
        OpenPropertyStore: *const fn (
            self: *IMMDevice,
            stgmAccess: u32,
            ppProperties: *?*IPropertyStore,
        ) callconv(.winapi) HRESULT,
        GetId: *const fn (self: *IMMDevice, ppstrId: *?[*:0]u16) callconv(.winapi) HRESULT,
        GetState: *const fn (self: *IMMDevice, pdwState: *u32) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IMMDevice) u32 {
        return self.vtable.Release(self);
    }
};

pub const IMMDeviceCollection = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: ?*const anyopaque,
        AddRef: ?*const anyopaque,
        Release: *const fn (self: *IMMDeviceCollection) callconv(.winapi) u32,
        GetCount: *const fn (self: *IMMDeviceCollection, pcDevices: *u32) callconv(.winapi) HRESULT,
        Item: *const fn (self: *IMMDeviceCollection, nDevice: u32, ppDevice: *?*IMMDevice) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IMMDeviceCollection) u32 {
        return self.vtable.Release(self);
    }
};

pub const PROPERTYKEY = extern struct {
    fmtid: std.os.windows.GUID,
    pid: u32,
};

pub const PROPVARIANT = extern struct {
    vt: u16,
    wReserved1: u16 = 0,
    wReserved2: u16 = 0,
    wReserved3: u16 = 0,
    pwszVal: ?[*:0]u16 = null,
    padding: [8]u8 = undefined,
};

pub const IPropertyStore = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: ?*const anyopaque,
        AddRef: ?*const anyopaque,
        Release: *const fn (self: *IPropertyStore) callconv(.winapi) u32,
        GetCount: ?*const anyopaque,
        GetAt: ?*const anyopaque,
        GetValue: *const fn (self: *IPropertyStore, key: *const PROPERTYKEY, pv: *PROPVARIANT) callconv(.winapi) HRESULT,
        SetValue: ?*const anyopaque,
        Commit: ?*const anyopaque,
    };

    pub inline fn release(self: *IPropertyStore) u32 {
        return self.vtable.Release(self);
    }
};

pub const PKEY_Device_FriendlyName = PROPERTYKEY{
    .fmtid = std.os.windows.GUID{
        .Data1 = 0xa45c254e,
        .Data2 = 0xdf1c,
        .Data3 = 0x4efd,
        .Data4 = .{ 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 },
    },
    .pid = 14,
};

pub const PKEY_Device_DeviceDesc = PROPERTYKEY{
    .fmtid = std.os.windows.GUID{
        .Data1 = 0xa45c254e,
        .Data2 = 0xdf1c,
        .Data3 = 0x4efd,
        .Data4 = .{ 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 },
    },
    .pid = 2,
};

pub const IMMDeviceEnumerator = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: ?*const anyopaque,
        AddRef: ?*const anyopaque,
        Release: *const fn (self: *IMMDeviceEnumerator) callconv(.winapi) u32,
        EnumAudioEndpoints: *const fn (
            self: *IMMDeviceEnumerator,
            dataFlow: u32,
            dwStateMask: u32,
            ppDevices: *?*IMMDeviceCollection,
        ) callconv(.winapi) HRESULT,
        GetDefaultAudioEndpoint: *const fn (
            self: *IMMDeviceEnumerator,
            dataFlow: u32, // 0 = eRender, 1 = eCapture
            role: u32,     // 0 = eConsole, 1 = eMultimedia, 2 = eCommunications
            ppEndpoint: *?*IMMDevice,
        ) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *IMMDeviceEnumerator) u32 {
        return self.vtable.Release(self);
    }
};

pub extern "ole32" fn CoCreateInstance(
    rclsid: *const std.os.windows.GUID,
    pUnkOuter: ?*anyopaque,
    dwClsContext: u32,
    riid: *const std.os.windows.GUID,
    ppv: *?*anyopaque,
) callconv(.winapi) HRESULT;

pub extern "ole32" fn CoInitializeEx(pvReserved: ?*anyopaque, dwCoInit: u32) callconv(.winapi) HRESULT;
pub extern "ole32" fn CoUninitialize() callconv(.winapi) void;
pub extern "ole32" fn CoTaskMemFree(pv: ?*anyopaque) callconv(.winapi) void;
pub extern "ole32" fn PropVariantClear(pvar: *PROPVARIANT) callconv(.winapi) HRESULT;

pub const AudioDevice = struct {
    name: [128]u8 = undefined,
    name_len: usize = 0,

    pub fn getName(self: *const AudioDevice) []const u8 {
        return self.name[0..self.name_len];
    }
};

fn getDefaultEndpoint(enumerator: *IMMDeviceEnumerator, data_flow: u32) ?*IMMDevice {
    var dev: ?*IMMDevice = null;
    // 0 = eConsole (Default audio / game)
    if (enumerator.vtable.GetDefaultAudioEndpoint(enumerator, data_flow, 0, &dev) == S_OK and dev != null) return dev;
    // 2 = eCommunications (Standard Windows headset / mic)
    if (enumerator.vtable.GetDefaultAudioEndpoint(enumerator, data_flow, 2, &dev) == S_OK and dev != null) return dev;
    // 1 = eMultimedia
    if (enumerator.vtable.GetDefaultAudioEndpoint(enumerator, data_flow, 1, &dev) == S_OK and dev != null) return dev;
    return null;
}

pub fn enumerateDevices(allocator: std.mem.Allocator, data_flow: u32) ![]AudioDevice {
    _ = CoInitializeEx(null, 0);

    var enumerator_ptr: ?*anyopaque = null;
    if (CoCreateInstance(&CLSID_MMDeviceEnumerator, null, 1, &IID_IMMDeviceEnumerator, &enumerator_ptr) != S_OK or enumerator_ptr == null) {
        return error.DeviceEnumeratorCreationFailed;
    }
    const enumerator: *IMMDeviceEnumerator = @ptrCast(@alignCast(enumerator_ptr.?));
    defer _ = enumerator.release();

    var col: ?*IMMDeviceCollection = null;
    // dwStateMask = 0x00000009 (DEVICE_STATE_ACTIVE | DEVICE_STATE_UNPLUGGED) to list all available hardware endpoints
    if (enumerator.vtable.EnumAudioEndpoints(enumerator, data_flow, 0x09, &col) != S_OK or col == null) {
        return error.EnumAudioEndpointsFailed;
    }
    defer _ = col.?.release();

    var count: u32 = 0;
    if (col.?.vtable.GetCount(col.?, &count) != S_OK) return error.GetCountFailed;

    var list: std.ArrayList(AudioDevice) = .empty;
    defer list.deinit(allocator);

    for (0..count) |i| {
        var dev: ?*IMMDevice = null;
        if (col.?.vtable.Item(col.?, @intCast(i), &dev) != S_OK or dev == null) continue;
        defer _ = dev.?.release();

        var props: ?*IPropertyStore = null;
        if (dev.?.vtable.OpenPropertyStore(dev.?, 0, &props) != S_OK or props == null) continue;
        defer _ = props.?.release();

        var got_name = false;
        var propvar = std.mem.zeroes(PROPVARIANT);
        if (props.?.vtable.GetValue(props.?, &PKEY_Device_FriendlyName, &propvar) == S_OK) {
            defer _ = PropVariantClear(&propvar);
            if (propvar.vt == 31 and propvar.pwszVal != null) {
                var item = AudioDevice{};
                const slice_u16 = std.mem.span(propvar.pwszVal.?);
                if (std.unicode.utf16LeToUtf8(&item.name, slice_u16)) |len| {
                    if (len > 0) {
                        item.name_len = len;
                        try list.append(allocator, item);
                        got_name = true;
                    }
                } else |_| {}
            }
        }

        if (!got_name) {
            var descvar = std.mem.zeroes(PROPVARIANT);
            if (props.?.vtable.GetValue(props.?, &PKEY_Device_DeviceDesc, &descvar) == S_OK) {
                defer _ = PropVariantClear(&descvar);
                if (descvar.vt == 31 and descvar.pwszVal != null) {
                    var item = AudioDevice{};
                    const slice_u16 = std.mem.span(descvar.pwszVal.?);
                    if (std.unicode.utf16LeToUtf8(&item.name, slice_u16)) |len| {
                        if (len > 0) {
                            item.name_len = len;
                            try list.append(allocator, item);
                        }
                    } else |_| {}
                }
            }
        }
    }

    return list.toOwnedSlice(allocator);
}

pub fn getDevicePeak(data_flow: u32, device_index: ?u32) f32 {
    _ = CoInitializeEx(null, 0);

    var enumerator_ptr: ?*anyopaque = null;
    if (CoCreateInstance(&CLSID_MMDeviceEnumerator, null, 1, &IID_IMMDeviceEnumerator, &enumerator_ptr) != S_OK or enumerator_ptr == null) {
        return 0.0;
    }
    const enumerator: *IMMDeviceEnumerator = @ptrCast(@alignCast(enumerator_ptr.?));
    defer _ = enumerator.release();

    var device: ?*IMMDevice = null;
    if (device_index == null or device_index.? == 0) {
        device = getDefaultEndpoint(enumerator, data_flow);
        if (device == null) return 0.0;
    } else {
        var col: ?*IMMDeviceCollection = null;
        if (enumerator.vtable.EnumAudioEndpoints(enumerator, data_flow, 0x09, &col) != S_OK or col == null) {
            return 0.0;
        }
        defer _ = col.?.release();

        const idx = device_index.? - 1;
        if (col.?.vtable.Item(col.?, idx, &device) != S_OK or device == null) {
            device = getDefaultEndpoint(enumerator, data_flow);
            if (device == null) return 0.0;
        }
    }
    defer _ = device.?.release();

    var meter_ptr: ?*anyopaque = null;
    if (device.?.vtable.Activate(device.?, &IID_IAudioMeterInformation, 1, null, &meter_ptr) != S_OK or meter_ptr == null) {
        return 0.0;
    }
    const meter: *IAudioMeterInformation = @ptrCast(@alignCast(meter_ptr.?));
    defer _ = meter.release();

    var peak: f32 = 0.0;
    if (meter.vtable.GetPeakValue(meter, &peak) == S_OK) {
        return peak;
    }
    return 0.0;
}

pub const WasapiCapture = struct {
    client: *IAudioClient,
    capture_client: *IAudioCaptureClient,
    sample_rate: u32,
    channels: u16,
    bits_per_sample: u16,
    is_float: bool,
    volume: f32 = 1.0,
    muted: bool = false,

    pub fn initEndpoint(data_flow: u32, device_index: ?u32) !WasapiCapture {
        _ = CoInitializeEx(null, 0); // COINIT_MULTITHREADED

        var enumerator_ptr: ?*anyopaque = null;
        if (CoCreateInstance(&CLSID_MMDeviceEnumerator, null, 1, &IID_IMMDeviceEnumerator, &enumerator_ptr) != S_OK or enumerator_ptr == null) {
            return error.DeviceEnumeratorCreationFailed;
        }
        const enumerator: *IMMDeviceEnumerator = @ptrCast(@alignCast(enumerator_ptr.?));
        defer _ = enumerator.release();

        var device: ?*IMMDevice = null;
        if (device_index == null or device_index.? == 0) {
            device = getDefaultEndpoint(enumerator, data_flow);
            if (device == null) {
                return error.DefaultAudioEndpointNotFound;
            }
        } else {
            var col: ?*IMMDeviceCollection = null;
            if (enumerator.vtable.EnumAudioEndpoints(enumerator, data_flow, 0x09, &col) != S_OK or col == null) {
                return error.EnumAudioEndpointsFailed;
            }
            defer _ = col.?.release();

            const idx = device_index.? - 1;
            if (col.?.vtable.Item(col.?, idx, &device) != S_OK or device == null) {
                device = getDefaultEndpoint(enumerator, data_flow);
                if (device == null) {
                    return error.DefaultAudioEndpointNotFound;
                }
            }
        }
        defer _ = device.?.release();

        var client_ptr: ?*anyopaque = null;
        if (device.?.vtable.Activate(device.?, &IID_IAudioClient, 1, null, &client_ptr) != S_OK or client_ptr == null) {
            return error.AudioClientActivationFailed;
        }
        const client: *IAudioClient = @ptrCast(@alignCast(client_ptr.?));
        errdefer _ = client.release();

        var format_ptr: ?*WAVEFORMATEX = null;
        if (client.vtable.GetMixFormat(client, &format_ptr) != S_OK or format_ptr == null) {
            return error.GetMixFormatFailed;
        }
        defer CoTaskMemFree(format_ptr);

        const mix_format = format_ptr.?;
        const sample_rate = mix_format.nSamplesPerSec;
        const channels = mix_format.nChannels;
        const bits_per_sample = mix_format.wBitsPerSample;
        const is_float = (mix_format.wFormatTag == 3 or (mix_format.wFormatTag == 0xFFFE and mix_format.wBitsPerSample == 32));

        const buffer_duration_hns: i64 = 1_000_000;
        const stream_flags: u32 = if (data_flow == 0) AUDCLNT_STREAMFLAGS_LOOPBACK else 0;
        const hr_init = client.vtable.Initialize(
            client,
            AUDCLNT_SHAREMODE_SHARED,
            stream_flags,
            buffer_duration_hns,
            0,
            mix_format,
            null,
        );
        if (hr_init != S_OK) return error.AudioClientInitFailed;

        var capture_client_ptr: ?*anyopaque = null;
        if (client.vtable.GetService(client, &IID_IAudioCaptureClient, &capture_client_ptr) != S_OK or capture_client_ptr == null) {
            return error.AudioCaptureClientServiceFailed;
        }
        const capture_client: *IAudioCaptureClient = @ptrCast(@alignCast(capture_client_ptr.?));

        _ = client.vtable.Start(client);

        return .{
            .client = client,
            .capture_client = capture_client,
            .sample_rate = sample_rate,
            .channels = channels,
            .bits_per_sample = bits_per_sample,
            .is_float = is_float,
        };
    }

    pub fn initLoopback(device_index: ?u32) !WasapiCapture {
        return initEndpoint(0, device_index);
    }

    pub fn initCapture(device_index: ?u32) !WasapiCapture {
        return initEndpoint(1, device_index);
    }

    /// Reads available audio samples and converts/scales into the output float buffer.
    /// GUARANTEED to output interleaved stereo (2 floats per frame) regardless of whether
    /// the source is mono, stereo, or multi-channel. Returns number of float samples written (frames * 2).
    pub fn readSamples(self: *WasapiCapture, out_samples: []f32, peak_out: *f32) u32 {
        var packet_size: u32 = 0;
        if (self.capture_client.vtable.GetNextPacketSize(self.capture_client, &packet_size) != S_OK or packet_size == 0) {
            return 0;
        }

        var samples_written: u32 = 0;
        var max_peak: f32 = 0.0;

        while (true) {
            var data_ptr: ?[*]u8 = null;
            var num_frames: u32 = 0;
            var flags: u32 = 0;

            const hr = self.capture_client.vtable.GetBuffer(
                self.capture_client,
                &data_ptr,
                &num_frames,
                &flags,
                null,
                null,
            );

            if (hr != S_OK or num_frames == 0 or data_ptr == null) break;

            const is_silent = (flags & 0x1) != 0 or self.muted;
            const vol = if (is_silent) 0.0 else self.volume;

            if (self.channels == 1) {
                // Mono source: duplicate each mono sample into Left and Right stereo channels
                for (0..num_frames) |f| {
                    if (samples_written + 2 > out_samples.len) break;
                    var sample: f32 = 0.0;
                    if (self.is_float) {
                        const float_ptr: [*]const f32 = @ptrCast(@alignCast(data_ptr.?));
                        sample = float_ptr[f] * vol;
                    } else if (self.bits_per_sample == 16) {
                        const s16_ptr: [*]const i16 = @ptrCast(@alignCast(data_ptr.?));
                        sample = (@as(f32, @floatFromInt(s16_ptr[f])) / 32768.0) * vol;
                    } else if (self.bits_per_sample == 24) {
                        const base_idx = f * 3;
                        const b0: u32 = data_ptr.?[base_idx];
                        const b1: u32 = data_ptr.?[base_idx + 1];
                        const b2: u32 = data_ptr.?[base_idx + 2];
                        var val: i32 = @bitCast((b0) | (b1 << 8) | (b2 << 16));
                        if ((val & 0x800000) != 0) val |= @bitCast(@as(u32, 0xFF000000));
                        sample = (@as(f32, @floatFromInt(val)) / 8388608.0) * vol;
                    } else if (self.bits_per_sample == 32) {
                        const s32_ptr: [*]const i32 = @ptrCast(@alignCast(data_ptr.?));
                        sample = (@as(f32, @floatFromInt(s32_ptr[f])) / 2147483648.0) * vol;
                    }

                    out_samples[samples_written] = sample;
                    out_samples[samples_written + 1] = sample;
                    samples_written += 2;

                    const abs_s = @abs(sample);
                    if (abs_s > max_peak) max_peak = abs_s;
                }
            } else {
                // Stereo or multi-channel: extract Left and Right
                const ch = self.channels;
                for (0..num_frames) |f| {
                    if (samples_written + 2 > out_samples.len) break;
                    var left_sample: f32 = 0.0;
                    var right_sample: f32 = 0.0;

                    if (self.is_float) {
                        const float_ptr: [*]const f32 = @ptrCast(@alignCast(data_ptr.?));
                        left_sample = float_ptr[f * ch] * vol;
                        right_sample = float_ptr[f * ch + 1] * vol;
                    } else if (self.bits_per_sample == 16) {
                        const s16_ptr: [*]const i16 = @ptrCast(@alignCast(data_ptr.?));
                        left_sample = (@as(f32, @floatFromInt(s16_ptr[f * ch])) / 32768.0) * vol;
                        right_sample = (@as(f32, @floatFromInt(s16_ptr[f * ch + 1])) / 32768.0) * vol;
                    } else if (self.bits_per_sample == 24) {
                        const b_idx_l = (f * ch) * 3;
                        const b_idx_r = (f * ch + 1) * 3;
                        var val_l: i32 = @bitCast(@as(u32, data_ptr.?[b_idx_l]) | (@as(u32, data_ptr.?[b_idx_l + 1]) << 8) | (@as(u32, data_ptr.?[b_idx_l + 2]) << 16));
                        if ((val_l & 0x800000) != 0) val_l |= @bitCast(@as(u32, 0xFF000000));
                        var val_r: i32 = @bitCast(@as(u32, data_ptr.?[b_idx_r]) | (@as(u32, data_ptr.?[b_idx_r + 1]) << 8) | (@as(u32, data_ptr.?[b_idx_r + 2]) << 16));
                        if ((val_r & 0x800000) != 0) val_r |= @bitCast(@as(u32, 0xFF000000));
                        left_sample = (@as(f32, @floatFromInt(val_l)) / 8388608.0) * vol;
                        right_sample = (@as(f32, @floatFromInt(val_r)) / 8388608.0) * vol;
                    } else if (self.bits_per_sample == 32) {
                        const s32_ptr: [*]const i32 = @ptrCast(@alignCast(data_ptr.?));
                        left_sample = (@as(f32, @floatFromInt(s32_ptr[f * ch])) / 2147483648.0) * vol;
                        right_sample = (@as(f32, @floatFromInt(s32_ptr[f * ch + 1])) / 2147483648.0) * vol;
                    }

                    out_samples[samples_written] = left_sample;
                    out_samples[samples_written + 1] = right_sample;
                    samples_written += 2;

                    const abs_l = @abs(left_sample);
                    const abs_r = @abs(right_sample);
                    if (abs_l > max_peak) max_peak = abs_l;
                    if (abs_r > max_peak) max_peak = abs_r;
                }
            }

            _ = self.capture_client.vtable.ReleaseBuffer(self.capture_client, num_frames);

            if (samples_written >= out_samples.len) break;
            if (self.capture_client.vtable.GetNextPacketSize(self.capture_client, &packet_size) != S_OK or packet_size == 0) break;
        }

        peak_out.* = @min(max_peak, 1.0);
        return samples_written;
    }

    pub fn deinit(self: *WasapiCapture) void {
        _ = self.client.vtable.Stop(self.client);
        _ = self.capture_client.release();
        _ = self.client.release();
        CoUninitialize();
    }
};
