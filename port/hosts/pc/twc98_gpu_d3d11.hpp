#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace twc98_pc_gpu {

constexpr int kFilterCount = 4;
constexpr const char* kFilterNames[kFilterCount] = {
    "NEAREST", "BILINEAR", "SHARP-BI", "CRT-ARC"
};

constexpr int kCrtLevelCount = 3;
constexpr const char* kCrtLevelNames[kCrtLevelCount] = {
    "OFF", "SOFT", "ARCADE"
};

struct alignas(16) ShaderParams {
    float tex_w;
    float tex_h;
    float dst_w;
    float dst_h;
    int32_t filter_mode;   // 0=Nearest, 1=Bilinear, 2=Sharp-Bilinear, 3=CRT-Arcade
    float crt_strength;    // 0.0, 0.55, 1.0
    float pad0;
    float pad1;
};

static const GUID kIID_ID3D11Texture2D = {
    0x6f15aaf2u, 0xd208u, 0x4e89u, {0x9au, 0xb4u, 0x48u, 0x95u, 0x35u, 0xd3u, 0x4fu, 0x9cu}
};

constexpr const char* kVsSource = R"(
struct VSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
};
VSOut main(uint vid : SV_VertexID) {
    float2 uv = float2((vid & 1u) ? 1.0 : 0.0, (vid & 2u) ? 1.0 : 0.0);
    VSOut o;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}
)";

constexpr const char* kPsSource = R"(
Texture2D<float4> srcTex : register(t0);
SamplerState sampPoint   : register(s0);
SamplerState sampLinear  : register(s1);

cbuffer Params : register(b0) {
    float2 texSize;
    float2 dstSize;
    int    filterMode;
    float  crtStrength;
    float2 padUnused;
};

float3 sample_linear_uv(float2 uv) {
    return srcTex.SampleLevel(sampLinear, uv, 0.0).rgb;
}

float3 sample_sharp_bilinear(float2 uv) {
    float2 scale = max(dstSize / texSize, float2(1.0, 1.0));
    float2 p = uv * texSize - 0.5;
    float2 pi = floor(p);
    float2 pf = p - pi;
    float2 pf_sharp = saturate((pf - 0.5) * scale + 0.5);
    float2 uv_sharp = (pi + pf_sharp + 0.5) / texSize;
    return srcTex.SampleLevel(sampLinear, uv_sharp, 0.0).rgb;
}

float3 sample_crt_arcade(float2 uv, float crt_amt) {
    // 5-tap horizontal Gaussian beam filter smooths 2x2 RBG0 steps and sprite jaggies
    // while keeping vertical line definition clean for arcade scanlines.
    float dx = 1.0 / texSize.x;
    float3 c0 = sample_linear_uv(uv + float2(-1.60 * dx, 0.0));
    float3 c1 = sample_linear_uv(uv + float2(-0.80 * dx, 0.0));
    float3 c2 = sample_linear_uv(uv);
    float3 c3 = sample_linear_uv(uv + float2( 0.80 * dx, 0.0));
    float3 c4 = sample_linear_uv(uv + float2( 1.60 * dx, 0.0));
    float3 beam = c0 * 0.08 + c1 * 0.22 + c2 * 0.40 + c3 * 0.22 + c4 * 0.08;

    // Slight highlight glow / saturation lift like arcade CRT phosphor
    float3 col = lerp(beam, pow(max(beam, 0.0), 0.94), 0.55);

    if (crt_amt > 0.01) {
        // Vertical beam profile over original source rows (224/448 scanline cadence)
        float scan_rows = (texSize.y > 300.0) ? (texSize.y * 0.5) : texSize.y;
        float row_phase = frac(uv.y * scan_rows);
        float scan_wave = 0.5 - 0.5 * cos(row_phase * 6.2831853);
        float luma = dot(col, float3(0.299, 0.587, 0.114));
        float scan_min = lerp(0.55, 0.78, luma); // bright highlights bloom wider across scanlines
        float scan_mul = lerp(1.0, lerp(scan_min, 1.08, scan_wave), crt_amt);

        // Subtle RGB aperture grille triad across physical output pixels
        float px_x = uv.x * dstSize.x;
        int triad = ((int)floor(px_x)) % 3;
        float3 mask = (triad == 0) ? float3(1.06, 0.94, 0.94)
                    : (triad == 1) ? float3(0.94, 1.06, 0.94)
                                   : float3(0.94, 0.94, 1.06);
        float3 mask_mul = lerp(float3(1.0, 1.0, 1.0), mask, crt_amt * 0.65);

        col *= scan_mul * mask_mul * lerp(1.0, 1.08, crt_amt);
    }
    return saturate(col);
}

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    float3 col;
    if (filterMode == 0) {
        col = srcTex.SampleLevel(sampPoint, uv, 0.0).rgb;
        if (crtStrength > 0.01) {
            float scan_rows = (texSize.y > 300.0) ? (texSize.y * 0.5) : texSize.y;
            float scan_wave = 0.5 - 0.5 * cos(frac(uv.y * scan_rows) * 6.2831853);
            col *= lerp(1.0, lerp(0.68, 1.05, scan_wave), crtStrength);
        }
    } else if (filterMode == 1) {
        col = sample_linear_uv(uv);
        if (crtStrength > 0.01) {
            float scan_rows = (texSize.y > 300.0) ? (texSize.y * 0.5) : texSize.y;
            float scan_wave = 0.5 - 0.5 * cos(frac(uv.y * scan_rows) * 6.2831853);
            col *= lerp(1.0, lerp(0.68, 1.05, scan_wave), crtStrength);
        }
    } else if (filterMode == 2) {
        col = sample_sharp_bilinear(uv);
        if (crtStrength > 0.01) {
            float scan_rows = (texSize.y > 300.0) ? (texSize.y * 0.5) : texSize.y;
            float scan_wave = 0.5 - 0.5 * cos(frac(uv.y * scan_rows) * 6.2831853);
            col *= lerp(1.0, lerp(0.68, 1.05, scan_wave), crtStrength);
        }
    } else {
        col = sample_crt_arcade(uv, crtStrength);
    }
    return float4(col, 1.0);
}
)";

constexpr const char* kOvPsSource = R"(
Texture2D<float4> ovTex : register(t0);
SamplerState sampPoint  : register(s0);

float4 main(float4 pos : SV_Position, float2 uv : TEXCOORD0) : SV_Target {
    return ovTex.SampleLevel(sampPoint, uv, 0.0);
}
)";

struct GpuD3D11Presenter {
    HMODULE d3d11_dll = nullptr;
    HMODULE compiler_dll = nullptr;
    PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN fn_create_dev_sc = nullptr;
    PFN_D3D11_CREATE_DEVICE fn_create_dev = nullptr;
    pD3DCompile fn_compile = nullptr;

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    IDXGISwapChain* sc = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    ID3D11VertexShader* vs = nullptr;
    ID3D11PixelShader* ps = nullptr;
    ID3D11PixelShader* ov_ps = nullptr;
    ID3D11BlendState* blend_alpha = nullptr;
    ID3D11Texture2D* tex = nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    ID3D11Texture2D* ov_tex = nullptr;
    ID3D11ShaderResourceView* ov_srv = nullptr;
    ID3D11SamplerState* samp_point = nullptr;
    ID3D11SamplerState* samp_linear = nullptr;
    ID3D11Buffer* cbuf = nullptr;

    int tex_w = 0;
    int tex_h = 0;
    int ov_tex_w = 0;
    int ov_tex_h = 0;
    int bb_w = 0;
    int bb_h = 0;
    bool ready = false;
    std::string backend_name = "CPU-GDI";
    std::string last_error;

    bool load_apis() {
        if (fn_create_dev_sc && fn_create_dev && fn_compile) return true;
        LoadLibraryA("user32.dll");
        LoadLibraryA("gdi32.dll");
        LoadLibraryA("dxgi.dll");
        if (!d3d11_dll) d3d11_dll = LoadLibraryA("d3d11.dll");
        if (!compiler_dll) compiler_dll = LoadLibraryA("d3dcompiler_47.dll");
        if (!compiler_dll) compiler_dll = LoadLibraryA("d3dcompiler_43.dll");
        if (!d3d11_dll || !compiler_dll) {
            last_error = "Missing d3d11.dll or d3dcompiler_47.dll";
            return false;
        }
        fn_create_dev_sc = reinterpret_cast<PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN>(
            reinterpret_cast<void*>(GetProcAddress(d3d11_dll, "D3D11CreateDeviceAndSwapChain")));
        fn_create_dev = reinterpret_cast<PFN_D3D11_CREATE_DEVICE>(
            reinterpret_cast<void*>(GetProcAddress(d3d11_dll, "D3D11CreateDevice")));
        fn_compile = reinterpret_cast<pD3DCompile>(
            reinterpret_cast<void*>(GetProcAddress(compiler_dll, "D3DCompile")));
        if (!fn_create_dev_sc || !fn_create_dev || !fn_compile) {
            last_error = "Failed to resolve D3D11/D3DCompile symbols";
            return false;
        }
        return true;
    }

    template <typename T>
    static void safe_release(T*& p) {
        if (p) {
            p->Release();
            p = nullptr;
        }
    }

    void shutdown() {
        ready = false;
        safe_release(cbuf);
        safe_release(samp_linear);
        safe_release(samp_point);
        safe_release(ov_srv);
        safe_release(ov_tex);
        safe_release(srv);
        safe_release(tex);
        safe_release(blend_alpha);
        safe_release(ov_ps);
        safe_release(ps);
        safe_release(vs);
        safe_release(rtv);
        safe_release(sc);
        safe_release(ctx);
        safe_release(dev);
        tex_w = tex_h = ov_tex_w = ov_tex_h = bb_w = bb_h = 0;
    }

    bool compile_shader(const char* src, const char* entry, const char* target, ID3DBlob** out_blob) {
        ID3DBlob* err = nullptr;
        HRESULT hr = fn_compile(
            src, std::strlen(src), nullptr, nullptr, nullptr,
            entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, out_blob, &err);
        if (FAILED(hr)) {
            if (err) {
                last_error.assign(
                    static_cast<const char*>(err->GetBufferPointer()),
                    err->GetBufferSize());
                err->Release();
            } else {
                char buf[64]{};
                std::snprintf(buf, sizeof(buf), "D3DCompile failed 0x%08lX", static_cast<unsigned long>(hr));
                last_error = buf;
            }
            return false;
        }
        if (err) err->Release();
        return true;
    }

    bool create_common_resources() {
        ID3DBlob* vs_blob = nullptr;
        ID3DBlob* ps_blob = nullptr;
        ID3DBlob* ov_blob = nullptr;
        if (!compile_shader(kVsSource, "main", "vs_4_0", &vs_blob) ||
            !compile_shader(kPsSource, "main", "ps_4_0", &ps_blob) ||
            !compile_shader(kOvPsSource, "main", "ps_4_0", &ov_blob)) {
            safe_release(vs_blob);
            safe_release(ps_blob);
            safe_release(ov_blob);
            return false;
        }
        HRESULT hr_vs = dev->CreateVertexShader(
            vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(), nullptr, &vs);
        HRESULT hr_ps = dev->CreatePixelShader(
            ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(), nullptr, &ps);
        HRESULT hr_ov = dev->CreatePixelShader(
            ov_blob->GetBufferPointer(), ov_blob->GetBufferSize(), nullptr, &ov_ps);
        safe_release(vs_blob);
        safe_release(ps_blob);
        safe_release(ov_blob);
        if (FAILED(hr_vs) || FAILED(hr_ps) || FAILED(hr_ov)) {
            last_error = "CreateVertexShader/CreatePixelShader failed";
            return false;
        }

        D3D11_BLEND_DESC bd{};
        bd.RenderTarget[0].BlendEnable = TRUE;
        bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
        bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
        bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
        bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(dev->CreateBlendState(&bd, &blend_alpha))) {
            last_error = "CreateBlendState(alpha) failed";
            return false;
        }

        D3D11_SAMPLER_DESC sd{};
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD = D3D11_FLOAT32_MAX;
        if (FAILED(dev->CreateSamplerState(&sd, &samp_point))) {
            last_error = "CreateSamplerState(point) failed";
            return false;
        }
        sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        if (FAILED(dev->CreateSamplerState(&sd, &samp_linear))) {
            last_error = "CreateSamplerState(linear) failed";
            return false;
        }

        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = sizeof(ShaderParams);
        cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateBuffer(&cbd, nullptr, &cbuf))) {
            last_error = "CreateBuffer(cbuf) failed";
            return false;
        }
        return true;
    }

    bool ensure_source_texture(int fw, int fh) {
        if (tex && srv && tex_w == fw && tex_h == fh) return true;
        safe_release(srv);
        safe_release(tex);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(fw);
        td.Height = static_cast<UINT>(fh);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &tex)) ||
            FAILED(dev->CreateShaderResourceView(tex, nullptr, &srv))) {
            last_error = "CreateTexture2D/SRV failed";
            safe_release(srv);
            safe_release(tex);
            return false;
        }
        tex_w = fw;
        tex_h = fh;
        return true;
    }

    bool ensure_overlay_texture(int ow, int oh) {
        if (ov_tex && ov_srv && ov_tex_w == ow && ov_tex_h == oh) return true;
        safe_release(ov_srv);
        safe_release(ov_tex);

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(ow);
        td.Height = static_cast<UINT>(oh);
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev->CreateTexture2D(&td, nullptr, &ov_tex)) ||
            FAILED(dev->CreateShaderResourceView(ov_tex, nullptr, &ov_srv))) {
            last_error = "CreateOverlayTexture/SRV failed";
            safe_release(ov_srv);
            safe_release(ov_tex);
            return false;
        }
        ov_tex_w = ow;
        ov_tex_h = oh;
        return true;
    }

    bool rebuild_rtv(int cw, int ch) {
        safe_release(rtv);
        if (sc) {
            if (bb_w != 0 && (bb_w != cw || bb_h != ch)) {
                HRESULT hr = sc->ResizeBuffers(0, static_cast<UINT>(cw), static_cast<UINT>(ch),
                                               DXGI_FORMAT_UNKNOWN, 0);
                if (FAILED(hr)) {
                    last_error = "ResizeBuffers failed";
                    return false;
                }
            }
            ID3D11Texture2D* bb = nullptr;
            HRESULT hr = sc->GetBuffer(0, kIID_ID3D11Texture2D, reinterpret_cast<void**>(&bb));
            if (FAILED(hr) || !bb) {
                last_error = "SwapChain::GetBuffer failed";
                safe_release(bb);
                return false;
            }
            hr = dev->CreateRenderTargetView(bb, nullptr, &rtv);
            safe_release(bb);
            if (FAILED(hr) || !rtv) {
                last_error = "CreateRenderTargetView failed";
                return false;
            }
        }
        bb_w = cw;
        bb_h = ch;
        return true;
    }

    bool init(HWND hwnd, int cw, int ch) {
        shutdown();
        if (!hwnd || !load_apis()) return false;

        DXGI_SWAP_CHAIN_DESC scd{};
        scd.BufferCount = 1;
        scd.BufferDesc.Width = static_cast<UINT>(std::max(1, cw));
        scd.BufferDesc.Height = static_cast<UINT>(std::max(1, ch));
        scd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        scd.BufferDesc.RefreshRate.Numerator = 0;
        scd.BufferDesc.RefreshRate.Denominator = 1;
        scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        scd.OutputWindow = hwnd;
        scd.SampleDesc.Count = 1;
        scd.Windowed = TRUE;
        scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL fl{};
        HRESULT hr = fn_create_dev_sc(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 2, D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
        backend_name = "GPU-D3D11-HW";
        if (FAILED(hr)) {
            hr = fn_create_dev_sc(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, 2, D3D11_SDK_VERSION, &scd, &sc, &dev, &fl, &ctx);
            backend_name = "GPU-D3D11-WARP";
        }
        if (FAILED(hr) || !sc || !dev || !ctx) {
            last_error = "D3D11CreateDeviceAndSwapChain failed";
            shutdown();
            return false;
        }
        if (!create_common_resources() ||
            !rebuild_rtv(std::max(1, cw), std::max(1, ch)) ||
            !ensure_source_texture(704, 448)) {
            shutdown();
            return false;
        }
        ready = true;
        return true;
    }

    bool present(const uint32_t* pixels, int fw, int fh,
                 int cw, int ch, int dx, int dy, int dw, int dh,
                 int filter_mode, int crt_level,
                 const uint32_t* ov_pixels = nullptr,
                 int ov_x = 0, int ov_y = 0, int ov_w = 0, int ov_h = 0) {
        if (!ready || !pixels || fw <= 0 || fh <= 0) return false;
        cw = std::max(1, cw);
        ch = std::max(1, ch);
        if (cw != bb_w || ch != bb_h || !rtv) {
            if (!rebuild_rtv(cw, ch)) return false;
        }
        if (!ensure_source_texture(fw, fh)) return false;

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
        const size_t row_bytes = static_cast<size_t>(fw) * sizeof(uint32_t);
        for (int y = 0; y < fh; ++y) {
            uint8_t* dst_row = static_cast<uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
            std::memcpy(dst_row, pixels + static_cast<size_t>(y) * static_cast<size_t>(fw), row_bytes);
        }
        ctx->Unmap(tex, 0);

        D3D11_MAPPED_SUBRESOURCE cb_map{};
        if (FAILED(ctx->Map(cbuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &cb_map))) return false;
        ShaderParams* sp = static_cast<ShaderParams*>(cb_map.pData);
        sp->tex_w = static_cast<float>(fw);
        sp->tex_h = static_cast<float>(fh);
        sp->dst_w = static_cast<float>(std::max(1, dw));
        sp->dst_h = static_cast<float>(std::max(1, dh));
        sp->filter_mode = std::clamp(filter_mode, 0, kFilterCount - 1);
        static constexpr float kCrtStrengths[kCrtLevelCount] = {0.0f, 0.55f, 1.0f};
        sp->crt_strength = kCrtStrengths[std::clamp(crt_level, 0, kCrtLevelCount - 1)];
        sp->pad0 = 0.0f;
        sp->pad1 = 0.0f;
        ctx->Unmap(cbuf, 0);

        const float clear_col[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        ctx->ClearRenderTargetView(rtv, clear_col);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);

        D3D11_VIEWPORT vp{};
        vp.TopLeftX = static_cast<float>(dx);
        vp.TopLeftY = static_cast<float>(dy);
        vp.Width = static_cast<float>(std::max(1, dw));
        vp.Height = static_cast<float>(std::max(1, dh));
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        ID3D11SamplerState* samplers[2] = {samp_point, samp_linear};
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetShader(vs, nullptr, 0);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->PSSetConstantBuffers(0, 1, &cbuf);
        ctx->PSSetShaderResources(0, 1, &srv);
        ctx->PSSetSamplers(0, 2, samplers);
        ctx->Draw(4, 0);

        if (ov_pixels && ov_w > 0 && ov_h > 0 && ensure_overlay_texture(ov_w, ov_h)) {
            D3D11_MAPPED_SUBRESOURCE ov_map{};
            if (SUCCEEDED(ctx->Map(ov_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &ov_map))) {
                const size_t ov_row_bytes = static_cast<size_t>(ov_w) * sizeof(uint32_t);
                for (int y = 0; y < ov_h; ++y) {
                    uint8_t* dst_row = static_cast<uint8_t*>(ov_map.pData) + static_cast<size_t>(y) * ov_map.RowPitch;
                    std::memcpy(dst_row, ov_pixels + static_cast<size_t>(y) * static_cast<size_t>(ov_w), ov_row_bytes);
                }
                ctx->Unmap(ov_tex, 0);

                D3D11_VIEWPORT ov_vp{};
                ov_vp.TopLeftX = static_cast<float>(ov_x);
                ov_vp.TopLeftY = static_cast<float>(ov_y);
                ov_vp.Width = static_cast<float>(ov_w);
                ov_vp.Height = static_cast<float>(ov_h);
                ov_vp.MinDepth = 0.0f;
                ov_vp.MaxDepth = 1.0f;
                ctx->RSSetViewports(1, &ov_vp);
                ctx->OMSetBlendState(blend_alpha, nullptr, 0xFFFFFFFFu);
                ctx->PSSetShader(ov_ps, nullptr, 0);
                ctx->PSSetShaderResources(0, 1, &ov_srv);
                ctx->Draw(4, 0);
                ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
            }
        }

        ID3D11ShaderResourceView* null_srv = nullptr;
        ctx->PSSetShaderResources(0, 1, &null_srv);

        HRESULT hr = sc->Present(0, 0);
        return SUCCEEDED(hr);
    }

    // Headless self-test: verifies HLSL compilation + D3D11 device/texture/shader creation without an HWND.
    bool test_headless_init(std::string& report) {
        shutdown();
        if (!load_apis()) {
            report = last_error;
            return false;
        }
        ID3DBlob* vs_blob = nullptr;
        ID3DBlob* ps_blob = nullptr;
        ID3DBlob* ov_blob = nullptr;
        if (!compile_shader(kVsSource, "main", "vs_4_0", &vs_blob) ||
            !compile_shader(kPsSource, "main", "ps_4_0", &ps_blob) ||
            !compile_shader(kOvPsSource, "main", "ps_4_0", &ov_blob)) {
            safe_release(vs_blob);
            safe_release(ps_blob);
            safe_release(ov_blob);
            report = std::string("SHADER_COMPILE_FAIL: ") + last_error;
            return false;
        }
        const size_t vs_sz = vs_blob->GetBufferSize();
        const size_t ps_sz = ps_blob->GetBufferSize() + ov_blob->GetBufferSize();
        safe_release(vs_blob);
        safe_release(ps_blob);
        safe_release(ov_blob);

        const D3D_FEATURE_LEVEL levels[] = {
            D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0
        };
        D3D_FEATURE_LEVEL fl{};
        HRESULT hr_hw = fn_create_dev(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
            levels, 3, D3D11_SDK_VERSION, &dev, &fl, &ctx);
        const char* drv = "HARDWARE";
        HRESULT hr_warp = S_OK;
        if (FAILED(hr_hw)) {
            hr_warp = fn_create_dev(
                nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
                levels, 3, D3D11_SDK_VERSION, &dev, &fl, &ctx);
            drv = "WARP";
        }
        if (FAILED(hr_hw) && FAILED(hr_warp)) {
            char buf[160]{};
            std::snprintf(buf, sizeof(buf),
                          "D3D11CreateDevice failed hw=0x%08lX warp=0x%08lX (vs=%zuB ps=%zuB)",
                          static_cast<unsigned long>(hr_hw),
                          static_cast<unsigned long>(hr_warp),
                          vs_sz, ps_sz);
            report = buf;
            shutdown();
            return false;
        }
        const bool ok = create_common_resources() &&
                        ensure_source_texture(704, 448) &&
                        ensure_overlay_texture(420, 420);
        if (!ok) {
            report = last_error;
            shutdown();
            return false;
        }
        char buf[160]{};
        std::snprintf(buf, sizeof(buf),
                      "PASS_GPU_D3D11_SHADERS driver=%s fl=0x%X vs=%zuB ps=%zuB",
                      drv, static_cast<unsigned>(fl), vs_sz, ps_sz);
        report = buf;
        shutdown();
        return true;
    }
};

}  // namespace twc98_pc_gpu
