/*
 * d3d8_screencopy.c -- the finished frame, copied into a texture.
 *
 * A title that post-processes its own image reads the screen back: it copies
 * the back buffer into a texture and draws that texture over the scene again,
 * for a glow, a soften, a heat haze. On the console the GPU does the copy and
 * the texture's memory is the framebuffer's.
 *
 * Under shadow mode the title's own copy still happens, but it happens in the
 * guest's world, where nothing is rendered -- so the texture it reads back is
 * all zeros, and drawing it over the scene blends black over everything.
 * TimeSplitters 2 does exactly this, three times a frame, and it cost the
 * picture 62% of its brightness before this existed.
 *
 * So the copy is done here as well, from the host's back buffer into the host
 * texture standing in for the title's. It is a draw rather than a resource
 * copy because the two rarely agree on format: the swap chain is RGBA and a
 * title's texture is usually BGRA, which D3D11 will not copy between. Reading
 * with Load at integer pixels also means no sampler is bound, so a texture
 * stage the title set is left alone -- the same rule d3d8_overlay.c follows
 * and for the same reason.
 */
#include "d3d8_internal.h"
#include "d3d8_xbox.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int             tried, failed;
    RhiView        *back_srv;       /* the scene, which the device owns */
    RhiShader      *vs;
    RhiShader      *ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    unsigned long   copies;
} g;

/* Source pixel = destination pixel * scale. One when the sizes agree, which
 * they do whenever a title reads back its own screen. */
typedef struct { float scale_x, scale_y, pad0, pad1; } ScreenCopyConstants;

static const char kSource[] =
    "cbuffer ScreenCopy : register(b7) { float4 scale; };\n"
    "struct VSOut { float4 pos : SV_Position; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    return o;\n"
    "}\n"
    "Texture2D<float4> screen : register(t9);\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    int2 p = int2(i.pos.xy * scale.xy);\n"
    "    return screen.Load(int3(p, 0));\n"
    "}\n";

static void fail(const char *what)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 screen copy: %s failed; a title that reads its "
            "own screen back will see black\n", what);
    fflush(stderr);
}

static RhiShader *compile_one(uint32_t stage, const char *entry, const char *target)
{
    RhiShaderSource src;
    RhiShader *s;
    char err[2048];

    memset(&src, 0, sizeof src);
    src.hlsl = kSource;
    src.len = sizeof kSource - 1;
    src.name = "screencopy";
    src.entry = entry;
    src.target = target;
    s = rhi_shader_create(stage, &src, err, sizeof err);
    if (!s) {
        if (err[0])
            fprintf(stderr, "D3D8 screen copy: %s\n", err);
        fail(stage == RHI_STAGE_VERTEX ? "the vertex shader" : "the pixel shader");
    }
    return s;
}

static int create(void)
{
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;

    if (g.failed) return 0;
    if (g.tried) return g.back_srv != NULL;
    g.tried = 1;
    if (!d3d8_GetD3D11Device() || !d3d8_GetSwapChain()) { g.failed = 1; return 0; }

    /* The scene the title has been drawing into, which is the swap chain's
     * own back buffer only while nothing is scaled. Reading the back
     * buffer directly would be wrong above scale 1: the resolve that fills
     * it has not run yet this frame, so it still holds the last one. */
    g.back_srv = d3d8_GetSceneView();
    if (!g.back_srv) { fail("GetSceneView"); return 0; }

    if (!(g.vs = compile_one(RHI_STAGE_VERTEX, "vs_main", "vs_4_0"))) return 0;
    if (!(g.ps = compile_one(RHI_STAGE_PIXEL, "ps_main", "ps_4_0"))) return 0;

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(ScreenCopyConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { fail("CreateBuffer"); return 0; }

    memset(&bl, 0, sizeof bl);
    bl.write_mask = RHI_WRITE_ALL;
    if (!(g.blend = rhi_blend_state_create(&bl))) { fail("CreateBlendState"); return 0; }

    memset(&ds, 0, sizeof ds);
    if (!(g.depth = rhi_depth_state_create(&ds))) { fail("CreateDepthStencilState"); return 0; }

    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.raster = rhi_raster_state_create(&rd))) { fail("CreateRasterizerState"); return 0; }
    return 1;
}

/* RECOMP_D3D8_SCREENCOPY_PROBE=n: what the source holds at the moment of the
 * first n copies, and whether drawing was going to the scene target at all.
 * The texture layer's own probe reads the destination; when that says "not
 * arriving" this says which side is empty. It reads the scene through
 * rhi_image_readback, so it reports the same under either backend. */
static void probe_source(const RhiOutputState *saved, const D3D8Texture *tex,
                         UINT back_w, UINT back_h)
{
    static int probe = -1;
    static unsigned long said;
    RhiImage *scene;
    RhiImageDesc sd;
    unsigned long long sum = 0;
    unsigned nonzero = 0, samples = 0;
    uint8_t *pixels;

    if (probe < 0) {
        const char *v = getenv("RECOMP_D3D8_SCREENCOPY_PROBE");
        probe = (v && atoi(v) > 0) ? atoi(v) : 0;   /* how many copies to report */
    }
    if (!probe || said++ >= (unsigned long)probe)
        return;

    memset(&sd, 0, sizeof sd);
    scene = d3d8_GetSceneImage();
    if (scene) {
        rhi_image_get_desc(scene, &sd);
        pixels = malloc((size_t)sd.width * sd.height * 4u);
        if (pixels && rhi_image_readback(scene, 0, pixels, sd.width * 4u) == 0) {
            UINT x, y;
            for (y = 0; y < sd.height; y += 16) {
                const uint8_t *row = pixels + (size_t)y * sd.width * 4u;
                for (x = 0; x < sd.width; x += 16) {
                    const uint8_t *p = row + (size_t)x * 4u;
                    sum += (unsigned)p[0] + p[1] + p[2];
                    samples += 3;
                    if (p[0] || p[1] || p[2]) nonzero++;
                }
            }
        }
        free(pixels);
    }
    fprintf(stderr, "D3D8 screen copy probe: scene %ux%u holds mean %.1f/255, %u of %u "
            "non-zero; current target %s the scene; dst %ux%u scale %g x %g\n",
            sd.width, sd.height,
            samples ? (double)sum / samples : 0.0, nonzero, samples / 3u,
            rhi_output_color_is(saved, d3d8_GetDefaultTargetView()) ? "is" : "is NOT",
            tex->width, tex->height,
            tex->width ? (double)back_w / tex->width : 0.0,
            tex->height ? (double)back_h / tex->height : 0.0);
    fflush(stderr);
}

HRESULT xbox_D3D8CopyBackBufferToTexture(IDirect3DTexture8 *dst)
{
    D3D8Texture *tex = (D3D8Texture *)dst;
    RhiImage *dst_image;
    RhiView *rtv;
    RhiViewDesc rtvd;
    RhiOutputState saved;
    ScreenCopyConstants c;
    RhiViewport vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    UINT back_w = d3d8_GetBackBufferWidth(), back_h = d3d8_GetBackBufferHeight();
    void *mapped;

    if (!d3d8_GetD3D11Device() || !tex || !tex->d3d11_texture || !back_w || !back_h)
        return E_INVALIDARG;
    if (!create())
        return E_FAIL;

    /* The destination texture is a native D3D11 object until the resource
     * layer moves behind rhi.h; wrap it for the length of the copy. */
    dst_image = rhi_d3d11_wrap_image(tex->d3d11_texture);
    memset(&rtvd, 0, sizeof rtvd);
    rtvd.format = tex->dxgi_format;
    rtvd.dim = RHI_VIEW_DIM_2D;
    rtv = dst_image ? rhi_view_create(dst_image, RHI_VIEW_RENDER_TARGET, &rtvd) : NULL;
    if (!rtv) {
        static int said;
        if (!said++)
            fprintf(stderr, "D3D8 screen copy: the destination texture is not a render "
                    "target; nothing copied\n");
        rhi_image_destroy(dst_image);
        return E_FAIL;
    }

    rhi_output_save(&saved);
    probe_source(&saved, tex, back_w, back_h);

    if ((mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)) != NULL) {
        c.scale_x = tex->width ? (float)back_w / (float)tex->width : 1.0f;
        c.scale_y = tex->height ? (float)back_h / (float)tex->height : 1.0f;
        c.pad0 = c.pad1 = 0.0f;
        memcpy(mapped, &c, sizeof c);
        rhi_buffer_unmap(g.cb);
    }

    vp.x = vp.y = 0.0f;
    vp.width = (float)tex->width;
    vp.height = (float)tex->height;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(rtv, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, g.ps);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    rhi_set_textures(9, 1, &g.back_srv);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(3, 0);

    /* The back buffer must not stay bound as a shader input while it is the
     * render target again. */
    {
        RhiView *none = NULL;
        rhi_set_textures(9, 1, &none);
    }
    rhi_output_restore(&saved);
    rhi_view_destroy(rtv);
    rhi_image_destroy(dst_image);
    g.copies++;
    return S_OK;
}

unsigned long xbox_D3D8ScreenCopyCount(void) { return g.copies; }

void xbox_D3D8ScreenCopyShutdown(void)
{
    g.back_srv = NULL;
    rhi_shader_destroy(g.vs);            g.vs = NULL;
    rhi_shader_destroy(g.ps);            g.ps = NULL;
    rhi_buffer_destroy(g.cb);            g.cb = NULL;
    rhi_blend_state_destroy(g.blend);    g.blend = NULL;
    rhi_depth_state_destroy(g.depth);    g.depth = NULL;
    rhi_raster_state_destroy(g.raster);  g.raster = NULL;
    g.tried = 0;
}
