/*
 * d3d8_capture.h -- the D3D8 frame capture container: one frame of the
 * title's D3D8 calls plus every byte they read.
 *
 * Why this exists: reaching an interesting frame in shadow mode
 * (hle_d3d8.c, RECOMP_HLE_D3D8=shadow) means running the title for minutes
 * and driving menus, and what the renderer does is then never quite the same
 * twice. A capture is the same frame every time, replays in seconds from
 * src/replay with no game running, and so turns a change to the shader
 * translation into an image to look at rather than another play-through.
 *
 * This file is the container only: no Direct3D, no Windows, no guest memory.
 * It builds on every platform so the round-trip test (tests/d3d8_capture)
 * runs in the Linux CI job as well as the Windows one. The guest side that
 * fills it is hle_d3d8_capture.c, which is Windows-only like the rest of
 * shadow mode; the reader side is src/replay.
 *
 * ------------------------------------------------------------------ layout
 *
 * Little-endian throughout, and no packing pragmas: every field of every
 * payload struct below is a 4-byte uint32_t or float, so each struct is the
 * same size and shape on MSVC x64 and gcc x86-64. That is the whole of the
 * portability argument -- the guest is x86-32 LE and both hosts are x86-64
 * LE (CLAUDE.md), so nothing is byte-swapped anywhere. A capture written on
 * one host and read on the other is byte-identical; a capture is NOT a
 * long-term archive format, and the version is checked exactly, not ranged.
 *
 *   D3D8CapHeader                      (32 bytes, at offset 0)
 *   then chunk_count chunks, in the order the title made the calls:
 *     uint32_t type;                   D3D8CAP_* below
 *     uint32_t bytes;                  payload length, NOT counting these 8
 *     uint8_t  payload[bytes];
 *     uint8_t  pad[];                  to the next 4-byte boundary
 *
 * Chunks are in call order and replay walks them in call order: the format
 * carries no random access and no index, because a frame is replayed whole.
 * Textures and vertex shader programs are the exception to pure call order --
 * they are emitted once, the first time the frame refers to them, and then
 * referred to by id, so a texture bound by forty draws is stored once.
 *
 * Not in the format, deliberately:
 *   - pixel shaders / register combiner state. The title's combiner state is
 *     not forwarded to the host device yet (hle_d3d8_state.c lists the pixel
 *     shader states among what it does not send), so there is nothing to
 *     record that replay could use.
 *   - lights and materials, for the same reason: shadow mode holds LIGHTING
 *     off because it forwards neither.
 *   - push-buffer traffic. A title that fills its own push buffer through
 *     BeginPush (Burnout 2 does, at two call sites -- see hle_d3d8.c) bypasses
 *     the replacements entirely, so those draws are invisible here. A capture
 *     of such a frame is silently incomplete, which is the format's main
 *     known limitation.
 *   - anything time-varying: no timestamps, no frame pacing. A capture is a
 *     frame's worth of state and data, not a recording of a run.
 */
#ifndef XBOXRECOMP_D3D8_CAPTURE_H
#define XBOXRECOMP_D3D8_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "XBRD3D8", NUL-terminated, so `head -c8` on a capture is readable. */
#define D3D8CAP_MAGIC        "XBRD3D8"
#define D3D8CAP_MAGIC_BYTES  8

/* Bumped on any change to a payload struct or chunk meaning. The reader
 * refuses anything else rather than guessing: captures are cheap to retake. */
#define D3D8CAP_VERSION      1u

/* The conventional extension. .gitignore has it: a capture contains the
 * title's own textures and vertices, so it is game content and must never be
 * committed (CLAUDE.md, Legal). */
#define D3D8CAP_EXTENSION    ".d3dcap"

enum {
    D3D8CAP_END                 = 0,  /* optional terminator; readers stop at EOF too */
    D3D8CAP_CLEAR               = 1,  /* D3D8CapClear */
    D3D8CAP_TRANSFORM           = 2,  /* D3D8CapTransform */
    D3D8CAP_VIEWPORT            = 3,  /* D3D8CapViewport */
    D3D8CAP_RENDER_STATE        = 4,  /* D3D8CapStateBatch + D3D8CapStatePair[] */
    D3D8CAP_TEXTURE_STAGE_STATE = 5,  /* D3D8CapStageBatch + D3D8CapStatePair[] */
    D3D8CAP_VS_PROGRAM          = 6,  /* D3D8CapVsProgram + microcode + declaration */
    D3D8CAP_VS_SELECT           = 7,  /* D3D8CapVsSelect */
    D3D8CAP_VS_CONSTANTS        = 8,  /* D3D8CapVsConstants + float4[count] */
    D3D8CAP_TEXTURE             = 9,  /* D3D8CapTexture + D3D8CapTextureLevel[] + texels */
    D3D8CAP_SET_TEXTURE         = 10, /* D3D8CapSetTexture */
    D3D8CAP_DRAW_UP             = 11, /* D3D8CapDrawUp + vertices */
    D3D8CAP_DRAW_INDEXED_UP     = 12  /* D3D8CapDrawIndexedUp + indices + vertices */
};

typedef struct {
    char     magic[D3D8CAP_MAGIC_BYTES];  /* D3D8CAP_MAGIC */
    uint32_t version;                     /* D3D8CAP_VERSION */
    uint32_t header_bytes;                /* sizeof(D3D8CapHeader); chunks start here */
    uint32_t frame;                       /* which swap this was, counting from 1 */
    uint32_t width, height;               /* the shadow device's back buffer */
    uint32_t chunk_count;                 /* content chunks, excluding the END
                                           * terminator; patched in at close,
                                           * and 0 in a capture whose run died
                                           * before it closed */
} D3D8CapHeader;

/* Xbox D3DCLEAR_* flags and the raw D3DCOLOR, exactly as the title passed
 * them: the Xbox-to-PC flag mapping lives in hle_d3d8.c and is applied at
 * replay, so a capture keeps the guest's own values and a fix to that mapping
 * changes what an existing capture draws. z is the float's bits, because the
 * guest passed it as a stack DWORD. */
typedef struct { uint32_t flags, color, z_bits, stencil; } D3D8CapClear;

/* state is the Xbox D3DTRANSFORMSTATETYPE (VIEW 0, PROJECTION 1, TEXTURE0-3
 * 2-5, WORLD-WORLD3 6-9); hle_d3d8.c maps it to the PC numbering at replay. */
typedef struct { uint32_t state; float m[16]; } D3D8CapTransform;

typedef struct { uint32_t x, y, width, height; float min_z, max_z; } D3D8CapViewport;

/* Render and texture stage states are stored as HOST values, already through
 * the converters in hle_d3d8_state.c. They are what that file actually set on
 * the device, captured at the point it set them, so a capture cannot be used
 * to re-examine the Xbox-to-PC state conversion -- only what came out of it.
 * The alternative, storing the guest arrays raw, would make a capture depend
 * on the title's XDK layout being known at replay time, which is exactly the
 * per-title coupling a capture is meant to remove. */
typedef struct { uint32_t count; } D3D8CapStateBatch;
typedef struct { uint32_t stage, count; } D3D8CapStageBatch;
typedef struct { uint32_t state, value; } D3D8CapStatePair;

/* A vertex shader the title created. guest_handle is the address of its
 * X_D3DVertexShader with bit 0 set, the handle the title then selects by.
 * The microcode is insn_count NV2A instructions of 4 DWORDs (hle_d3d8.c,
 * D3DDevice_CreateVertexShader), and is what replay recreates the host
 * program from. decl_dwords is the declaration token stream that followed, as
 * DWORDs; it is recorded for completeness and is not yet consumed at replay,
 * because the host CreateVertexShader ignores pDeclaration (d3d8_device.c). */
typedef struct { uint32_t guest_handle, insn_count, decl_dwords; } D3D8CapVsProgram;

/* An FVF code (bit 0 clear) or a program handle (bit 0 set), as the title
 * passed it to SetVertexShader -- the same discriminator hle_d3d8.c uses. */
typedef struct { uint32_t guest_handle; } D3D8CapVsSelect;

/* first_reg is already 0..191, the host's own range. The whole bank is
 * written once at frame start so a replay does not depend on constants set
 * in earlier frames; later chunks in the same frame are incremental updates. */
typedef struct { uint32_t first_reg, count; } D3D8CapVsConstants;

/* format is the Xbox D3DFORMAT code; the host D3D8 layer takes those directly
 * (hle_d3d8_texture.c). Levels are stored already in the Xbox's own packing,
 * swizzled or compressed as they were in guest memory, because that is what
 * the host upload path expects. linear marks the non-zero-Size case, whose
 * rows are padded to 64 bytes in the guest. */
typedef struct {
    uint32_t id;                       /* referred to by D3D8CapSetTexture */
    uint32_t format, width, height, levels, linear;
} D3D8CapTexture;

/* One per level, in level order, then all the texel bytes back to back in the
 * same order. pitch/rows are the guest's, so replay needs no format maths and
 * a change to d3d8_row_pitch cannot silently re-interpret an old capture. */
typedef struct { uint32_t pitch, rows, bytes; } D3D8CapTextureLevel;

/* texture_id 0 means "nothing bound at this stage". */
typedef struct { uint32_t stage, texture_id; } D3D8CapSetTexture;

/* prim is the Xbox D3DPRIMITIVETYPE; the conversion to a PC type and a
 * primitive count is hle_d3d8.c's and is applied at replay. vertex_bytes is
 * what the draw actually reads -- vertex_count * stride for a UP draw, and
 * (highest index + 1) * stride for an indexed one. */
typedef struct { uint32_t prim, vertex_count, stride, vertex_bytes; } D3D8CapDrawUp;

/* Indices are always 16-bit on the Xbox, so index_bytes is index_count * 2. */
typedef struct {
    uint32_t prim, index_count, stride, vertex_bytes, index_bytes;
} D3D8CapDrawIndexedUp;

/* ------------------------------------------------------------------ writer */

typedef struct D3D8CapWriter D3D8CapWriter;

/* Creates the file and reserves its header. Returns NULL if it cannot be
 * opened, so a mistyped RECOMP_D3D8_CAPTURE path disables capture rather
 * than stopping the run. */
D3D8CapWriter *d3d8cap_create(const char *path, uint32_t frame,
                              uint32_t width, uint32_t height);

/* One chunk, from up to three pieces, so a header and its payload go out
 * without being concatenated into a temporary buffer first. Any piece may be
 * NULL/0. Returns 0 on success, -1 once the stream has failed; a writer that
 * has failed stays failed and every later call is a no-op. */
int d3d8cap_chunk(D3D8CapWriter *w, uint32_t type,
                  const void *p0, size_t n0,
                  const void *p1, size_t n1,
                  const void *p2, size_t n2);

/* Writes the terminator, patches chunk_count into the header and closes.
 * Returns 0 if the whole capture was written, -1 otherwise; the writer is
 * freed either way. */
int d3d8cap_close(D3D8CapWriter *w);

/* How many chunks have been written so far (for the run log). */
uint32_t d3d8cap_chunk_count(const D3D8CapWriter *w);

/* ------------------------------------------------------------------ reader */

typedef struct {
    uint32_t    type;
    uint32_t    bytes;
    const void *data;    /* into the reader's buffer; valid until close */
} D3D8CapChunk;

typedef struct D3D8CapReader D3D8CapReader;

/* Reads the whole capture into memory -- a frame is tens of megabytes at
 * most, and replay walks it repeatedly. On failure returns NULL and writes a
 * reason into err (never truncated to nothing; err may be NULL). */
D3D8CapReader *d3d8cap_open(const char *path, char *err, size_t err_bytes);

const D3D8CapHeader *d3d8cap_header(const D3D8CapReader *r);

/* Next chunk, or 0 at the end. Every chunk is bounds-checked against the file
 * -- a capture from a run that crashed mid-frame is truncated, which must
 * stop the walk rather than read past the buffer. */
int d3d8cap_next(D3D8CapReader *r, D3D8CapChunk *out);

/* Back to the first chunk, for replaying the same capture again. */
void d3d8cap_rewind(D3D8CapReader *r);

void d3d8cap_close_read(D3D8CapReader *r);

/* Payload accessor: the bytes that follow a fixed-size payload header inside
 * a chunk, bounds-checked. Returns NULL if the chunk is too short to hold
 * `head` bytes plus `want` bytes, which is how replay rejects a malformed
 * chunk without trusting its own counts. */
const void *d3d8cap_tail(const D3D8CapChunk *c, size_t head, size_t want);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_CAPTURE_H */
