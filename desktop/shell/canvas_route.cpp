// ---------------------------------------------------------------------------
// canvas_route.cpp — `nc::`, the one canvas the shell talks to.
//
// The shell's whole knowledge of presentation is this file.  It picks the
// backend once, at boot:
//
//   1. DirectComposition (ncd) — the picture is a composition visual and the
//      WebView2 page is composited above it, so "transparent page over a native
//      canvas" is literally true.
//   2. GDI DIB child window (ncg) — only used when the composition device
//      cannot be created.  A child window can never show through a page HWND,
//      so this path keeps the page's own canvas visible and is a degradation,
//      not an alternative design.
//
// MB_CANVAS=gdi forces the fallback; MB_CANVAS=none disables the canvas
// entirely (the page's canvas then carries the picture, as it did before).
// ---------------------------------------------------------------------------
#include <windows.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "canvas_backend.h"
#include "native_canvas.h"

namespace {

enum class Backend { None, Dcomp, Dib };
Backend g_back = Backend::None;

bool EqualsNoCase(const char* a, const char* b) {
  if (!a || !b) return false;
  while (*a && *b) {
    char x = *a++, y = *b++;
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return *a == 0 && *b == 0;
}

}  // namespace

namespace nc {

bool Init(HWND host) {
  const char* mode = std::getenv("MB_CANVAS");
  if (mode && EqualsNoCase(mode, "none")) return false;
  const bool allowDcomp = !(mode && (EqualsNoCase(mode, "gdi") || EqualsNoCase(mode, "dib")));
  if (allowDcomp && ncd::Init(host)) {
    g_back = Backend::Dcomp;
    return true;
  }
  if (ncg::Init(host)) {
    g_back = Backend::Dib;
    return true;
  }
  g_back = Backend::None;
  return false;
}

void Shutdown() {
  if (g_back == Backend::Dcomp) ncd::Shutdown();
  else if (g_back == Backend::Dib) ncg::Shutdown();
  g_back = Backend::None;
}

bool Ready() {
  if (g_back == Backend::Dcomp) return ncd::Ready();
  if (g_back == Backend::Dib) return ncg::Ready();
  return false;
}

void SetBounds(int x, int y, int w, int h) {
  if (g_back == Backend::Dcomp) ncd::SetBounds(x, y, w, h);
  else if (g_back == Backend::Dib) ncg::SetBounds(x, y, w, h);
}

void BeginPass(int passW, int passH, int ss, int cw, int ch) {
  if (g_back == Backend::Dcomp) ncd::BeginPass(passW, passH, ss, cw, ch);
  else if (g_back == Backend::Dib) ncg::BeginPass(passW, passH, ss, cw, ch);
}

void UploadTile(const uint8_t* frame, int frameW, int frameH,
                int x, int y, int w, int h) {
  if (g_back == Backend::Dcomp) ncd::UploadTile(frame, frameW, frameH, x, y, w, h);
  else if (g_back == Backend::Dib) ncg::UploadTile(frame, frameW, frameH, x, y, w, h);
}

bool CommitPass() {
  if (g_back == Backend::Dcomp) return ncd::CommitPass();
  if (g_back == Backend::Dib) return ncg::CommitPass();
  return false;
}

bool CommitPassWithPreview(double sx, double ox, double sy, double oy) {
  if (g_back == Backend::Dcomp) return ncd::CommitPassWithPreview(sx, ox, sy, oy);
  if (g_back == Backend::Dib) return ncg::CommitPassWithPreview(sx, ox, sy, oy);
  return false;
}

void Preview(double scale, double dx, double dy, double cx, double cy) {
  if (g_back == Backend::Dcomp) ncd::Preview(scale, dx, dy, cx, cy);
  else if (g_back == Backend::Dib) ncg::Preview(scale, dx, dy, cx, cy);
}

void SetPreview(double sx, double ox, double sy, double oy) {
  if (g_back == Backend::Dcomp) ncd::SetPreview(sx, ox, sy, oy);
  else if (g_back == Backend::Dib) ncg::SetPreview(sx, ox, sy, oy);
}

void ResetPreview() {
  if (g_back == Backend::Dcomp) ncd::ResetPreview();
  else if (g_back == Backend::Dib) ncg::ResetPreview();
}

void Present() {
  if (g_back == Backend::Dcomp) ncd::Present();
  else if (g_back == Backend::Dib) ncg::Present();
}

HWND Hwnd() {
  if (g_back == Backend::Dcomp) return nullptr;   // it is a visual, not a window
  if (g_back == Backend::Dib) return ncg::Hwnd();
  return nullptr;
}

double LastUploadMs() {
  if (g_back == Backend::Dcomp) return ncd::LastUploadMs();
  if (g_back == Backend::Dib) return ncg::LastUploadMs();
  return 0.0;
}

double LastPresentMs() {
  if (g_back == Backend::Dcomp) return ncd::LastPresentMs();
  if (g_back == Backend::Dib) return ncg::LastPresentMs();
  return 0.0;
}

int FrameCount() {
  if (g_back == Backend::Dcomp) return ncd::FrameCount();
  if (g_back == Backend::Dib) return ncg::FrameCount();
  return 0;
}

const char* BackendName() {
  if (g_back == Backend::Dcomp) return "dcomp";
  if (g_back == Backend::Dib) return "dib";
  return "none";
}

bool Composited() { return g_back == Backend::Dcomp; }

IUnknown* OverlayVisual() {
  if (g_back == Backend::Dcomp) return ncd::OverlayVisual();
  return nullptr;
}

void CommitComposition() {
  if (g_back == Backend::Dcomp) ncd::CommitComposition();
}

/** Backend-specific reason the canvas did not come up (for shell.log). */
const char* CanvasWhy() {
  if (g_back == Backend::Dcomp) return ncd::Why();
  return "";
}

}  // namespace nc
