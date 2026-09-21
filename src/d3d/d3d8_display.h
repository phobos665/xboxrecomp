/*
 * d3d8_display.h -- the size the host renders at, and the pass that gets
 * that image onto the screen.
 *
 * The guest presents at its own size (640x480 for most titles) and every
 * draw path already ends in normalised coordinates: vertex programs have
 * their screen-space transform undone, the fixed-function path is
 * transformed by the host, and pre-transformed vertices divide by the
 * guest's presentation size. So a larger host target re-rasterises the
 * same scene rather than magnifying it, and nothing needs resampling.
 *
 * That is the whole mechanism behind both supersampling and, later,
 * widescreen: render the scene at a size of the host's choosing, then
 * resolve it down onto a swap chain that still matches the window.
 */
#ifndef XBOXRECOMP_D3D8_DISPLAY_H
#define XBOXRECOMP_D3D8_DISPLAY_H

#include "d3d8_internal.h"

#if defined(_WIN32)

/* Read once, from the environment, before the first device is created.
 *
 * RECOMP_RES_SCALE=<1..8> is the supersample factor. 1 renders at the
 * guest's own size and must be indistinguishable from having none of this
 * code at all -- the scene target is skipped entirely in that case.
 */
typedef struct D3D8DisplayPolicy {
    UINT scale;
} D3D8DisplayPolicy;

const D3D8DisplayPolicy *d3d8_display_policy(void);

/* The size the scene is rendered at, for a guest presenting at guest_w by
 * guest_h. Equal to the guest's own size when nothing is scaled. */
void d3d8_display_scene_size(UINT guest_w, UINT guest_h,
                             UINT *scene_w, UINT *scene_h);

/* Where the scene lands inside a back buffer of bb_w by bb_h: the largest
 * centred rectangle with the scene's own shape. Equal to the whole buffer
 * when the two already agree; otherwise the difference is the bars. */
typedef struct D3D8DisplayFit { UINT x, y, w, h; } D3D8DisplayFit;

D3D8DisplayFit d3d8_display_fit(UINT scene_w, UINT scene_h, UINT bb_w, UINT bb_h);

/* Draw `scene` into `fit` inside `out`, box filtered, painting everything
 * outside `fit` black so a window of a different shape gets bars rather
 * than a stretched picture. Binds `out` itself and puts back the render
 * target and viewport it found, which is why the caller must not bind the
 * output first: the state this restores is the scene the next frame draws
 * into. */
HRESULT d3d8_display_resolve(ID3D11ShaderResourceView *scene,
                             UINT scene_w, UINT scene_h,
                             ID3D11RenderTargetView *out,
                             D3D8DisplayFit fit);

void d3d8_display_shutdown(void);

#endif /* _WIN32 */

#endif /* XBOXRECOMP_D3D8_DISPLAY_H */
