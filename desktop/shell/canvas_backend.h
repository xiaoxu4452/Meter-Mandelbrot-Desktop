// ---------------------------------------------------------------------------
// canvas_backend.h — the two implementations behind the `nc::` router.
//
// Both backends export exactly the same functions, so the router in
// canvas_route.cpp can pick one at boot and never look at it again.  The
// declarations live here once; a stray signature change then fails to compile
// instead of silently linking to a stale overload.
//
//   ncd::  DirectComposition visual + composition swapchain   (product path)
//   ncg::  GDI DIB child window                               (fallback)
// ---------------------------------------------------------------------------
#pragma once
#include <windows.h>
#include <cstdint>

#define MB_CANVAS_BACKEND_DECLS(ns)                          \
  namespace ns {                                             \
  bool Init(HWND host);                                      \
  void Shutdown();                                           \
  bool Ready();                                              \
  void SetBounds(int x, int y, int w, int h);                 \
  void BeginPass(int passW, int passH, int ss, int cw, int ch); \
  void UploadTile(const uint8_t* frame, int frameW, int frameH, int x, int y, \
                  int w, int h);                             \
  bool CommitPass();                                         \
  bool CommitPassWithPreview(double sx, double ox, double sy, double oy); \
  void Preview(double scale, double dx, double dy, double cx, double cy); \
  void SetPreview(double sx, double ox, double sy, double oy); \
  void ResetPreview();                                       \
  void Present();                                            \
  HWND Hwnd();                                               \
  double LastUploadMs();                                     \
  double LastPresentMs();                                    \
  int FrameCount();                                          \
  }

MB_CANVAS_BACKEND_DECLS(ncd)
MB_CANVAS_BACKEND_DECLS(ncg)

namespace ncd {
/** Why Init() refused, for the shell log. */
const char* Why();
/** The visual handed to put_RootVisualTarget (created above the canvas). */
IUnknown* OverlayVisual();
void CommitComposition();
}  // namespace ncd
