/*
 * d3d8_postfx.c -- shaders over the finished scene: the post-process chain.
 *
 * One pixel shader runs over the whole scene image, with the scene's colour
 * and its depth buffer as inputs, and its output replaces the scene. It runs
 * at scene resolution, before the display resolve, so it is supersampled
 * with everything else at RECOMP_RES_SCALE > 1.
 *
 * Which shader. RECOMP_POSTFX / `postfx`:
 *
 *   off (default)  nothing; this file costs one branch a frame
 *   cel            the built-in cel shader below: ink lines from depth,
 *                  shading in flat bands, colour kept
 *   <file>.hlsl    a shader of your own, written against the contract below
 *
 * RECOMP_POSTFX_PARAMS / `postfx_params` sets the shader's numbers, as
 * name=value pairs separated by commas: `ink_width=1.5,bands=3`. A file's
 * numbers are p0 to p23. F8 switches the effect off and on while a title
 * runs.
 *
 * When. Ink lines and banding belong on the 3D world, not on the HUD drawn
 * over it: text with an outline traced round every letter and its colours
 * banded is unreadable. So a title can say where its world ends:
 * xbox_D3D8PostFxScene, recorded as host_PostFxScene (hle_d3d8_record.h)
 * so captures and frame interpolation's redraws run it at the same point.
 * It runs once a frame at most; a title that has said it marks its frames
 * (xbox_D3D8PostFxMarksFrames) gets no pass in a frame it did not mark --
 * a menu, a loading screen. A title that says nothing gets the pass over
 * the whole frame at its end (xbox_D3D8PostFxFrameEnd), HUD and all, which
 * is still right for an effect meant for everything (a colour grade, a CRT
 * mask).
 *
 * The contract a shader file is written against. The toolkit puts this in
 * front of the file, and the file defines ps_main:
 *
 *   cbuffer PostFx : register(b7) {
 *       float4 frame;   // scene width, height, 1/width, 1/height
 *       float4 info;    // resolution scale, seconds, depth bound (0/1), frame
 *       float4 ctl;     // the toolkit's own
 *       float4 p[6];    // postfx_params: p0 = p[0].x ... p23 = p[5].w
 *   };
 *   Texture2D<float4> scene       : register(t9);  // the scene so far
 *   Texture2D<float4> scene_depth : register(t8);  // read through these two:
 *   float scene_z(int2 px);        // depth: 0 near .. 1 far
 *   bool  scene_fresh(int2 px);    // whether this run is to shade the pixel
 *   SamplerState linear_clamp : register(s9);
 *   SamplerState point_clamp  : register(s8);
 *   struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
 *   float4 ps_main(VSOut i) : SV_Target;
 *
 * Depth is whatever depth surface is bound when the pass runs, if it is the
 * scene's size; info.z is 0 when there is none. The image is the title's
 * own depth buffer, so its values are the projection's: near 0, far 1, and
 * nonlinear.
 *
 * A frame can need more than one run. A title's 3D and 2D interleave:
 * TimeSplitters 2 draws lamp glows as 2D and then its first-person weapon.
 * After a run the mark stays armed, and 3D that reaches the scene later
 * gets a run of its own at the next 2D. A pixel is new to a run when its
 * depth changed since the last one, and only new pixels are shaded or
 * written, so nothing is shaded twice and the HUD drawn in between is left
 * alone except where the new 3D covers it. scene_depth carries that flag in
 * its sign (z for new, -z-1 for old), which is why shaders read it through
 * scene_z and scene_fresh. Without depth a frame gets one run.
 *
 * A run is four full-screen triangles at scene size: which pixels are new
 * (depth against the depth the last run saw), the effect into a second image
 * (a pass cannot read the image it writes), the new pixels back onto the
 * scene, and the depth kept for the next run. At 1280x960 that is well under
 * a millisecond.
 *
 * The built-in cel shader, and why each part:
 *
 *  - Silhouettes come from depth: a pixel is inked when a neighbour is
 *    farther away by more than a fraction of its own distance. Only the near
 *    side is inked, so the line sits on the object's edge, as drawn.
 *  - Creases come from depth too. The depth buffer holds a value that is a
 *    plane's linear function of screen position (z = A + B/w, and 1/w is
 *    affine across a projected plane), so its second difference is zero on
 *    any flat face and not zero where two faces meet. Divided by 1 - z,
 *    which is proportional to 1/w, the test is the same near and far.
 *  - Shading is banded in the light, not the texture. Brightness is split
 *    into light (the brightness of the neighbourhood, from a blur that stops
 *    at depth edges) and detail (this pixel against it). The light is
 *    banded, the detail goes back on at reduced contrast, and the colour is
 *    scaled to match, so hue and saturation survive. Banding the colour
 *    itself turned TimeSplitters 2's noisy snow and rock into blotches.
 *  - Lines on features thinner than the line (a fence's wire, snow on a
 *    branch) are thinned too, or a chain-link fence goes solid black.
 *  - Optional diagonal hatching in the darkest band, the comic-book shadow.
 *  - Ink fades with distance, so fog and far geometry are not scribbled on.
 *
 * Linear depth assumes a projection whose far plane is far beyond its near
 * one (A close to 1), which is every game's: then 1 / (1 - z) is distance
 * in units of the near plane.
 */
#include "d3d8_internal.h"
#include "recomp_config.h"

#if defined(_WIN32)

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define POSTFX_PARAMS 24

/* ================================================================
 * The shaders
 * ================================================================ */

static const char kHeader[] =
    "cbuffer PostFx : register(b7) { float4 frame; float4 info; float4 ctl; float4 p[6]; };\n"
    "Texture2D<float4> scene : register(t9);\n"
    "Texture2D<float4> scene_depth : register(t8);\n"
    "SamplerState linear_clamp : register(s9);\n"
    "SamplerState point_clamp : register(s8);\n"
    "struct VSOut { float4 pos : SV_Position; float2 uv : TEXCOORD0; };\n"
    "VSOut vs_main(uint id : SV_VertexID) {\n"
    "    float2 c = float2((id << 1) & 2, id & 2);\n"   /* one oversized triangle */
    "    VSOut o;\n"
    "    o.pos = float4(c * float2(2, -2) + float2(-1, 1), 0, 1);\n"
    "    o.uv = c;\n"
    "    return o;\n"
    "}\n"
    /* scene_depth holds depth with the sign as a flag (see the file's
     * comment): z where the pixel is new since the last run, -z-1 where it
     * is not. These two read it. */
    "float scene_z(int2 px) {\n"
    "    float d = scene_depth.Load(int3(clamp(px, int2(0, 0), int2(frame.xy) - 1), 0)).r;\n"
    "    return d >= 0 ? d : -d - 1;\n"
    "}\n"
    "bool scene_fresh(int2 px) {\n"
    "    return info.z < 0.5 || scene_depth.Load(int3(px, 0)).r >= 0;\n"
    "}\n"
    "#line 1\n";

/* Back onto the scene, where the scene is new since the last run. */
static const char kCopy[] =
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    if (!scene_fresh(int2(i.pos.xy))) discard;\n"
    "    return scene.Load(int3(i.pos.xy, 0));\n"
    "}\n";

/* Which pixels are new: the depth buffer (t8) against the one the last run
 * saw (t9). ctl.x: the first run of the frame, when all of them are. A pixel
 * whose depth went back to the far plane was cleared, not drawn. */
static const char kEncode[] =
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float z = scene_depth.Load(int3(i.pos.xy, 0)).r;\n"
    "    float was = scene.Load(int3(i.pos.xy, 0)).r;\n"
    "    bool fresh = ctl.x > 0.5 || (z != was && z < 0.999999);\n"
    "    return fresh ? z : -z - 1;\n"
    "}\n";

/* The depth this run saw, for the next one. */
static const char kSave[] =
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    return scene_depth.Load(int3(i.pos.xy, 0)).r;\n"
    "}\n";

static const char kCel[] =
    "#define BANDS       p[0].x\n"
    "#define SOFTNESS    p[0].y\n"
    "#define SATURATION  p[0].z\n"
    "#define SHADE_FLOOR p[0].w\n"
    "#define INK_WIDTH   p[1].x\n"
    "#define SILHOUETTE  p[1].y\n"
    "#define CREASE      p[1].z\n"
    "#define COLOUR_EDGE p[1].w\n"
    "#define INK_RGB     p[2].xyz\n"
    "#define INK         p[2].w\n"
    "#define SMOOTH      p[3].x\n"
    "#define HATCH       p[3].y\n"
    "#define INK_FAR     p[3].z\n"
    "#define VIEW        p[3].w\n"
    "#define LIGHT_RADIUS p[4].x\n"
    "#define DETAIL      p[4].y\n"
    "#define LIFT        p[4].z\n"
    "#define THIN_KEEP   p[4].w\n"
    "#define VIBRANCE    p[5].x\n"
    "#define WARMTH      p[5].y\n"
    "\n"
    "static const float3 LUMA = float3(0.299, 0.587, 0.114);\n"
    "\n"
    "float3 rgb_at(float2 px) {\n"
    "    return scene.Load(int3(clamp(px, 0, frame.xy - 1), 0)).rgb;\n"
    "}\n"
    "float z_at(float2 px) { return scene_z(int2(px)); }\n"
    "/* Distance in near-plane units (see the file's comment). */\n"
    "float lin(float z) { return 1.0 / max(1.0 - z, 1e-7); }\n"
    "\n"
    "/* The flattest of four overlapping 3x3 neighbourhoods, `step` pixels\n"
    " * apart: edges stay sharp while texture noise inside an area goes. */\n"
    "float3 kuwahara(float2 uv, float step) {\n"
    "    float2 t = frame.zw * step;\n"
    "    float3 best = 0; float best_v = 1e9;\n"
    "    [unroll] for (int q = 0; q < 4; q++) {\n"
    "        float2 o = float2(q & 1 ? 0 : -2, q & 2 ? 0 : -2);\n"
    "        float3 m = 0, m2 = 0;\n"
    "        [unroll] for (int y = 0; y < 3; y++)\n"
    "            [unroll] for (int x = 0; x < 3; x++) {\n"
    "                float3 c = scene.SampleLevel(linear_clamp,\n"
    "                               uv + (o + float2(x, y)) * t, 0).rgb;\n"
    "                m += c; m2 += c * c;\n"
    "            }\n"
    "        m /= 9.0;\n"
    "        float v = dot(m2 / 9.0 - m * m, 1);\n"
    "        if (v < best_v) { best_v = v; best = m; }\n"
    "    }\n"
    "    return best;\n"
    "}\n"
    "\n"
    "/* The light on this pixel, roughly: the brightness around it, from\n"
    " * two rings of samples, each counted only if it is on the same surface\n"
    " * (within 5% of the distance) so light does not bleed across edges. */\n"
    "float light_at(float2 pos, float r, float y0) {\n"
    "    if (r <= 0) return y0;\n"
    "    float dc = lin(z_at(floor(pos)));\n"
    "    float sum = y0, wsum = 1;\n"
    "    [unroll] for (int ring = 1; ring <= 2; ring++)\n"
    "        [unroll] for (int k = 0; k < 12; k++) {\n"
    "            float a = (k + 0.5 * ring) * 0.5235988;\n"
    "            float2 o = float2(cos(a), sin(a)) * r * ring * 0.5;\n"
    "            float ys = dot(scene.SampleLevel(linear_clamp, (pos + o) * frame.zw, 0).rgb, LUMA);\n"
    "            float wgt = 1;\n"
    "            if (info.z > 0)\n"
    "                wgt = saturate(1 - abs(lin(z_at(floor(pos + o))) - dc) / (dc * 0.05));\n"
    "            sum += ys * wgt; wsum += wgt;\n"
    "        }\n"
    "    return sum / wsum;\n"
    "}\n"
    "\n"
    "/* 0..1: how much of an ink line this pixel is, from depth. */\n"
    "float depth_ink(float2 px, float w, out float dist) {\n"
    "    float zc = z_at(px);\n"
    "    dist = lin(zc);\n"
    "    if (zc >= 0.999999) return 0;          /* sky: its edge is inked on the object */\n"
    "    static const float2 dirs[8] = { float2(1,0), float2(-1,0), float2(0,1), float2(0,-1),\n"
    "        float2(0.7071,0.7071), float2(-0.7071,0.7071), float2(0.7071,-0.7071),\n"
    "        float2(-0.7071,-0.7071) };\n"
    "    float sil = 0, rel[8];\n"
    "    /* Two rings, so a wide line is solid on the diagonals too. */\n"
    "    [unroll] for (int k = 0; k < 8; k++) {\n"
    "        rel[k] = (lin(z_at(px + dirs[k] * w)) - dist) / dist;\n"
    "        sil = max(sil, rel[k]);\n"
    "        sil = max(sil, (lin(z_at(px + dirs[k] * w * 0.5)) - dist) / dist);\n"
    "    }\n"
    "    sil = saturate((sil - SILHOUETTE) / SILHOUETTE);\n"
    "    /* Thinner than the line: farther on both sides of some axis. A wire\n"
    "     * in a fence, a twig, a clump of snow on a branch -- inked in full,\n"
    "     * they would all go black. */\n"
    "    float thin = 0;\n"
    "    [unroll] for (int a = 0; a < 4; a++) {\n"
    "        int i0 = a < 2 ? a * 2 : (a == 2 ? 4 : 5);\n"
    "        int i1 = a < 2 ? a * 2 + 1 : (a == 2 ? 7 : 6);\n"
    "        thin = max(thin, saturate(min(rel[i0], rel[i1]) / SILHOUETTE - 1));\n"
    "    }\n"
    "    sil *= lerp(1.0, THIN_KEEP, thin);\n"
    "    float qc = 1.0 - zc;\n"
    "    float lx = (1.0 - z_at(px + float2(-w, 0))) + (1.0 - z_at(px + float2(w, 0))) - 2 * qc;\n"
    "    float ly = (1.0 - z_at(px + float2(0, -w))) + (1.0 - z_at(px + float2(0, w))) - 2 * qc;\n"
    "    float crease = (abs(lx) + abs(ly)) / max(qc, 1e-7);\n"
    "    crease = saturate((crease - CREASE) / CREASE);\n"
    "    /* Only where the surface is continuous: across a jump in depth the\n"
    "     * second difference fires on the far side too, and the silhouette\n"
    "     * test already inks the near side. */\n"
    "    float jump = max(max(abs(rel[0]), abs(rel[1])), max(abs(rel[2]), abs(rel[3])));\n"
    "    crease *= saturate(1.0 - jump / SILHOUETTE);\n"
    "    return max(sil, crease * lerp(1.0, THIN_KEEP, thin));\n"
    "}\n"
    "\n"
    "/* 0..1: ink from a strong change in brightness, for edges depth cannot see\n"
    " * (a painted line, a decal, 2D drawn before the mark). */\n"
    "float colour_ink(float2 px, float w) {\n"
    "    if (COLOUR_EDGE <= 0) return 0;\n"
    "    float tl = dot(rgb_at(px + float2(-w, -w)), LUMA), t = dot(rgb_at(px + float2(0, -w)), LUMA);\n"
    "    float tr = dot(rgb_at(px + float2(w, -w)), LUMA),  l = dot(rgb_at(px + float2(-w, 0)), LUMA);\n"
    "    float r = dot(rgb_at(px + float2(w, 0)), LUMA),    bl = dot(rgb_at(px + float2(-w, w)), LUMA);\n"
    "    float b = dot(rgb_at(px + float2(0, w)), LUMA),    br = dot(rgb_at(px + float2(w, w)), LUMA);\n"
    "    float gx = (tr + 2 * r + br) - (tl + 2 * l + bl);\n"
    "    float gy = (bl + 2 * b + br) - (tl + 2 * t + tr);\n"
    "    return saturate((length(float2(gx, gy)) - COLOUR_EDGE) / COLOUR_EDGE);\n"
    "}\n"
    "\n"
    "float4 ps_main(VSOut i) : SV_Target {\n"
    "    float2 px = floor(i.pos.xy);\n"
    "    float scale = max(info.x, 1);\n"
    "    float w = max(INK_WIDTH * scale, 1);\n"
    "    float4 src = scene.Load(int3(px, 0));\n"
    "    if (!scene_fresh(int2(px))) return src;   /* drawn by an earlier run */\n"
    "\n"
    "    /* Shading in bands. The brightness is split into light, the\n"
    "     * brightness of the neighbourhood (a blur that stops at depth edges),\n"
    "     * and detail, this pixel against it: the texture. The light is\n"
    "     * banded; the detail goes back on at DETAIL of its contrast. */\n"
    "    float3 c = SMOOTH > 0 ? kuwahara(i.pos.xy * frame.zw, SMOOTH * scale) : src.rgb;\n"
    "    float y = dot(c, LUMA);\n"
    "    float light = light_at(i.pos.xy, LIGHT_RADIUS * scale, y);\n"
    "    float detail = clamp(y / max(light, 1e-3), 0, 4);\n"
    "    /* Lift raises the darks and mid-tones and leaves the top where it\n"
    "     * was: a sunlit map is bright already, and a title's glow (TS2's\n"
    "     * doubles what is bright) would blow a lifted one out. */\n"
    "    float lit = saturate(light);\n"
    "    lit = lerp(pow(lit, 1.0 / (1.0 + LIFT)), lit, smoothstep(0.45, 0.85, lit));\n"
    "    float x = lit * BANDS;\n"
    "    float band = floor(x);\n"
    "    float f = x - band;\n"
    "    float edge = fwidth(x) + SOFTNESS * 0.5;\n"
    "    float level = (band + smoothstep(0.5 - edge, 0.5 + edge, f)) / BANDS;\n"
    "    level = lerp(SHADE_FLOOR, 1.0, saturate(level));\n"
    "    float y2 = level * pow(detail, DETAIL);\n"
    "    float3 shaded = c * (y2 / max(y, 1e-4));\n"
    /* All three colour controls hold back where the colour is already
     * there. A sunlit desert map is warm and saturated as the title drew it;
     * pushed by the same amounts as Siberia's grey snow it went red. */
    "    float mx = max(shaded.r, max(shaded.g, shaded.b));\n"
    "    float mn = min(shaded.r, min(shaded.g, shaded.b));\n"
    "    float room = 1.0 - saturate((mx - mn) / max(mx, 1e-4));   /* 0: fully saturated */\n"
    "    shaded = lerp(dot(shaded, LUMA), shaded, 1.0 + (SATURATION - 1.0) * room);\n"
    "    /* Vibrance: more colour where there is a little, none on white or\n"
    "     * grey (snow and highlights stay clean), less as colour gets rich. */\n"
    "    if (VIBRANCE != 0) {\n"
    "        float ys = dot(shaded, LUMA);\n"
    "        float chroma = max(shaded.r, max(shaded.g, shaded.b)) - min(shaded.r, min(shaded.g, shaded.b));\n"
    "        float white = smoothstep(0.6, 0.9, ys) * (1.0 - saturate(chroma * 4.0));\n"
    "        float keep = (1.0 - white) * saturate(chroma * 10.0) * room * room;\n"
    "        shaded = lerp(ys, shaded, 1.0 + VIBRANCE * keep);\n"
    "    }\n"
    "    /* Warmth: the grade toward ochre and gold of a painted comic (XIII),\n"
    "     * on the lit mid-tones of cool and neutral colours: the darks stay\n"
    "     * dark under the ink, white stays white, and what is warm already\n"
    "     * (sand, wood, fire) is left as it is. */\n"
    "    if (WARMTH != 0) {\n"
    "        float yw = dot(shaded, LUMA);\n"
    "        float mw = max(shaded.r, max(shaded.g, shaded.b));\n"
    "        float cw = mw - min(shaded.r, min(shaded.g, shaded.b));\n"
    "        float whiteish = smoothstep(0.35, 0.65, yw) * (1.0 - saturate(cw * 3.0));\n"
    "        float warmed = saturate((shaded.r - shaded.b) / max(mw, 1e-4) * 2.5);\n"
    "        float3 warm = float3(1.0 + 0.10 * WARMTH, 1.0 + 0.02 * WARMTH, 1.0 - 0.15 * WARMTH);\n"
    "        float amount = smoothstep(0.05, 0.4, yw) * (1.0 - whiteish) * (1.0 - warmed);\n"
    "        shaded = shaded * lerp(1.0, warm, amount);\n"
    "    }\n"
    "    /* Out of range: scale the colour down rather than clip it. Clipping\n"
    "     * one channel shifts the hue: an over-bright orange clips to red. */\n"
    "    shaded = max(shaded, 0) / max(1.0, max(shaded.r, max(shaded.g, shaded.b)));\n"
    "\n"
    "    /* Hatching in the darkest band: diagonal strokes, 4 guest pixels apart. */\n"
    "    if (HATCH > 0) {\n"
    "        float dark = saturate(1.0 - (level - SHADE_FLOOR) / max(1.5 / BANDS, 1e-3));\n"
    "        float d = frac((px.x + px.y) / (4.0 * scale));\n"
    "        float stroke = 1.0 - smoothstep(0.18, 0.18 + 1.0 / (4.0 * scale), abs(d - 0.5));\n"
    "        shaded *= 1.0 - HATCH * dark * stroke;\n"
    "    }\n"
    "\n"
    "    /* Ink. */\n"
    "    float dist = 1;\n"
    "    float ink = 0;\n"
    "    if (info.z > 0) {\n"
    "        ink = depth_ink(px, w, dist);\n"
    "        if (INK_FAR > 0) ink *= saturate(2.0 - dist / INK_FAR);\n"
    "    }\n"
    "    ink = max(ink, colour_ink(px, w)) * INK;\n"
    "    float3 outc = lerp(shaded, INK_RGB, saturate(ink));\n"
    "\n"
    "    /* Views for tuning (view=1..4): distance, ink alone, bands alone,\n"
    "     * the raw depth buffer. */\n"
    "    if (VIEW >= 0.5 && VIEW < 1.5) outc = frac(log2(max(dist, 1)) / 4.0);\n"
    "    else if (VIEW >= 1.5 && VIEW < 2.5) outc = 1.0 - saturate(ink);\n"
    "    else if (VIEW >= 2.5 && VIEW < 3.5) outc = shaded;\n"
    "    else if (VIEW >= 3.5 && VIEW < 4.5) outc = z_at(px);\n"
    "    return float4(outc, src.a);\n"
    "}\n";

/* The built-in cel shader's numbers, by name, and their defaults. */
typedef struct { const char *name; float value; } ParamName;

static const ParamName kCelParams[POSTFX_PARAMS] = {
    { "bands",        4.0f },   /* light levels */
    { "softness",     0.10f },  /* of a band: how soft its edge */
    { "saturation",   1.15f },  /* colour after banding: 1 leaves it */
    { "shade_floor",  0.12f },  /* the darkest band's brightness */
    { "ink_width",    1.0f },   /* in guest pixels */
    { "silhouette",   0.10f },  /* depth step, as a fraction of distance */
    { "crease",       0.08f },  /* depth curvature that counts as a corner */
    { "colour_edge",  0.0f },   /* 0 off; else the brightness step that is inked */
    { "ink_r",        0.06f },
    { "ink_g",        0.05f },
    { "ink_b",        0.08f },
    { "ink",          1.0f },   /* 0 no lines .. 1 solid */
    { "smooth",       0.0f },   /* Kuwahara step in guest pixels; 0 off */
    { "hatch",        0.0f },   /* 0 off .. 1 black strokes */
    { "ink_far",      2000.0f },/* distance (near planes) where ink starts to fade; 0 never */
    { "view",         0.0f },   /* 1 distance, 2 ink, 3 bands, 4 raw depth */
    { "light_radius", 6.0f },   /* guest pixels the light is averaged over; 0 bands colour */
    { "detail",       0.6f },   /* texture contrast kept: 1 all, 0 flat colour */
    { "lift",         0.0f },   /* brightens the light before banding: 0 leaves it */
    { "thin",         0.15f },  /* ink kept on features thinner than the line */
    { "vibrance",     0.0f },   /* more colour on colourful pixels, none on white or grey */
    { "warmth",       0.0f },   /* lit tones toward ochre and gold; negative cools */
    { "p22", 0 }, { "p23", 0 },
};

/* ================================================================
 * Settings
 * ================================================================ */

enum { FX_OFF = 0, FX_CEL, FX_FILE };

static struct {
    int         read;
    int         kind;               /* what the settings asked for */
    int         on;                 /* F8 */
    int         marks;              /* the title marks its frames */
    int         armed;              /* the title marked this frame */
    int         seen_3d;            /* 3D has reached the scene since the last run */
    int         runs;               /* runs this frame */
    int         depth_ok;           /* they could tell new pixels from old */
    char        path[512];
    float       params[POSTFX_PARAMS];
    unsigned    frames;
} s;

static int param_index(const char *name)
{
    int k;

    if ((name[0] == 'p' || name[0] == 'P') && name[1] >= '0' && name[1] <= '9') {
        k = atoi(name + 1);
        return k < POSTFX_PARAMS ? k : -1;
    }
    if (s.kind == FX_CEL)
        for (k = 0; k < POSTFX_PARAMS; k++)
            if (_stricmp(name, kCelParams[k].name) == 0)
                return k;
    return -1;
}

static void read_params(const char *list)
{
    char buf[1024], *item, *next;

    snprintf(buf, sizeof buf, "%s", list);
    for (item = buf; item && *item; item = next) {
        char *eq;
        int k;

        next = strchr(item, ',');
        if (next) *next++ = 0;
        while (*item == ' ') item++;
        eq = strchr(item, '=');
        if (!eq) continue;
        *eq = 0;
        {
            char *end = eq - 1;
            while (end > item && *end == ' ') *end-- = 0;
        }
        k = param_index(item);
        if (k < 0) {
            fprintf(stderr, "D3D8 postfx: no parameter called %s; ignored\n", item);
            continue;
        }
        s.params[k] = (float)atof(eq + 1);
    }
}

/* The built-in shader's defaults, then the title's (xbox_D3D8PostFxSetDefaults),
 * then the player's postfx_params, each over the one before. */
static char g_title_defaults[512];

static void apply_params(void)
{
    const char *v;
    int k;

    for (k = 0; k < POSTFX_PARAMS; k++)
        s.params[k] = s.kind == FX_CEL ? kCelParams[k].value : 0.0f;
    if (s.kind == FX_CEL && g_title_defaults[0])
        read_params(g_title_defaults);
    v = recomp_config_lookup("RECOMP_POSTFX_PARAMS", "postfx_params");
    if (v && *v)
        read_params(v);
}

void xbox_D3D8PostFxSetDefaults(const char *params)
{
    snprintf(g_title_defaults, sizeof g_title_defaults, "%s", params ? params : "");
    if (s.read)
        apply_params();
}

static void read_settings(void)
{
    const char *v;

    if (s.read) return;
    s.read = 1;
    v = recomp_config_lookup("RECOMP_POSTFX", "postfx");
    if (!v || !*v || _stricmp(v, "off") == 0 || strcmp(v, "0") == 0) {
        s.kind = FX_OFF;
    } else if (_stricmp(v, "cel") == 0) {
        s.kind = FX_CEL;
    } else {
        s.kind = FX_FILE;
        snprintf(s.path, sizeof s.path, "%s", v);
    }
    apply_params();
    s.on = s.kind != FX_OFF;
    if (s.kind != FX_OFF)
        fprintf(stderr, "D3D8 postfx: %s over the scene%s. F8 switches it off and on.\n",
                s.kind == FX_CEL ? "cel shading" : s.path,
                s.marks ? ", before the title's 2D" : "");
}

/* ================================================================
 * GPU objects
 * ================================================================ */

typedef struct {
    float frame[4];
    float info[4];
    float ctl[4];                       /* x: the first run of the frame */
    float p[POSTFX_PARAMS];
} PostFxConstants;

/* A scene-sized image the passes draw into and read back. */
typedef struct {
    RhiImage *img;
    RhiView  *rtv, *srv;
} Target;

static struct {
    RhiShader      *vs, *ps, *copy_ps, *encode_ps, *save_ps;
    RhiBuffer      *cb;
    RhiBlendState  *blend;
    RhiDepthState  *depth;
    RhiRasterState *raster;
    RhiSampler     *linear, *point;
    Target          work;               /* the effect's output, RGBA8 */
    Target          enc;                /* depth, sign-flagged new or not (R32F) */
    Target          seen;               /* depth when the last run ended (R32F) */
    UINT            tw, th;
    RhiImage       *depth_img;          /* held, while depth_srv views it */
    RhiView        *depth_srv;
    int             tried, failed;
    LARGE_INTEGER   qpf, start;
} g;

static void give_up(const char *what, const char *detail)
{
    g.failed = 1;
    fprintf(stderr, "D3D8 postfx: %s failed%s%s; the scene is shown without it\n",
            what, detail && *detail ? ": " : "", detail ? detail : "");
    fflush(stderr);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (n >= 0) ? (char *)malloc((size_t)n + 1) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    if (buf) { buf[n] = 0; *len = (size_t)n; }
    return buf;
}

static RhiShader *compile(uint32_t stage, const char *body, const char *name,
                          const char *entry, const char *target)
{
    size_t hl = sizeof kHeader - 1, bl = strlen(body);
    char *src = (char *)malloc(hl + bl + 1);
    char err[4096];
    RhiShaderSource ss;
    RhiShader *sh;

    if (!src) return NULL;
    memcpy(src, kHeader, hl);
    memcpy(src + hl, body, bl + 1);
    memset(&ss, 0, sizeof ss);
    ss.hlsl = src;
    ss.len = hl + bl;
    ss.name = name;
    ss.entry = entry;
    ss.target = target;
    ss.optimize = 1;
    err[0] = 0;
    sh = rhi_shader_create(stage, &ss, err, sizeof err);
    if (!sh)
        give_up(name, err);
    free(src);
    return sh;
}

static int create(void)
{
    RhiBufferDesc bd;
    RhiBlendDesc bl;
    RhiDepthDesc ds;
    RhiRasterDesc rd;
    RhiSamplerDesc sm;
    const char *body = kCel, *name = "postfx_cel";
    char *file = NULL;
    size_t len;

    if (g.failed) return 0;
    if (g.tried) return 1;
    g.tried = 1;
    QueryPerformanceFrequency(&g.qpf);
    QueryPerformanceCounter(&g.start);

    if (s.kind == FX_FILE) {
        file = read_file(s.path, &len);
        if (!file) { give_up("reading the shader", s.path); return 0; }
        body = file;
        name = s.path;
    }
    g.vs = compile(RHI_STAGE_VERTEX, kCopy, "postfx_vs", "vs_main", "vs_5_0");
    g.ps = g.vs ? compile(RHI_STAGE_PIXEL, body, name, "ps_main", "ps_5_0") : NULL;
    g.copy_ps = g.ps ? compile(RHI_STAGE_PIXEL, kCopy, "postfx_copy", "ps_main", "ps_5_0") : NULL;
    g.encode_ps = g.copy_ps ? compile(RHI_STAGE_PIXEL, kEncode, "postfx_encode", "ps_main",
                                      "ps_5_0") : NULL;
    g.save_ps = g.encode_ps ? compile(RHI_STAGE_PIXEL, kSave, "postfx_save", "ps_main",
                                      "ps_5_0") : NULL;
    free(file);
    if (!g.save_ps) return 0;

    memset(&bd, 0, sizeof bd);
    bd.size = sizeof(PostFxConstants);
    bd.usage = RHI_USAGE_DYNAMIC;
    bd.bind = RHI_BIND_UNIFORM;
    bd.cpu_access = RHI_CPU_WRITE;
    if (!(g.cb = rhi_buffer_create(&bd, NULL))) { give_up("CreateBuffer", NULL); return 0; }

    memset(&bl, 0, sizeof bl);
    bl.write_mask = RHI_WRITE_ALL;
    memset(&ds, 0, sizeof ds);
    memset(&rd, 0, sizeof rd);
    rd.fill = RHI_FILL_SOLID;
    rd.cull = RHI_CULL_NONE;
    rd.depth_clip = 1;
    if (!(g.blend = rhi_blend_state_create(&bl)) || !(g.depth = rhi_depth_state_create(&ds)) ||
        !(g.raster = rhi_raster_state_create(&rd))) {
        give_up("state creation", NULL);
        return 0;
    }

    memset(&sm, 0, sizeof sm);
    sm.address_u = sm.address_v = sm.address_w = RHI_ADDRESS_CLAMP;
    sm.compare = RHI_CMP_NEVER;
    sm.max_lod = 3.402823466e+38f;
    sm.filter = RHI_FILTER_LINEAR;
    g.linear = rhi_sampler_create(&sm);
    sm.filter = RHI_FILTER_POINT;
    g.point = rhi_sampler_create(&sm);
    if (!g.linear || !g.point) { give_up("CreateSamplerState", NULL); return 0; }
    return 1;
}

static void target_destroy(Target *t)
{
    rhi_view_destroy(t->srv);  t->srv = NULL;
    rhi_view_destroy(t->rtv);  t->rtv = NULL;
    rhi_image_destroy(t->img); t->img = NULL;
}

static int target_make(Target *t, UINT w, UINT h, RhiFormat format)
{
    RhiImageDesc d;

    memset(&d, 0, sizeof d);
    d.type = RHI_IMAGE_2D;
    d.width = w;
    d.height = h;
    d.depth = 1;
    d.mip_levels = 1;
    d.format = format;
    d.samples = 1;
    d.usage = RHI_USAGE_DEFAULT;
    d.bind = RHI_BIND_RENDER_TARGET | RHI_BIND_SAMPLED;
    return (t->img = rhi_image_create(&d, NULL)) != NULL &&
           (t->rtv = rhi_view_create(t->img, RHI_VIEW_RENDER_TARGET, NULL)) != NULL &&
           (t->srv = rhi_view_create(t->img, RHI_VIEW_SAMPLED, NULL)) != NULL;
}

static void targets_destroy(void)
{
    target_destroy(&g.work);
    target_destroy(&g.enc);
    target_destroy(&g.seen);
    g.tw = g.th = 0;
}

static int targets_for(UINT w, UINT h)
{
    if (g.work.img && g.tw == w && g.th == h)
        return 1;
    targets_destroy();
    if (target_make(&g.work, w, h, RHI_FORMAT_R8G8B8A8_UNORM) &&
        target_make(&g.enc, w, h, RHI_FORMAT_R32_FLOAT) &&
        target_make(&g.seen, w, h, RHI_FORMAT_R32_FLOAT)) {
        g.tw = w;
        g.th = h;
        return 1;
    }
    targets_destroy();
    give_up("the pass's images", NULL);
    return 0;
}

static void depth_release(void)
{
    rhi_view_destroy(g.depth_srv);  g.depth_srv = NULL;
    rhi_image_destroy(g.depth_img); g.depth_img = NULL;
}

/* A sampled view of the depth image, kept while it is the same image. The
 * image is retained so its address cannot be reused by another while the
 * view is cached. */
static RhiView *depth_view_of(RhiImage *img)
{
    if (img == g.depth_img)
        return g.depth_srv;
    depth_release();
    if (!img)
        return NULL;
    g.depth_img = rhi_image_retain(img);
    g.depth_srv = rhi_view_create(img, RHI_VIEW_SAMPLED, NULL);
    if (!g.depth_srv) {
        static int said;
        if (!said++)
            fprintf(stderr, "D3D8 postfx: the depth buffer cannot be sampled; "
                    "effects run without depth\n");
    }
    return g.depth_srv;
}

static void draw(RhiView *target, RhiShader *ps, RhiView *colour, RhiView *depth,
                 UINT w, UINT h)
{
    RhiViewport vp;
    float blend_factor[4] = { 1, 1, 1, 1 };
    RhiView *views[2];
    RhiSampler *samplers[2];

    vp.x = vp.y = 0.0f;
    vp.width = (float)w;
    vp.height = (float)h;
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    rhi_set_render_target(target, NULL);
    rhi_set_viewports(1, &vp);
    rhi_set_vertex_layout(NULL);
    rhi_set_topology(RHI_TOPOLOGY_TRIANGLES);
    rhi_set_shader(RHI_STAGE_VERTEX, g.vs);
    rhi_set_shader(RHI_STAGE_PIXEL, ps);
    rhi_set_uniform_buffers(RHI_STAGE_PIXEL, 7, 1, &g.cb);
    views[0] = depth;                  /* t8 */
    views[1] = colour;                 /* t9 */
    rhi_set_textures(8, 2, views);
    samplers[0] = g.point;
    samplers[1] = g.linear;
    rhi_set_samplers(8, 2, samplers);
    rhi_set_blend_state(g.blend, blend_factor, 0xFFFFFFFF);
    rhi_set_depth_state(g.depth, 0);
    rhi_set_raster_state(g.raster);
    rhi_draw(3, 0);
    views[0] = views[1] = NULL;
    rhi_set_textures(8, 2, views);
}

/* One run over what is new in the scene. first: nothing in the frame has
 * been through the pass yet. Returns whether depth was there to tell new
 * from old -- without it a frame gets one run, over everything. */
static int run(int first)
{
    D3D8SceneTargets t;
    RhiOutputState saved;
    RhiView *depth;
    void *mapped;

    if (!d3d8_scene_targets(&t) || !create() || !targets_for(t.width, t.height))
        return 0;
    depth = depth_view_of(t.depth);

    if ((mapped = rhi_buffer_map(g.cb, RHI_MAP_WRITE_DISCARD)) != NULL) {
        PostFxConstants c;
        LARGE_INTEGER now;

        QueryPerformanceCounter(&now);
        c.frame[0] = (float)t.width;
        c.frame[1] = (float)t.height;
        c.frame[2] = 1.0f / (float)t.width;
        c.frame[3] = 1.0f / (float)t.height;
        c.info[0] = (float)t.scale;
        c.info[1] = g.qpf.QuadPart
                  ? (float)((double)(now.QuadPart - g.start.QuadPart) / (double)g.qpf.QuadPart)
                  : 0.0f;
        c.info[2] = depth ? 1.0f : 0.0f;
        c.info[3] = (float)s.frames;
        c.ctl[0] = first ? 1.0f : 0.0f;
        c.ctl[1] = c.ctl[2] = c.ctl[3] = 0.0f;
        memcpy(c.p, s.params, sizeof c.p);
        memcpy(mapped, &c, sizeof c);
        rhi_buffer_unmap(g.cb);
    }

    /* RECOMP_POSTFX_TRACE=1: say when the pass runs, which with d3d8_replay
     * --list-draws puts it after the draw that set it off. */
    {
        static int trace = -1;
        if (trace < 0)
            trace = recomp_config_bool("RECOMP_POSTFX_TRACE", NULL, 0);
        if (trace)
            fprintf(stderr, "[postfx] pass runs (frame %u, %s run, depth %s)\n", s.frames,
                    first ? "first" : "a later", depth ? "bound" : "none");
    }

    rhi_output_save(&saved);
    if (depth)
        draw(g.enc.rtv, g.encode_ps, g.seen.srv, depth, t.width, t.height);
    draw(g.work.rtv, g.ps, t.srv, depth ? g.enc.srv : NULL, t.width, t.height);
    draw(t.rtv, g.copy_ps, g.work.srv, depth ? g.enc.srv : NULL, t.width, t.height);
    if (depth)
        draw(g.seen.rtv, g.save_ps, NULL, depth, t.width, t.height);
    rhi_output_restore(&saved);
    return depth != NULL;
}

/* ================================================================
 * The title's side
 * ================================================================ */

void xbox_D3D8PostFxMarksFrames(BOOL on)
{
    s.marks = on ? 1 : 0;
}

/* The title's mark arms the pass, and it runs at the next screen-space draw
 * to the scene that follows 3D (d3d8_postfx_before_draw), or at the frame's
 * end. A title knows where its 2D starts, but not always where its 3D does:
 * TimeSplitters 2 reserves 2D for the handheld's screen before its world is
 * drawn, and draws glows on lamps as 2D before its first-person weapon.
 * So the mark stays armed: 3D that reaches the scene after a run (the
 * weapon) gets a run of its own at the next 2D, over only the pixels whose
 * depth it changed, and nothing is shaded twice. */
void xbox_D3D8PostFxScene(void)
{
    read_settings();
    if (s.on && !g.failed)
        s.armed = 1;
}

BOOL d3d8_postfx_before_draw(BOOL to_scene, BOOL screen_space)
{
    if (!to_scene)
        return FALSE;
    if (!screen_space) {
        s.seen_3d = 1;
        return FALSE;
    }
    if (!s.armed || !s.seen_3d || (s.runs && !s.depth_ok) || !s.on || g.failed)
        return FALSE;
    s.depth_ok = run(s.runs == 0);
    s.runs++;
    s.seen_3d = 0;
    return TRUE;
}

void xbox_D3D8PostFxFrameEnd(void)
{
    read_settings();
    if (s.on && !g.failed) {
        if (s.armed) {
            if (s.seen_3d && (!s.runs || s.depth_ok))
                run(s.runs == 0);
        } else if (!s.marks && !s.runs) {
            run(1);
        }
    }
    s.armed = s.seen_3d = s.runs = s.depth_ok = 0;
    s.frames++;
}

void xbox_D3D8PostFxToggle(void)
{
    read_settings();
    if (s.kind == FX_OFF) {
        /* Nothing chosen: F8 tries the built-in one, as the title tunes it. */
        s.kind = FX_CEL;
        apply_params();
    }
    s.on = !s.on;
    fprintf(stderr, "D3D8 postfx: %s\n", s.on ? "on" : "off");
}

void xbox_D3D8PostFxShutdown(void)
{
    targets_destroy();
    depth_release();
    rhi_shader_destroy(g.vs);           g.vs = NULL;
    rhi_shader_destroy(g.ps);           g.ps = NULL;
    rhi_shader_destroy(g.copy_ps);      g.copy_ps = NULL;
    rhi_shader_destroy(g.encode_ps);    g.encode_ps = NULL;
    rhi_shader_destroy(g.save_ps);      g.save_ps = NULL;
    rhi_buffer_destroy(g.cb);           g.cb = NULL;
    rhi_blend_state_destroy(g.blend);   g.blend = NULL;
    rhi_depth_state_destroy(g.depth);   g.depth = NULL;
    rhi_raster_state_destroy(g.raster); g.raster = NULL;
    rhi_sampler_destroy(g.linear);      g.linear = NULL;
    rhi_sampler_destroy(g.point);       g.point = NULL;
    g.tried = g.failed = 0;
}

#endif /* _WIN32 */
