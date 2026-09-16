/*
 * d3d8_capture -- round-trip the frame capture container.
 *
 * Writes a synthetic capture describing a couple of fake draws, a clear, a
 * transform, both kinds of state batch, a vertex shader program, a constant
 * bank, a texture and its binding; reads it back; asserts every field and
 * every payload byte survived.
 *
 * The data here is invented -- four vertices of nonsense and a 2x2 texture of
 * counting bytes. No game data is used or needed, which is the point: a
 * capture taken from a title is game content and must never be committed
 * (CLAUDE.md, Legal), so the only capture in the tree is the one this test
 * builds at run time and deletes.
 *
 * What it covers is the container: lengths, alignment, chunk order, the
 * bounds checks, and that a truncated capture stops the walk rather than
 * reading past its buffer. It does not and cannot cover whether the captured
 * values describe the frame correctly -- that needs a running title.
 *
 * No Direct3D and no Windows: this runs in the Linux CI job too.
 */
#include "d3d8_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;
static int g_failures;

static void check(int cond, const char *what)
{
    g_checks++;
    if (!cond) {
        g_failures++;
        printf("FAIL: %s\n", what);
    }
}

#define CAP_PATH "d3d8_capture_roundtrip.tmp"

/* Two fake draws' worth of vertices: a stride of 16 (XYZ + diffuse), four
 * vertices, filled with a recognisable ramp so a byte lost in the container
 * shows up as a mismatch rather than as plausible garbage. */
static const float TRI_VERTICES[4 * 4] = {
      0.0f,   0.0f, 0.5f, 1.0f,
    100.0f,   0.0f, 0.5f, 2.0f,
    100.0f, 100.0f, 0.5f, 3.0f,
      0.0f, 100.0f, 0.5f, 4.0f,
};
static const uint16_t QUAD_INDICES[6] = { 0, 1, 2, 0, 2, 3 };

/* A 2x2 texture with two mip levels, in the Xbox's own packing: level after
 * level, each rows * pitch, exactly as hle_d3d8_texture.c reads them. */
static const uint8_t TEXELS[4 * 4 + 1 * 4] = {
    0x00, 0x01, 0x02, 0x03,  0x04, 0x05, 0x06, 0x07,
    0x08, 0x09, 0x0A, 0x0B,  0x0C, 0x0D, 0x0E, 0x0F,
    0xAA, 0xBB, 0xCC, 0xDD,
};

static const uint32_t MICROCODE[2 * 4] = {
    0x00000000, 0x0020001B, 0x0836106C, 0x2F100FF8,
    0x00000000, 0x0420061B, 0x083613FC, 0x5011F819,
};
static const uint32_t DECLARATION[3] = { 0x00000000, 0x00000002, 0xFFFFFFFF };

static int write_capture(const char *path)
{
    D3D8CapWriter *w = d3d8cap_create(path, 7, 640, 480);
    D3D8CapClear clear = { 0xF1u, 0xFF203040u, 0x3F800000u, 0u };
    D3D8CapTransform xf;
    D3D8CapViewport vp = { 0u, 0u, 640u, 480u, 0.0f, 1.0f };
    D3D8CapStateBatch rs_batch = { 3u };
    /* Host values: ZENABLE on, CULLMODE none (the synthetic triangles are
     * clockwise on screen), ALPHABLENDENABLE off. */
    D3D8CapStatePair rs[3] = { { 7u, 1u }, { 22u, 1u }, { 27u, 0u } };
    D3D8CapStageBatch ts_batch = { 1u, 2u };
    D3D8CapStatePair ts[2] = { { 13u, 1u }, { 14u, 2u } };
    D3D8CapVsProgram prog = { 0x0012ABCDu | 1u, 2u, 3u };
    /* D3DFVF_XYZRHW (0x004): stride 16, pretransformed, so a replay of this
     * capture draws visible pixels without depending on the program above. */
    D3D8CapVsSelect sel = { 0x004u };
    D3D8CapVsConstants consts = { 0u, 2u };
    float const_data[2 * 4] = { 1.f, 2.f, 3.f, 4.f, 5.f, 6.f, 7.f, 8.f };
    D3D8CapTexture tex = { 1u, 0x12u, 2u, 2u, 2u, 0u };
    D3D8CapTextureLevel levels[2] = { { 8u, 2u, 16u }, { 4u, 1u, 4u } };
    D3D8CapSetTexture bind = { 0u, 1u };
    D3D8CapDrawUp up = { 5u, 4u, 16u, sizeof TRI_VERTICES };
    D3D8CapDrawIndexedUp iup = { 5u, 6u, 16u, sizeof TRI_VERTICES, sizeof QUAD_INDICES };
    int i;

    if (!w) {
        printf("FAIL: could not create %s\n", path);
        return -1;
    }
    xf.state = 1u;                                  /* PROJECTION, Xbox numbering */
    for (i = 0; i < 16; i++)
        xf.m[i] = (float)i;

    d3d8cap_chunk(w, D3D8CAP_CLEAR, &clear, sizeof clear, NULL, 0, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_TRANSFORM, &xf, sizeof xf, NULL, 0, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_VIEWPORT, &vp, sizeof vp, NULL, 0, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_RENDER_STATE, &rs_batch, sizeof rs_batch,
                  rs, sizeof rs, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_TEXTURE_STAGE_STATE, &ts_batch, sizeof ts_batch,
                  ts, sizeof ts, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_VS_PROGRAM, &prog, sizeof prog,
                  MICROCODE, sizeof MICROCODE, DECLARATION, sizeof DECLARATION);
    d3d8cap_chunk(w, D3D8CAP_VS_SELECT, &sel, sizeof sel, NULL, 0, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_VS_CONSTANTS, &consts, sizeof consts,
                  const_data, sizeof const_data, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_TEXTURE, &tex, sizeof tex,
                  levels, sizeof levels, TEXELS, sizeof TEXELS);
    d3d8cap_chunk(w, D3D8CAP_SET_TEXTURE, &bind, sizeof bind, NULL, 0, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_DRAW_UP, &up, sizeof up,
                  TRI_VERTICES, sizeof TRI_VERTICES, NULL, 0);
    d3d8cap_chunk(w, D3D8CAP_DRAW_INDEXED_UP, &iup, sizeof iup,
                  QUAD_INDICES, sizeof QUAD_INDICES,
                  TRI_VERTICES, sizeof TRI_VERTICES);

    check(d3d8cap_chunk_count(w) == 12, "twelve chunks were written");
    check(d3d8cap_close(w) == 0, "the capture closes cleanly");
    return 0;
}

static void read_capture(void)
{
    char err[128];
    D3D8CapReader *r = d3d8cap_open(CAP_PATH, err, sizeof err);
    const D3D8CapHeader *h;
    D3D8CapChunk c;
    int seen = 0, draws = 0;

    if (!r) {
        printf("FAIL: could not open %s: %s\n", CAP_PATH, err);
        g_checks++;
        g_failures++;
        return;
    }
    h = d3d8cap_header(r);
    check(h != NULL, "the header reads back");
    check(h && h->version == D3D8CAP_VERSION, "the version matches");
    check(h && h->frame == 7u, "the frame index survives");
    check(h && h->width == 640u && h->height == 480u, "the frame size survives");
    check(h && h->chunk_count == 12u, "chunk_count was patched in at close");

    while (d3d8cap_next(r, &c)) {
        seen++;
        switch (c.type) {
        case D3D8CAP_CLEAR: {
            const D3D8CapClear *p = c.data;
            check(c.bytes == sizeof *p, "the clear chunk is the right length");
            check(p->flags == 0xF1u && p->color == 0xFF203040u,
                  "the clear keeps the title's own flags and colour");
            check(p->z_bits == 0x3F800000u, "the clear depth survives as float bits");
            break;
        }
        case D3D8CAP_TRANSFORM: {
            const D3D8CapTransform *p = c.data;
            check(p->state == 1u, "the transform keeps the Xbox state number");
            check(p->m[0] == 0.0f && p->m[15] == 15.0f, "the matrix survives");
            break;
        }
        case D3D8CAP_VIEWPORT: {
            const D3D8CapViewport *p = c.data;
            check(p->width == 640u && p->max_z == 1.0f, "the viewport survives");
            break;
        }
        case D3D8CAP_RENDER_STATE: {
            const D3D8CapStateBatch *p = c.data;
            const D3D8CapStatePair *pairs =
                d3d8cap_tail(&c, sizeof *p, 3 * sizeof *pairs);
            check(p->count == 3u, "three render states were recorded");
            check(pairs != NULL, "the render state pairs are in bounds");
            check(pairs && pairs[1].state == 22u && pairs[1].value == 1u,
                  "a render state pair survives");
            break;
        }
        case D3D8CAP_TEXTURE_STAGE_STATE: {
            const D3D8CapStageBatch *p = c.data;
            const D3D8CapStatePair *pairs =
                d3d8cap_tail(&c, sizeof *p, 2 * sizeof *pairs);
            check(p->stage == 1u && p->count == 2u, "the stage batch survives");
            check(pairs && pairs[0].state == 13u, "a stage state pair survives");
            break;
        }
        case D3D8CAP_VS_PROGRAM: {
            const D3D8CapVsProgram *p = c.data;
            const uint32_t *code = d3d8cap_tail(&c, sizeof *p, sizeof MICROCODE);
            const uint32_t *decl = d3d8cap_tail(&c, sizeof *p + sizeof MICROCODE,
                                                sizeof DECLARATION);
            check(p->guest_handle == (0x0012ABCDu | 1u), "the guest handle survives");
            check(p->insn_count == 2u && p->decl_dwords == 3u,
                  "the instruction and declaration counts survive");
            check(code && memcmp(code, MICROCODE, sizeof MICROCODE) == 0,
                  "the microcode survives byte for byte");
            check(decl && memcmp(decl, DECLARATION, sizeof DECLARATION) == 0,
                  "the declaration survives byte for byte");
            break;
        }
        case D3D8CAP_VS_SELECT: {
            const D3D8CapVsSelect *p = c.data;
            check(p->guest_handle == 0x004u, "the selected FVF code survives");
            break;
        }
        case D3D8CAP_VS_CONSTANTS: {
            const D3D8CapVsConstants *p = c.data;
            const float *f = d3d8cap_tail(&c, sizeof *p, 2 * 4 * sizeof *f);
            check(p->first_reg == 0u && p->count == 2u, "the constant range survives");
            check(f && f[0] == 1.0f && f[7] == 8.0f, "the constant data survives");
            break;
        }
        case D3D8CAP_TEXTURE: {
            const D3D8CapTexture *p = c.data;
            const D3D8CapTextureLevel *lv =
                d3d8cap_tail(&c, sizeof *p, 2 * sizeof *lv);
            const uint8_t *texels =
                d3d8cap_tail(&c, sizeof *p + 2 * sizeof *lv, sizeof TEXELS);
            check(p->id == 1u && p->format == 0x12u, "the texture id and format survive");
            check(p->width == 2u && p->height == 2u && p->levels == 2u,
                  "the texture dimensions survive");
            check(lv && lv[0].bytes == 16u && lv[1].bytes == 4u,
                  "the per-level sizes survive");
            check(texels && memcmp(texels, TEXELS, sizeof TEXELS) == 0,
                  "every texel byte survives");
            break;
        }
        case D3D8CAP_SET_TEXTURE: {
            const D3D8CapSetTexture *p = c.data;
            check(p->stage == 0u && p->texture_id == 1u, "the texture binding survives");
            break;
        }
        case D3D8CAP_DRAW_UP: {
            const D3D8CapDrawUp *p = c.data;
            const void *v = d3d8cap_tail(&c, sizeof *p, sizeof TRI_VERTICES);
            draws++;
            check(p->prim == 5u && p->vertex_count == 4u && p->stride == 16u,
                  "the UP draw's primitive, count and stride survive");
            check(p->vertex_bytes == sizeof TRI_VERTICES, "the vertex length survives");
            check(v && memcmp(v, TRI_VERTICES, sizeof TRI_VERTICES) == 0,
                  "the UP draw's vertices survive byte for byte");
            break;
        }
        case D3D8CAP_DRAW_INDEXED_UP: {
            const D3D8CapDrawIndexedUp *p = c.data;
            const void *idx = d3d8cap_tail(&c, sizeof *p, sizeof QUAD_INDICES);
            const void *v = d3d8cap_tail(&c, sizeof *p + sizeof QUAD_INDICES,
                                         sizeof TRI_VERTICES);
            draws++;
            check(p->index_count == 6u && p->index_bytes == sizeof QUAD_INDICES,
                  "the indexed draw's index count survives");
            check(idx && memcmp(idx, QUAD_INDICES, sizeof QUAD_INDICES) == 0,
                  "the indices survive byte for byte");
            check(v && memcmp(v, TRI_VERTICES, sizeof TRI_VERTICES) == 0,
                  "the indexed draw's vertices survive byte for byte");
            break;
        }
        default:
            check(0, "no unexpected chunk type appears");
            break;
        }
    }
    check(seen == 12, "every chunk is walked back, in order, and the walk ends");
    check(draws == 2, "both draws come back");

    /* Replaying the same capture again must see the same chunks: the replay
     * tool's --loops does exactly this. */
    d3d8cap_rewind(r);
    seen = 0;
    while (d3d8cap_next(r, &c))
        seen++;
    check(seen == 12, "rewinding replays the same chunks");

    d3d8cap_close_read(r);
}

/* A capture from a run that died mid-frame is truncated. The walk must stop
 * at the last whole chunk instead of reading past the end of the buffer. */
static void truncated_capture(void)
{
    const char *path = "d3d8_capture_truncated.tmp";
    char err[128];
    D3D8CapReader *r;
    D3D8CapChunk c;
    unsigned char *bytes;
    long size;
    size_t got;
    int seen = 0;
    FILE *f = fopen(CAP_PATH, "rb");

    if (!f) {
        check(0, "the round-trip capture can be reopened for truncation");
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    bytes = malloc((size_t)size);
    got = bytes ? fread(bytes, 1, (size_t)size, f) : 0;
    fclose(f);
    if (!bytes || got != (size_t)size) {
        free(bytes);
        check(0, "the round-trip capture can be read for truncation");
        return;
    }

    /* Cut it off part-way through the payload of a late chunk. */
    f = fopen(path, "wb");
    if (f) {
        fwrite(bytes, 1, got - 40u, f);
        fclose(f);
    }
    free(bytes);

    r = d3d8cap_open(path, err, sizeof err);
    check(r != NULL, "a truncated capture still opens");
    if (r) {
        while (d3d8cap_next(r, &c))
            seen++;
        check(seen > 0 && seen < 12, "the walk stops at the last whole chunk");
        d3d8cap_close_read(r);
    }
    remove(path);
}

static void rejects_junk(void)
{
    const char *path = "d3d8_capture_junk.tmp";
    char err[128];
    D3D8CapReader *r;
    FILE *f = fopen(path, "wb");

    if (f) {
        fwrite("not a capture at all, but long enough to be one", 1, 48, f);
        fclose(f);
    }
    r = d3d8cap_open(path, err, sizeof err);
    check(r == NULL, "a file that is not a capture is refused");
    if (r)
        d3d8cap_close_read(r);
    remove(path);
}

int main(int argc, char **argv)
{
    /* --write <path> keeps the synthetic capture instead of testing it, so
     * the replay tool can be smoke-tested end to end without game files:
     *
     *   d3d8_capture_test --write synthetic.d3dcap
     *   d3d8_replay synthetic.d3dcap --out synthetic
     *
     * It draws invented geometry -- pretransformed triangles in the top-left
     * corner -- and exercises device creation, every chunk kind and the dump. */
    if (argc == 3 && strcmp(argv[1], "--write") == 0) {
        if (write_capture(argv[2]) != 0 || g_failures) {
            printf("could not write %s\n", argv[2]);
            return 1;
        }
        printf("wrote synthetic capture %s\n", argv[2]);
        return 0;
    }

    if (write_capture(CAP_PATH) == 0) {
        read_capture();
        truncated_capture();
    }
    rejects_junk();
    remove(CAP_PATH);

    printf("d3d8_capture: %d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
