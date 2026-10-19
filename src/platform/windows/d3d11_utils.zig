//! Direct3D 11 device and texture utilities for hardware-accelerated Zero-Copy capture.
const std = @import("std");

pub const HRESULT = i32;
pub const S_OK = 0;
pub const DXGI_ERROR_WAIT_TIMEOUT = @as(HRESULT, @bitCast(@as(u32, 0x887A0027)));
pub const DXGI_ERROR_ACCESS_LOST = @as(HRESULT, @bitCast(@as(u32, 0x887A0026)));

// Direct3D 11 Constants
pub const D3D11_SDK_VERSION = 7;
pub const D3D_DRIVER_TYPE_HARDWARE = 1;
pub const D3D11_CREATE_DEVICE_BGRA_SUPPORT = 0x20;
pub const D3D11_USAGE_DEFAULT = 0;
pub const D3D11_USAGE_STAGING = 3;
pub const D3D11_CPU_ACCESS_READ = 0x20000;
pub const D3D11_BIND_RENDER_TARGET = 0x20;
pub const D3D11_BIND_SHADER_RESOURCE = 0x8;

pub const DXGI_FORMAT_B8G8R8A8_UNORM = 87;
pub const DXGI_FORMAT_R8G8B8A8_UNORM = 28;
pub const DXGI_FORMAT_NV12 = 103;

pub const D3D_FEATURE_LEVEL = enum(u32) {
    level_11_1 = 0xb100,
    level_11_0 = 0xb000,
    level_10_1 = 0xa100,
    level_10_0 = 0xa000,
};

pub const D3D11_TEXTURE2D_DESC = extern struct {
    Width: u32,
    Height: u32,
    MipLevels: u32 = 1,
    ArraySize: u32 = 1,
    Format: u32 = DXGI_FORMAT_B8G8R8A8_UNORM,
    SampleDesc: extern struct {
        Count: u32 = 1,
        Quality: u32 = 0,
    } = .{},
    Usage: u32 = D3D11_USAGE_DEFAULT,
    BindFlags: u32 = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
    CPUAccessFlags: u32 = 0,
    MiscFlags: u32 = 0x2, // D3D11_RESOURCE_MISC_SHARED or 0
};

// COM Interface definitions
pub const IUnknown = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        QueryInterface: *const fn (self: *IUnknown, riid: *const std.os.windows.GUID, ppvObject: *?*anyopaque) callconv(.winapi) HRESULT,
        AddRef: *const fn (self: *IUnknown) callconv(.winapi) u32,
        Release: *const fn (self: *IUnknown) callconv(.winapi) u32,
    };

    pub inline fn release(self: *IUnknown) u32 {
        return self.vtable.Release(self);
    }
};

pub const ID3D11Resource = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        base: IUnknown.VTable,
        GetType: *const fn (self: *ID3D11Resource, pType: *u32) callconv(.winapi) void,
        SetEvictionPriority: *const fn (self: *ID3D11Resource, EvictionPriority: u32) callconv(.winapi) void,
        GetEvictionPriority: *const fn (self: *ID3D11Resource) callconv(.winapi) u32,
    };

    pub inline fn release(self: *ID3D11Resource) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

pub const ID3D11Texture2D = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        base: ID3D11Resource.VTable,
        GetDesc: *const fn (self: *ID3D11Texture2D, pDesc: *D3D11_TEXTURE2D_DESC) callconv(.winapi) void,
    };

    pub inline fn release(self: *ID3D11Texture2D) u32 {
        return self.vtable.base.base.Release(@ptrCast(self));
    }

    pub inline fn getDesc(self: *ID3D11Texture2D) D3D11_TEXTURE2D_DESC {
        var desc: D3D11_TEXTURE2D_DESC = undefined;
        self.vtable.GetDesc(self, &desc);
        return desc;
    }
};

pub const ID3D11DeviceContext = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        base: IUnknown.VTable,
        // D3D11DeviceContext functions (CopyResource is at index 47, base IUnknown is 3 methods, so reserved is 44)
        reserved: [44]?*const anyopaque,
        CopyResource: *const fn (self: *ID3D11DeviceContext, pDstResource: *ID3D11Resource, pSrcResource: *ID3D11Resource) callconv(.winapi) void,
    };

    pub inline fn release(self: *ID3D11DeviceContext) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }

    pub inline fn copyResource(self: *ID3D11DeviceContext, dst: *ID3D11Resource, src: *ID3D11Resource) void {
        self.vtable.CopyResource(self, dst, src);
    }
};

pub const IID_ID3D11Texture2D = std.os.windows.GUID.parse("{6f15aaf2-d208-4e89-9ab4-489535d34f9c}");

pub const ID3D11Device = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        base: IUnknown.VTable,
        CreateBuffer: ?*const anyopaque,
        CreateTexture1D: ?*const anyopaque,
        CreateTexture2D: *const fn (
            self: *ID3D11Device,
            pDesc: *const D3D11_TEXTURE2D_DESC,
            pInitialData: ?*const anyopaque,
            ppTexture2D: *?*ID3D11Texture2D,
        ) callconv(.winapi) HRESULT,
        CreateTexture3D: ?*const anyopaque,
        CreateShaderResourceView: ?*const anyopaque,
        CreateUnorderedAccessView: ?*const anyopaque,
        CreateRenderTargetView: ?*const anyopaque,
        CreateDepthStencilView: ?*const anyopaque,
        CreateInputLayout: ?*const anyopaque,
        CreateVertexShader: ?*const anyopaque,
        CreateGeometryShader: ?*const anyopaque,
        CreateGeometryShaderWithStreamOutput: ?*const anyopaque,
        CreatePixelShader: ?*const anyopaque,
        CreateHullShader: ?*const anyopaque,
        CreateDomainShader: ?*const anyopaque,
        CreateComputeShader: ?*const anyopaque,
        CreateClassLinkage: ?*const anyopaque,
        CreateBlendState: ?*const anyopaque,
        CreateDepthStencilState: ?*const anyopaque,
        CreateRasterizerState: ?*const anyopaque,
        CreateSamplerState: ?*const anyopaque,
        CreateQuery: ?*const anyopaque,
        CreatePredicate: ?*const anyopaque,
        CreateCounter: ?*const anyopaque,
        CreateDeferredContext: ?*const anyopaque,
        OpenSharedResource: *const fn (
            self: *ID3D11Device,
            hResource: ?*anyopaque,
            ReturnedInterface: *const std.os.windows.GUID,
            ppResource: *?*anyopaque,
        ) callconv(.winapi) HRESULT,
    };

    pub inline fn release(self: *ID3D11Device) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }

    pub inline fn createTexture2D(self: *ID3D11Device, desc: *const D3D11_TEXTURE2D_DESC) !*ID3D11Texture2D {
        var tex: ?*ID3D11Texture2D = null;
        const hr = self.vtable.CreateTexture2D(self, desc, null, &tex);
        if (hr != S_OK or tex == null) return error.TextureCreationFailed;
        return tex.?;
    }

    pub inline fn openSharedResource(self: *ID3D11Device, handle: ?*anyopaque) !*ID3D11Texture2D {
        var res: ?*anyopaque = null;
        const hr = self.vtable.OpenSharedResource(self, handle, &IID_ID3D11Texture2D, &res);
        if (hr != S_OK or res == null) return error.OpenSharedResourceFailed;
        return @ptrCast(@alignCast(res.?));
    }
};

pub const IID_ID3D11Multithread = std.os.windows.GUID{
    .Data1 = 0x9b7e4e00,
    .Data2 = 0x342c,
    .Data3 = 0x4106,
    .Data4 = .{ 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 },
};

pub const ID3D11Multithread = extern struct {
    vtable: *const VTable,

    pub const VTable = extern struct {
        base: IUnknown.VTable,
        Enter: *const fn (self: *ID3D11Multithread) callconv(.winapi) void,
        Leave: *const fn (self: *ID3D11Multithread) callconv(.winapi) void,
        SetMultithreadProtected: *const fn (self: *ID3D11Multithread, bMTProtect: i32) callconv(.winapi) i32,
        GetMultithreadProtected: *const fn (self: *ID3D11Multithread) callconv(.winapi) i32,
    };

    pub inline fn release(self: *ID3D11Multithread) u32 {
        return self.vtable.base.Release(@ptrCast(self));
    }
};

// D3D11 Dynamic / Direct Creation Function
pub extern "d3d11" fn D3D11CreateDevice(
    pAdapter: ?*anyopaque,
    DriverType: u32,
    Software: ?*anyopaque,
    Flags: u32,
    pFeatureLevels: ?[*]const D3D_FEATURE_LEVEL,
    FeatureLevels: u32,
    SDKVersion: u32,
    ppDevice: *?*ID3D11Device,
    pFeatureLevel: ?*D3D_FEATURE_LEVEL,
    ppImmediateContext: *?*ID3D11DeviceContext,
) callconv(.winapi) HRESULT;

pub const D3D11Context = struct {
    device: *ID3D11Device,
    context: *ID3D11DeviceContext,
    feature_level: D3D_FEATURE_LEVEL,

    pub fn init() !D3D11Context {
        return initWithAdapter(null);
    }

    pub fn initWithAdapter(adapter: ?*anyopaque) !D3D11Context {
        const feature_levels = [_]D3D_FEATURE_LEVEL{
            .level_11_1,
            .level_11_0,
            .level_10_1,
            .level_10_0,
        };

        var device: ?*ID3D11Device = null;
        var context: ?*ID3D11DeviceContext = null;
        var chosen_level: D3D_FEATURE_LEVEL = .level_11_0;

        const driver_type: u32 = if (adapter != null) 0 else 1; // 0 = UNKNOWN, 1 = HARDWARE

        const hr = D3D11CreateDevice(
            adapter,
            driver_type,
            null,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT,
            &feature_levels,
            feature_levels.len,
            D3D11_SDK_VERSION,
            &device,
            &chosen_level,
            &context,
        );

        if (hr != S_OK or device == null or context == null) {
            std.log.scoped(.d3d11).warn("D3D11CreateDevice failed with hr=0x{x}", .{@as(u32, @bitCast(hr))});
            return error.D3D11DeviceCreationFailed;
        }

        // Enable D3D11 Multithread protection so capture and encoder threads can safely share context
        var multithread_ptr: ?*anyopaque = null;
        if (context.?.vtable.base.QueryInterface(@ptrCast(context.?), &IID_ID3D11Multithread, &multithread_ptr) == S_OK and multithread_ptr != null) {
            const mt: *ID3D11Multithread = @ptrCast(@alignCast(multithread_ptr.?));
            _ = mt.vtable.SetMultithreadProtected(mt, 1);
            _ = mt.release();
        }

        return .{
            .device = device.?,
            .context = context.?,
            .feature_level = chosen_level,
        };
    }

    pub fn deinit(self: *D3D11Context) void {
        _ = self.context.release();
        _ = self.device.release();
    }
};
