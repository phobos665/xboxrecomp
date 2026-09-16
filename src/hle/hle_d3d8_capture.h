/*
 * hle_d3d8_capture.h -- the guest side of frame capture: what shadow mode
 * hands to the container in d3d8_capture.h.
 *
 * Every entry point here is a no-op unless RECOMP_D3D8_CAPTURE names a path
 * and the run has reached the chosen swap, so the call sites in hle_d3d8.c,
 * hle_d3d8_state.c and hle_d3d8_texture.c are unconditional one-liners that
 * cost a predicate on a global when capture is off.
 *
 * Windows only, like the rest of shadow mode. The container itself
 * (d3d8_capture.c) is portable and builds everywhere.
 */
#ifndef XBOXRECOMP_HLE_D3D8_CAPTURE_H
#define XBOXRECOMP_HLE_D3D8_CAPTURE_H

#include <stdint.h>

#ifdef _WIN32

/* True while the chosen frame is being recorded. Call sites that would have
 * to do real work to produce their arguments -- reading guest memory, walking
 * a mip chain -- test this first. */
int hle_d3d8_capture_active(void);

/* Called once per Swap, with the swap counter after it was incremented and
 * the shadow back buffer's size. This is the only frame boundary the capture
 * has: it starts recording when the counter reaches the requested swap and
 * closes the file at the next one, so a capture holds the operations between
 * one Swap and the next, as the replay tool expects. */
void hle_d3d8_capture_swap(unsigned long swaps, uint32_t width, uint32_t height);

void hle_d3d8_capture_clear(uint32_t flags, uint32_t color,
                            uint32_t z_bits, uint32_t stencil);
void hle_d3d8_capture_transform(uint32_t xbox_state, const float *matrix16);
void hle_d3d8_capture_viewport(uint32_t x, uint32_t y, uint32_t width,
                               uint32_t height, float min_z, float max_z);

/* Render and texture stage states, recorded as the host values
 * hle_d3d8_shadow_apply_states actually set. They accumulate into one batch
 * per draw and are flushed by hle_d3d8_capture_states_flush(), which that
 * function calls once it has applied everything -- so the state chunks land
 * in the capture immediately before the draw they belong to. */
void hle_d3d8_capture_render_state(uint32_t host_state, uint32_t value);
void hle_d3d8_capture_stage_state(uint32_t stage, uint32_t host_state, uint32_t value);
void hle_d3d8_capture_states_flush(void);

/* A vertex shader program the title created: the NV2A microcode replay
 * rebuilds the host program from, and the declaration token stream that came
 * with it. declaration may be NULL. */
void hle_d3d8_capture_vs_program(uint32_t guest_handle,
                                 const uint32_t *microcode, uint32_t insn_count,
                                 const uint32_t *declaration, uint32_t decl_dwords);

/* The handle passed to SetVertexShader: an FVF code, or a program handle with
 * bit 0 set. */
void hle_d3d8_capture_vs_select(uint32_t guest_handle);

/* Vertex shader constants, in the host's own 0..191 register numbering. The
 * whole bank is written at frame start; these are the updates within it.
 *
 * Nothing calls this at this commit: the XDK sets constants through fastcall
 * register-argument functions that hle_d3d8.c cannot replace yet (see its
 * header comment). The frame-start snapshot below is therefore the only
 * source of constants in a capture today. When those setters are replaced --
 * the work in hle_d3d8_vertex.c on the vertex-forwarding branch, whose
 * forward_constants() already has the values in hand -- one call here makes
 * mid-frame constant changes land in captures too. */
void hle_d3d8_capture_vs_constants(uint32_t first_reg, const float *data,
                                   uint32_t count);

/* A texture being bound, with everything replay needs to rebuild it. Levels
 * are the guest's own bytes, still swizzled or compressed, because that is
 * what the host upload path takes (hle_d3d8_texture.c).
 *
 * guest_va identifies the texture for de-duplication inside one frame: a
 * texture bound by forty draws is stored once and referred to by id.
 * texels points at level 0 in host address space; pass NULL (or stage alone)
 * through hle_d3d8_capture_unbind_texture to record "nothing bound here". */
typedef struct {
    uint32_t    guest_va;
    uint32_t    format;          /* Xbox D3DFORMAT code */
    uint32_t    width, height, levels;
    int         linear;          /* rows padded to guest_pitch, one level */
    uint32_t    guest_pitch;     /* linear textures only */
    const void *texels;
} HleD3D8CaptureTexture;

void hle_d3d8_capture_bind_texture(uint32_t stage, const HleD3D8CaptureTexture *t);
void hle_d3d8_capture_unbind_texture(uint32_t stage);

/* The two draws shadow mode forwards. `vertices` is a host pointer to the
 * first vertex, and vertex_bytes is what the draw actually reads: for the
 * indexed draw that is (highest index + 1) * stride, which hle_d3d8.c has
 * already worked out to give the host its vertex range. */
void hle_d3d8_capture_draw_up(uint32_t prim, uint32_t vertex_count,
                              const void *vertices, uint32_t stride);
void hle_d3d8_capture_draw_indexed_up(uint32_t prim, uint32_t index_count,
                                      const uint16_t *indices,
                                      const void *vertices, uint32_t stride,
                                      uint32_t vertex_bytes);

/* ------------------------------------------- implemented by their own files
 *
 * A capture has to be self-contained, but most of the state a frame draws
 * with was set in earlier frames. So at frame start the capture asks each
 * part of shadow mode to re-emit what it is holding. */

/* hle_d3d8_state.c: drop the "unchanged since last draw" memo, so the next
 * draw re-applies every state and the capture sees the whole set rather than
 * the handful that happened to change inside the captured frame. */
void hle_d3d8_shadow_states_invalidate(void);

/* hle_d3d8.c: re-emit the vertex shader currently selected. */
void hle_d3d8_capture_snapshot_shader(void);

/* hle_d3d8_texture.c: re-emit the texture bound to each stage. */
void hle_d3d8_capture_snapshot_textures(void);

#endif /* _WIN32 */

#endif /* XBOXRECOMP_HLE_D3D8_CAPTURE_H */
