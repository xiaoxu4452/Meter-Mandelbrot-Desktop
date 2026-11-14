// ---------------------------------------------------------------------------
// native_canvas.h — the product canvas: a DirectComposition visual holding a
// flip-model swapchain, with the WebView2 page composited above it.
//
// The renderer (mbc::Engine / the core kernels) still writes plain RGBA into a
// CPU surface; this module owns the presentation side: a D3D11 device, a
// composition swapchain, a DEFAULT-usage texture fed by UpdateSubresource, a
// fullscreen-triangle shader that scales / supersample-reduces / preview-
// transforms the last pass, and the Present call.
//
// Two backends implement the same API:
//   — DirectComposition (dcomp_canvas.cpp): the canvas is an IDCompositionVisual
//     under a visual tree owned by the window; WebView2 hands its own visual to
//     the same tree, so the page is genuinely transparent over the picture.
//   — GDI DIB (dib_canvas.cpp): a child window, kept only for machines where
//     DirectComposition is unavailable; the page then cannot be transparent and
//     the JS canvas stays the visible one.
//
// Everything is optional: Init() returning false leaves the product on the
// original JS-canvas path, no blank window.
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <cstdint>

namespace nc {

/** Create the canvas child window and the D3D11 objects.  False = no path. */
bool Init(HWND host);
void Shutdown();
bool Ready();

/** Move the child window and resize the swapchain without re-rendering. */
void SetBounds(int x, int y, int w, int h);

/** Declare a new pass surface.  passW/passH are the engine bitmap dimensions,
 *  ss the supersampling factor, cw/ch the page canvas dimensions. */
void BeginPass(int passW, int passH, int ss, int cw, int ch);

/** Copy one finished tile into the staging texture (no present yet). */
void UploadTile(const uint8_t* frame, int frameW, int frameH,
                int x, int y, int w, int h);

/** Promote the staging texture to the visible one, reset the preview
 *  transform and present.  Returns false when there was no staging surface. */
bool CommitPass();

/** Promote the staging texture while KEEPING the gesture transform that
 *  accumulated while this pass was being computed (src = sx*dst + ox, in
 *  normalised units).  Without this, a live pass committed mid-gesture snaps
 *  the picture back to the view that existed when the pass started, which is
 *  the "zoom jumps / goes blurry again" symptom.  The page sends its own
 *  accumulated transform with the flush message. */
bool CommitPassWithPreview(double sx, double ox, double sy, double oy);

/** One gesture-preview step, mirroring the page's blitSnapshot(dx,dy,scale,
 *  cx,cy).  Coordinates are normalised by the canvas size. */
void Preview(double scale, double dx, double dy, double cx, double cy);

/** Replace the preview transform (src = sx*dst + ox) and present.  A pan knows
 *  its total offset and must not have it composed again per event. */
void SetPreview(double sx, double ox, double sy, double oy);

/** Drop the preview transform (new frame took over). */
void ResetPreview();

/** Present the current texture scaled to the window. */
void Present();

HWND Hwnd();
double LastUploadMs();
double LastPresentMs();
int FrameCount();

/* ---- composition hosting -------------------------------------------------
 *
 * The product canvas is a DirectComposition visual, not a child window, and
 * the WebView2 is a second visual stacked on top of it (visual hosting).  That
 * is the only arrangement in which a transparent page really shows the native
 * picture: a page rendered into its own child HWND can never reveal a sibling
 * window underneath, no matter what the page's background is.
 *
 * "dcomp" = composition visual, "dib" = the GDI child-window fallback,
 * "none"  = no canvas at all.
 */
const char* BackendName();

/** True when the picture lives in a composition visual (no child HWND). */
bool Composited();

/** The visual the WebView2 composition controller renders into, created above
 *  the canvas and already added to the tree.  nullptr when the backend has no
 *  DirectComposition tree.  The caller passes it to put_RootVisualTarget. */
IUnknown* OverlayVisual();

/** Push pending visual-tree changes (bounds, ordering, a new root visual). */
void CommitComposition();

/** Why the DirectComposition backend refused to start, for shell.log. */
const char* CanvasWhy();

}  // namespace nc
