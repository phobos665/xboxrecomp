/**
 * NV2A Vertex Shader Microcode to HLSL Translator
 *
 * The Xbox NV2A GPU has a programmable vertex shader unit compatible with
 * (and extending) the original GeForce3/4 vertex shader architecture.
 * Games upload sequences of 128-bit microcode instructions via
 * D3DDevice_CreateVertexShader(). At draw time, the NV2A executes
 * these instructions in its vertex shader pipeline.
 *
 * This module translates NV2A vertex shader microcode into HLSL source
 * code, compiles it with D3DCompile, and caches the resulting
 * ID3D11VertexShader for use by the D3D8->D3D11 compatibility layer.
 *
 * Decoding the microcode and running it on the CPU is not here. That half
 * needs no graphics API, so it lives in src/kernel/nv2a_vsh.{c,h} and builds
 * on every platform; the instruction set, register model and the types
 * NV2AVshProgram and NV2AVshState are documented there. This header adds only
 * what is specific to the Direct3D 11 path.
 *
 * References:
 *   - envytools NV20 vertex shader documentation
 *   - xemu NV2A vertex shader implementation
 *   - Xbox SDK D3D vertex shader programming guide
 *   - US Patent 7,002,588 (Microsoft/Nvidia vertex shader architecture)
 *   - docs/technical/nv2a-vertex-program-encoding.md
 */

#ifndef XBOXRECOMP_D3D8_VSH_H
#define XBOXRECOMP_D3D8_VSH_H

#include <d3d11.h>
#include <stdint.h>
#include <windows.h>

#include "../kernel/nv2a_vsh.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Maximum number of shader programs that can be stored. */
#define NV2A_VS_MAX_SLOTS           128

/** Shader cache size (hashed microcode -> compiled shader). */
#define NV2A_VS_CACHE_SIZE          64

/* ================================================================
 * Shader Slot (stored microcode)
 * ================================================================ */

/**
 * A stored vertex shader program slot.
 *
 * Created by CreateVertexShader(), indexed by handle.
 * The microcode is stored as raw DWORDs; parsing and compilation
 * are deferred until the shader is first used in a draw call.
 */
typedef struct NV2AVshSlot {
    DWORD   microcode[NV2A_VS_MAX_INSTRUCTIONS * 4]; /* Raw 128-bit instructions */
    int     length;         /* Number of instructions */
    int     in_use;         /* 1 if this slot is allocated */
} NV2AVshSlot;

/* ================================================================
 * VS Constant Buffer Layout (HLSL)
 *
 * Uploaded to register(b1) so it doesn't conflict with the
 * fixed-function transform CB at b0.
 *
 * Must be 16-byte aligned and match the HLSL cbuffer declaration.
 * ================================================================ */

typedef struct NV2AVSConstants {
    float c[NV2A_VS_MAX_CONSTANTS][4];  /* 192 float4 constants */
} NV2AVSConstants;

/* ================================================================
 * Public API
 * ================================================================ */

/**
 * Initialize the vertex shader translator.
 * Allocates the constant buffer and shader cache.
 * Must be called after D3D11 device creation.
 */
HRESULT d3d8_vsh_init(void);

/**
 * Shut down the vertex shader translator.
 * Releases all cached shaders, input layouts, and buffers.
 */
void d3d8_vsh_shutdown(void);

/**
 * Store a vertex shader program (CreateVertexShader).
 *
 * Copies the microcode into an internal slot. The shader is not
 * compiled until first use.
 *
 * @param microcode   Pointer to the 128-bit instruction array (4 DWORDs each)
 * @param num_insns   Number of instructions
 * @param out_handle  Receives the shader handle (>= 0x10000 to distinguish from FVF)
 * @return S_OK on success
 */
HRESULT d3d8_vsh_create_shader(const DWORD *microcode, int num_insns,
                                DWORD *out_handle);

/**
 * Delete a previously created vertex shader.
 *
 * @param handle  The shader handle from d3d8_vsh_create_shader
 * @return S_OK on success
 */
HRESULT d3d8_vsh_delete_shader(DWORD handle);

/**
 * Set a vertex shader constant register.
 *
 * @param start_reg  First register index (0-191)
 * @param data       Pointer to float4 data (4 floats per register)
 * @param count      Number of float4 registers to set
 */
void d3d8_vsh_set_constant(int start_reg, const float *data, int count);

/**
 * The whole constant bank, as NV2A_VS_MAX_CONSTANTS float4 registers laid out
 * back to back (NV2AVSConstants).
 *
 * Constants arrive a few registers at a time and are never read back by the
 * renderer, so nothing needed this until frame capture: a capture has to be
 * self-contained, and the frame it records draws with constants set before it
 * began (src/hle/hle_d3d8_capture.c). Read-only; the pointer stays valid for
 * the life of the process.
 */
const float *d3d8_vsh_constants(void);

/**
 * Check if a shader handle refers to a programmable vertex shader
 * (as opposed to an FVF code).
 *
 * On Xbox, handles > 0xFFFF are shader handles.
 */
BOOL d3d8_vsh_is_programmable(DWORD handle);

/**
 * Prepare for a draw call using a programmable vertex shader.
 *
 * - Parses microcode if not yet parsed
 * - Generates HLSL and compiles if not cached
 * - Updates the constant buffer
 * - Binds the vertex shader, input layout, and constant buffer
 *
 * @param handle  The active vertex shader handle
 * @return TRUE if a programmable VS was bound, FALSE on fallback
 */
BOOL d3d8_vsh_prepare_draw(DWORD handle);

/**
 * Generate HLSL vertex shader source from parsed program.
 *
 * @param program   Parsed program (from nv2a_vsh_parse)
 * @param buf       Output buffer for HLSL source
 * @param bufsize   Size of output buffer
 * @return Number of characters written, or -1 on error
 */
int d3d8_vsh_generate_hlsl(const NV2AVshProgram *program,
                            char *buf, int bufsize);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VSH_H */
