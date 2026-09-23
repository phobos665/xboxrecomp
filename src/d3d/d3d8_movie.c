/*
 * d3d8_movie.c -- see d3d8_movie.h.
 *
 * Built the way d3d8_overlay.c is: its own texture, shaders and states, the
 * quad from SV_VertexID, no sampler (the pixel shader filters by Load, four
 * texels, so the title's sampler state is never touched and the picture is
 * still smooth at any window size), and the render target and viewport put
 * back afterwards.
 *
 * Frames arrive on whichever guest thread polls the movie and are drawn at
 * Swap, so the CPU copy is guarded and the upload happens at draw time, on
 * the device's thread.
 */
#include "d3d8_internal.h"
#include "d3d8_movie.h"

#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int                       tried, failed;
    ID3D11Texture2D          *texture;
    ID3D11ShaderResourceView *srv;
    uint32_t                  tex_w, tex_h;
    ID3D11Buffer             *cb;
    ID3D11VertexShader       *vs;
    ID3D11PixelShader        *ps;
    ID3D11BlendState         *blend;
    ID3D11DepthStencilState  *depth;
    ID3D11RasterizerState    *raster;

    SRWLOCK                   lock;
    uint8_t                  *frame;       /* latest frame, BGRA */
    uint32_t                  frame_w, frame_h;
    int                       dirty;       /* not uploaded yet */
    int                       have;        /* a frame to draw */
} g = { 0, 0, NULL, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, SRWLOCK_INIT };

typedef struct {
    float x, y, w, h;          /* the picture's rectangle, clip space */
    float tex_w, tex_h, pad0, pad1;
} MovieConstants;

static const char kShaderSource[] =
    "cbuffer Movie : register(b7) {\n"
    "    float4 rect;\n"
    "    float4 size;\n"
    "};\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2(id & 1, id >> 1);\n"
    "    VSOut o;\n"
    "    o.pos = float4(rect.x + c.x * rect.z, rect.y - c.y * rect.w, 0, 1);\n"
    "    o.uv  = c * size.xy;\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> tex : register(t9);\n"
    "float4 texel(int2 p) {\n"
    "    p = clamp(p, int2(0, 0), int2(size.xy) - 1);\n"
    "    return tex.Load(int3(p, 0));\n"
    "}\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float2 t = i.uv - 0.5;\n"
    "    int2 p = int2(floor(t));\n"
    "    float2 f = t - floor(t);\n"
    "    float4 a = lerp(texel(p), texel(p + int2(1, 0)), f.x);\n"
    "    float4 b = lerp(texel(p + int2(0, 1)), texel(p + int2(1, 1)), f.x);\n"
    "    return float4(lerp(a, b, f.y).rgb, 1);\n"
    "}\n";

static void movie_fail(const char *what, HRESULT hr)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 movie: %s failed (0x%08lX); movies are not drawn this run\n",
            what, (unsigned long)hr);
    fflush(stderr);
}

static int compile(const char *entry, const char *profile, ID3DBlob **code)
{
    ID3DBlob *err = NULL;
    HRESULT hr = D3DCompile(kShaderSource, sizeof kShaderSource - 1, "movie", NULL, NULL,
                            entry, profile, 0, 0, code, &err);
    if (FAILED(hr)) {
        if (err)
            fprintf(stderr, "D3D8 movie: %s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        movie_fail("D3DCompile", hr);
    }
    if (err)
        ID3D10Blob_Release(err);
    return SUCCEEDED(hr);
}

static int movie_create(void)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    D3D11_BUFFER_DESC bd;
    D3D11_BLEND_DESC bl;
    D3D11_DEPTH_STENCIL_DESC ds;
    D3D11_RASTERIZER_DESC rd;
    ID3DBlob *code = NULL;
    HRESULT hr;

    if (g.failed)
        return 0;
    if (g.tried)
        return g.vs != NULL;
    g.tried = 1;
    if (!dev) {
        g.failed = 1;
        return 0;
    }
    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(MovieConstants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateBuffer(dev, &bd, NULL, &g.cb);
    if (FAILED(hr)) { movie_fail("CreateBuffer", hr); return 0; }

    if (!compile("vs_main", "vs_4_0", &code))
        return 0;
    hr = ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(code),
                                         ID3D10Blob_GetBufferSize(code), NULL, &g.vs);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { movie_fail("CreateVertexShader", hr); return 0; }
    if (!compile("ps_main", "ps_4_0", &code))
        return 0;
    hr = ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(code),
                                        ID3D10Blob_GetBufferSize(code), NULL, &g.ps);
    ID3D10Blob_Release(code);
    if (FAILED(hr)) { movie_fail("CreatePixelShader", hr); return 0; }

    memset(&bl, 0, sizeof bl);
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    hr = ID3D11Device_CreateBlendState(dev, &bl, &g.blend);
    if (FAILED(hr)) { movie_fail("CreateBlendState", hr); return 0; }
    memset(&ds, 0, sizeof ds);
    hr = ID3D11Device_CreateDepthStencilState(dev, &ds, &g.depth);
    if (FAILED(hr)) { movie_fail("CreateDepthStencilState", hr); return 0; }
    memset(&rd, 0, sizeof rd);
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = ID3D11Device_CreateRasterizerState(dev, &rd, &g.raster);
    if (FAILED(hr)) { movie_fail("CreateRasterizerState", hr); return 0; }
    return 1;
}

/* A texture of the frame's size, remade when the size changes. */
static int movie_texture(uint32_t w, uint32_t h)
{
    ID3D11Device *dev = d3d8_GetD3D11Device();
    D3D11_TEXTURE2D_DESC td;
    HRESULT hr;

    if (g.texture && g.tex_w == w && g.tex_h == h)
        return 1;
    if (g.srv) { ID3D11ShaderResourceView_Release(g.srv); g.srv = NULL; }
    if (g.texture) { ID3D11Texture2D_Release(g.texture); g.texture = NULL; }
    memset(&td, 0, sizeof td);
    td.Width = w;
    td.Height = h;
    td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    hr = ID3D11Device_CreateTexture2D(dev, &td, NULL, &g.texture);
    if (FAILED(hr)) { movie_fail("CreateTexture2D", hr); return 0; }
    hr = ID3D11Device_CreateShaderResourceView(dev, (ID3D11Resource *)g.texture, NULL, &g.srv);
    if (FAILED(hr)) { movie_fail("CreateShaderResourceView", hr); return 0; }
    g.tex_w = w;
    g.tex_h = h;
    return 1;
}

void d3d8_movie_set_frame(const uint8_t *bgra, uint32_t width, uint32_t height)
{
    size_t bytes = (size_t)width * height * 4u;

    if (!bgra || !width || !height)
        return;
    AcquireSRWLockExclusive(&g.lock);
    if (g.frame_w != width || g.frame_h != height) {
        free(g.frame);
        g.frame = (uint8_t *)malloc(bytes);
        g.frame_w = g.frame ? width : 0;
        g.frame_h = g.frame ? height : 0;
    }
    if (g.frame) {
        memcpy(g.frame, bgra, bytes);
        g.dirty = g.have = 1;
    }
    ReleaseSRWLockExclusive(&g.lock);
}

void d3d8_movie_clear(void)
{
    AcquireSRWLockExclusive(&g.lock);
    g.have = 0;
    ReleaseSRWLockExclusive(&g.lock);
}

void d3d8_movie_draw(void)
{
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    ID3D11RenderTargetView *rtv = d3d8_GetDefaultRTV();
    ID3D11RenderTargetView *saved_rtv = NULL;
    ID3D11DepthStencilView *saved_dsv = NULL;
    D3D11_VIEWPORT saved_vp[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE], vp;
    UINT saved_vps = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    UINT bw = d3d8_GetBackBufferWidth(), bh = d3d8_GetBackBufferHeight();
    D3D11_MAPPED_SUBRESOURCE mapped;
    MovieConstants c;
    float blend_factor[4] = { 1, 1, 1, 1 }, black[4] = { 0, 0, 0, 1 };
    float fa, ba;
    uint32_t w, h, y;

    if (!ctx || !rtv || !bw || !bh || !g.have || !movie_create())
        return;

    AcquireSRWLockExclusive(&g.lock);
    w = g.frame_w;
    h = g.frame_h;
    if (!g.have || !g.frame || !movie_texture(w, h)) {
        ReleaseSRWLockExclusive(&g.lock);
        return;
    }
    if (g.dirty &&
        SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g.texture, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        for (y = 0; y < h; y++)
            memcpy((uint8_t *)mapped.pData + (size_t)y * mapped.RowPitch,
                   g.frame + (size_t)y * w * 4u, (size_t)w * 4u);
        ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g.texture, 0);
        g.dirty = 0;
    }
    ReleaseSRWLockExclusive(&g.lock);

    ID3D11DeviceContext_OMGetRenderTargets(ctx, 1, &saved_rtv, &saved_dsv);
    ID3D11DeviceContext_RSGetViewports(ctx, &saved_vps, saved_vp);

    /* The whole screen goes black first, then the picture at its own aspect:
     * a 720x576 PAL movie and a 640x480 one both fill a 4:3 window, and
     * neither is stretched to a wide one. Xbox movies are encoded for a 4:3
     * screen whatever their pixel size, so 4:3 is the aspect used. */
    ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, black);
    if (FAILED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)g.cb, 0,
                                       D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        goto restore;
    fa = 4.0f / 3.0f;
    ba = (float)bw / (float)bh;
    c.w = ba > fa ? 2.0f * fa / ba : 2.0f;
    c.h = ba > fa ? 2.0f : 2.0f * ba / fa;
    c.x = -c.w / 2.0f;
    c.y = c.h / 2.0f;
    c.tex_w = (float)w;
    c.tex_h = (float)h;
    c.pad0 = c.pad1 = 0.0f;
    memcpy(mapped.pData, &c, sizeof c);
    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)g.cb, 0);

    vp.TopLeftX = vp.TopLeftY = 0.0f;
    vp.Width = (float)bw;
    vp.Height = (float)bh;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
    ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(ctx, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    ID3D11DeviceContext_VSSetShader(ctx, g.vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(ctx, g.ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(ctx, 7, 1, &g.cb);
    ID3D11DeviceContext_PSSetConstantBuffers(ctx, 7, 1, &g.cb);
    ID3D11DeviceContext_PSSetShaderResources(ctx, 9, 1, &g.srv);
    ID3D11DeviceContext_OMSetBlendState(ctx, g.blend, blend_factor, 0xFFFFFFFF);
    ID3D11DeviceContext_OMSetDepthStencilState(ctx, g.depth, 0);
    ID3D11DeviceContext_RSSetState(ctx, g.raster);
    ID3D11DeviceContext_Draw(ctx, 4, 0);

restore:
    ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &saved_rtv, saved_dsv);
    if (saved_vps)
        ID3D11DeviceContext_RSSetViewports(ctx, saved_vps, saved_vp);
    if (saved_rtv) ID3D11RenderTargetView_Release(saved_rtv);
    if (saved_dsv) ID3D11DepthStencilView_Release(saved_dsv);
}

void d3d8_movie_shutdown(void)
{
    if (g.srv)     { ID3D11ShaderResourceView_Release(g.srv);  g.srv = NULL; }
    if (g.texture) { ID3D11Texture2D_Release(g.texture);       g.texture = NULL; }
    if (g.cb)      { ID3D11Buffer_Release(g.cb);               g.cb = NULL; }
    if (g.vs)      { ID3D11VertexShader_Release(g.vs);         g.vs = NULL; }
    if (g.ps)      { ID3D11PixelShader_Release(g.ps);          g.ps = NULL; }
    if (g.blend)   { ID3D11BlendState_Release(g.blend);        g.blend = NULL; }
    if (g.depth)   { ID3D11DepthStencilState_Release(g.depth); g.depth = NULL; }
    if (g.raster)  { ID3D11RasterizerState_Release(g.raster);  g.raster = NULL; }
    g.tex_w = g.tex_h = 0;
    g.tried = 0;
}
