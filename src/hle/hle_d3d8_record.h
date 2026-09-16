/*
 * hle_d3d8_record.h -- the host boundary of shadow mode, and frame capture
 * recorded at it.
 *
 * Every call src/hle makes into the host renderer (src/d3d) goes through one
 * of the wrappers below. Each one forwards the call unchanged and, while a
 * frame is being captured, also writes it into the capture
 * (d3d8_capture.h). Because the wrappers sit after all of shadow mode's Xbox
 * conversion, a capture holds exactly what the host was given, and replay
 * (src/replay) needs no Xbox knowledge to reproduce it.
 *
 * The rule that keeps this true: nothing in src/hle calls a device vtable
 * entry or a d3d8_vsh_* / d3d8_combiners_* setter directly. A direct call is
 * a call the capture cannot see, and a replay that silently differs from the
 * run. The one exception is reading the back buffer for frame dumps
 * (hle_d3d8.c, shadow_dump_frame), which changes no state.
 *
 * Cost when not capturing: one test of a static pointer per call.
 *
 *   RECOMP_D3D8_CAPTURE=<path>     where to write; capture is off without it
 *   RECOMP_D3D8_CAPTURE_SWAP=<n>   which swap to record (default 120)
 *
 * Threads: guest threads are host threads, and the wrappers take no lock.
 * That is no worse than the host renderer itself, which takes none either;
 * shadow mode's calls arrive from whichever guest thread draws. A title that
 * draws from two threads at once would interleave its chunks in the capture
 * in the order the calls happened to be made.
 *
 * Windows only, like the rest of shadow mode.
 */
#ifndef XBOXRECOMP_HLE_D3D8_RECORD_H
#define XBOXRECOMP_HLE_D3D8_RECORD_H

#include <stdint.h>

#ifdef _WIN32

#include "d3d8_xbox.h"
#include "d3d8_vsh.h"

/* Called once per Swap, before the host Swap, with the swap counter after it
 * was incremented and the host back buffer's size. This is the capture's only
 * frame boundary: recording starts when the counter reaches the requested
 * swap (with a snapshot of the host's state) and the file is closed at the
 * next one. */
void hle_d3d8_capture_swap(unsigned long swaps, uint32_t width, uint32_t height);

/* True while a frame is being recorded. */
int hle_d3d8_capture_active(void);

/* ----------------------------------------------------------- device calls */

HRESULT host_Clear(IDirect3DDevice8 *dev, DWORD count, const D3DRECT *rects,
                   DWORD flags, D3DCOLOR color, float z, DWORD stencil);
/* Not recorded: Swap is the frame boundary, and replay presents on its own. */
HRESULT host_Swap(IDirect3DDevice8 *dev, DWORD flags);
HRESULT host_SetRenderState(IDirect3DDevice8 *dev, D3DRENDERSTATETYPE state,
                            DWORD value);
HRESULT host_SetTextureStageState(IDirect3DDevice8 *dev, DWORD stage,
                                  D3DTEXTURESTAGESTATETYPE type, DWORD value);
HRESULT host_SetTransform(IDirect3DDevice8 *dev, D3DTRANSFORMSTATETYPE state,
                          const D3DMATRIX *matrix);
HRESULT host_SetViewport(IDirect3DDevice8 *dev, const D3DVIEWPORT8 *viewport);
HRESULT host_SetTexture(IDirect3DDevice8 *dev, DWORD stage,
                        IDirect3DBaseTexture8 *texture);
HRESULT host_SetVertexShader(IDirect3DDevice8 *dev, DWORD handle);
HRESULT host_DrawPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                             UINT prims, const void *vertices, UINT stride);
HRESULT host_DrawIndexedPrimitiveUP(IDirect3DDevice8 *dev, D3DPRIMITIVETYPE type,
                                    UINT min_index, UINT num_vertices, UINT prims,
                                    const void *indices, D3DFORMAT index_format,
                                    const void *vertices, UINT stride);

/* Textures. Creation and locking are not recorded as such: a texture is
 * written whole, from the host's own copy, the first time the capture needs
 * it (bound, or bound at the snapshot). An unlock of a texture the capture
 * already holds re-records that level, and a release retires its id. */
HRESULT host_CreateTexture(IDirect3DDevice8 *dev, UINT width, UINT height,
                           UINT levels, DWORD usage, D3DFORMAT format,
                           D3DPOOL pool, IDirect3DTexture8 **texture);
HRESULT host_LockRect(IDirect3DTexture8 *texture, UINT level,
                      D3DLOCKED_RECT *locked, const RECT *rect, DWORD flags);
HRESULT host_UnlockRect(IDirect3DTexture8 *texture, UINT level);
ULONG   host_ReleaseTexture(IDirect3DTexture8 *texture);

/* -------------------------------------------- vertex programs, combiners */

HRESULT host_vsh_create_shader(const DWORD *microcode, int insn_count, DWORD *handle);
HRESULT host_vsh_delete_shader(DWORD handle);
void    host_vsh_set_constant(int first_reg, const float *data, int count);
HRESULT host_vsh_set_declaration(DWORD handle, const D3D8VshInput *inputs, int count);
void    host_vsh_set_screenspace(const float scale[4], const float offset[4]);
void    host_combiners_set_pixel_shader(DWORD token);

#endif /* _WIN32 */

#endif /* XBOXRECOMP_HLE_D3D8_RECORD_H */
