// ------------------------------------------------------------------
// host.cpp — Win32 + WebView2 desktop shell for MANDELBROT.
//
// Structure mirrors the mature trainer shell (host.cpp / bridge.js):
//   · one .exe, UI resources compiled in (ui_resources.cpp)
//   · virtual origin https://mb.test/* served by an in-process interceptor
//   · native C++ (mbc::Engine) does every pixel; the page never runs WASM
//   · finished tiles reach the page through WebView2 shared buffers, so the
//     kernel writes once and the renderer reads the same pages (no base64,
//     no per-frame JSON pixel payload)
//   · minimise pauses the engine, DPI follows the monitor, geometry is
//     persisted atomically on exit
//
// Message protocol (page -> host):
//   {type:"hello"}                             -> {type:"ready", threads}
//   {type:"render", seq, ...}                   one tile
//   {type:"ack", seq}                           shared-buffer slot released
//   {type:"cancel", viewId}                     drop stale work
//   {type:"pause", on}                          minimise handling
// ------------------------------------------------------------------
#include <windows.h>
#include <dwmapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <sstream>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "WebView2.h"
#include "mb_core.h"
#include "mb_gmp.h"
#include "mb_view.h"
#include "mb_bridge.h"
#include "native_canvas.h"
#include "mset_backend.h"
#include "gpu_render.h"
#include "cl_field.h"
#include "ui_resources.h"

#pragma comment(lib, "WebView2LoaderStatic.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "windowscodecs.lib")

// Native child-window canvas.  Two backends present the same picture:
//
//   gpur::   OpenGL 3.3 core + OpenCL field   (the architecture)
//   nc::     D3D11 bitblt swapchain           (fallback)
//
// The shell only ever calls the `canvas::` shims below, so a machine without a
// GL 3.3 context keeps the exact behaviour it had before.
static bool g_nativeCanvas = false;   // any native canvas is presenting
static bool g_glCanvas = false;       // it is the OpenGL one
static bool g_gpuField = false;       // OpenCL computes the iteration field
/* Size / view of the pass currently resident in the GL canvas's field texture.
 * A field request is a whole frame, so the texture is reallocated only when the
 * frame really changes. */
static int g_glFieldW = 0, g_glFieldH = 0, g_glFieldView = -1;
/* Iteration budget above which the OpenCL field beats the CPU (see the gate at
 * the render site). */
static constexpr int kGpuMinIter = 2000;
static int g_glFieldPosts = 0;
/* Gesture previews applied / dropped because a newer frame had landed.  A
 * preview that arrives late must not move the new picture. */
static int g_prevApplied = 0, g_prevStale = 0;

namespace canvas {

inline void BeginPass(int w, int h, int ss, int cw, int ch) {
  if (g_glCanvas) gpur::beginPass(w, h, ss, cw, ch);
  else if (g_nativeCanvas) nc::BeginPass(w, h, ss, cw, ch);
}

inline bool UploadField(const void* p, int w, int h) {
  return g_glCanvas ? gpur::uploadField(static_cast<const float*>(p), w, h)
                    : false;
}

inline bool Upload(const uint8_t* p, int w, int h, int x, int y, int tw, int th) {
  if (g_glCanvas) {
    // `p` is the base of the whole pass surface; the canvas wants the tile.
    return gpur::uploadRGBA(p + ((size_t)y * (size_t)w + (size_t)x) * 4, tw, th,
                            x, y);
  }
  if (g_nativeCanvas) { nc::UploadTile(p, w, h, x, y, tw, th); return true; }
  return false;
}

inline bool Commit(bool keep, double sx, double ox, double sy, double oy) {
  if (g_glCanvas) return gpur::commit(keep, sx, ox, sy, oy);
  if (g_nativeCanvas)
    return keep ? nc::CommitPassWithPreview(sx, ox, sy, oy) : nc::CommitPass();
  return false;
}

inline void SetBounds(int x, int y, int w, int h) {
  if (g_glCanvas) gpur::setBounds(x, y, w, h);
  else if (g_nativeCanvas) nc::SetBounds(x, y, w, h);
}

inline void Preview(double scale, double dx, double dy, double cx, double cy) {
  if (g_glCanvas) gpur::preview(scale, dx, dy, cx, cy);
  else if (g_nativeCanvas) nc::Preview(scale, dx, dy, cx, cy);
}

/** Replace the gesture transform instead of composing onto it (see glc::SetPreview). */
inline void SetPreview(double sx, double ox, double sy, double oy) {
  if (g_glCanvas) gpur::setPreview(sx, ox, sy, oy);
  else if (g_nativeCanvas) nc::SetPreview(sx, ox, sy, oy);
}

inline HWND Hwnd() { return g_glCanvas ? gpur::hwnd() : nc::Hwnd(); }
inline int Frames() { return g_glCanvas ? gpur::frames() : nc::FrameCount(); }
inline double UploadMs() {
  return g_glCanvas ? 0.0 : nc::LastUploadMs();
}
inline double PresentMs() {
  return g_glCanvas ? gpur::lastPresentMs() : nc::LastPresentMs();
}

/* The visual tree the WebView2 composition controller renders into, created
 * above the canvas visual.  nullptr when the canvas is a plain child window,
 * in which case the page must stay opaque and carry its own pixels. */
inline IUnknown* OverlayVisual() { return g_glCanvas ? nullptr : nc::OverlayVisual(); }
inline void CommitComposition() { if (!g_glCanvas) nc::CommitComposition(); }
inline bool Composited() { return !g_glCanvas && nc::Composited(); }
inline const char* Backend() { return g_glCanvas ? "gl" : nc::BackendName(); }

}  // namespace canvas

// ---------------------------------------------------------------- utilities

static std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return std::string();
  int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

static std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return std::wstring();
  int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

static const wchar_t* kAppName = L"MandelbrotView";

static std::string g_pendingShellMsg;

static std::wstring AppDir() {
  wchar_t b[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"LOCALAPPDATA", b, MAX_PATH) && b[0])
    return std::wstring(b) + L"\\" + kAppName;
  return L".";
}

/* Where the exe itself lives.  Distinct from AppDir(), which is the writable
 * per-user folder for logs and the WebView2 profile: the preset library is a
 * thing the user copies around with the exe, so it belongs next to it. */
static std::wstring ExeDir() {
  wchar_t b[MAX_PATH] = {0};
  const DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
  if (!n) return L".";
  std::wstring p(b, n);
  const size_t k = p.find_last_of(L"\\/");
  return k == std::wstring::npos ? std::wstring(L".") : p.substr(0, k);
}

static std::wstring LogPath() { return AppDir() + L"\\shell.log"; }

static void EnsureDir() { CreateDirectoryW(AppDir().c_str(), nullptr); }

static std::mutex g_logMu;

static void LogMsg(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_logMu);
  EnsureDir();
  FILE* f = _wfopen(LogPath().c_str(), L"ab");
  if (!f) return;
  char ts[32];
  SYSTEMTIME st;
  GetLocalTime(&st);
  snprintf(ts, sizeof(ts), "%02d:%02d:%02d.%03d ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
  fputs(ts, f);
  fputs(s.c_str(), f);
  fputc('\n', f);
  fclose(f);
}

// ---------------------------------------------------------------- handlers

template <class T>
class Cb : public T {
 public:
  ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (riid == __uuidof(T) || riid == IID_IUnknown) {
      *pp = static_cast<T*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
};

// ---------------------------------------------------------------- globals

static HWND g_hwnd = nullptr;
/* Deferred first show: the window is created (WebView2 needs its client size
 * and DPI) but stays hidden until the page has painted the branded splash.
 * See the `splashPainted` message and the 0x69 failsafe. */
static bool g_windowShown = false;
static bool g_startMaxed = false;

static void ShowAppWindow(const char* why) {
  if (g_windowShown || !g_hwnd) return;
  g_windowShown = true;
  ShowWindow(g_hwnd, g_startMaxed ? SW_SHOWMAXIMIZED : SW_SHOW);
  UpdateWindow(g_hwnd);
  LogMsg(std::string("[boot] window shown (") + why + ")");
}
static ICoreWebView2Environment* g_env = nullptr;
static ICoreWebView2Controller* g_ctl = nullptr;
static ICoreWebView2* g_wv = nullptr;
static ICoreWebView2_17* g_wv17 = nullptr;
static ICoreWebView2Environment12* g_env12 = nullptr;
/* Visual hosting (composition controller).  When this is non-null the page is
 * a DirectComposition visual stacked above the native canvas visual instead of
 * a child HWND: that is the only arrangement in which a transparent page shows
 * the C++ picture underneath.  g_compMode is decided at boot and never changes;
 * the windowed controller stays as the fallback for machines without
 * DirectComposition. */
static ICoreWebView2CompositionController* g_wvc = nullptr;
static bool g_compMode = false;
static bool g_dark = true;

// ---------------------------------------------------------------- shared buffers

struct Slot {
  ICoreWebView2SharedBuffer* buf = nullptr;
  BYTE* ptr = nullptr;
  uint64_t size = 0;
  bool busy = false;
  uint64_t seq = 0;
};

static const int kSlotCount = 6;
static const uint64_t kSlotBytes = 1u << 20;   // 1 MB covers a 512x512 RGBA tile
static Slot g_slots[kSlotCount];
static std::mutex g_slotMu;
static std::condition_variable g_slotCv;
static bool g_slotsReady = false;

static bool CreateSlots() {
  if (!g_env12) return false;
  for (int i = 0; i < kSlotCount; ++i) {
    ICoreWebView2SharedBuffer* sb = nullptr;
    if (FAILED(g_env12->CreateSharedBuffer(kSlotBytes, &sb)) || !sb) {
      LogMsg("[shm] CreateSharedBuffer failed at slot " + std::to_string(i));
      return false;
    }
    BYTE* p = nullptr;
    if (FAILED(sb->get_Buffer(&p)) || !p) {
      LogMsg("[shm] get_Buffer failed");
      sb->Release();
      return false;
    }
    g_slots[i].buf = sb;
    g_slots[i].ptr = p;
    g_slots[i].size = kSlotBytes;
    g_slots[i].busy = false;
    g_slots[i].seq = 0;
  }
  g_slotsReady = true;
  LogMsg("[shm] " + std::to_string(kSlotCount) + " shared buffers, " +
         std::to_string(kSlotBytes / 1024) + " KB each");
  return true;
}

// ---------------------------------------------------------------- frame buffer
//
// Single-buffer mode: instead of one shared buffer per tile, the page declares
// the pass size once, every tile is written straight into that one buffer at
// its offset, and the finished frame is published once.  This removes ~70
// buffer round-trips and 70 putImageData calls per pass.

static void PostJson(const std::string& json);

struct FrameBuf {
  ICoreWebView2SharedBuffer* buf = nullptr;
  std::shared_ptr<std::vector<uint8_t>> cpu;   // native-canvas pass surface
  BYTE* ptr = nullptr;
  int w = 0, h = 0, viewId = -1;
  int ss = 1, cw = 0, ch = 0;
  bool finalPass = false;
};

static FrameBuf g_frame;
static std::mutex g_frameMu;
static int g_framePosted = 0;

// Last completed final pass, kept for PNG export.  Swapped out of g_frame so
// the renderer never has to copy a 4K surface.
static std::vector<uint8_t> g_lastFinal;
static int g_lastW = 0, g_lastH = 0, g_lastSS = 1;
static std::mutex g_lastMu;

// ---------------------------------------------------------------- field buffer
//
// One whole-frame iteration field, handed to the page so it can be shaded on
// the GPU.  It is NOT a per-tile pool: a field pass is a single request for the
// whole view, because the shader needs the frame's own pixel grid for its
// screen-space terms (screencoord, dFdx/dFdy, per-sample offsets).
//
// The kernel now writes *directly* into the shared buffer (see mbc::Field::
// adopt).  The old path allocated a 47 MB vector, zero-filled it, let the kernel
// write it, then memcpy'd those 47 MB into the shared buffer — three times on a
// progressive frame.  Eliminating all three costs is the whole point of this
// change; no pixel value changes.
//
// A slot pool is required once the write is direct.  The page reads the buffer
// asynchronously (it uploads it to a GL texture), so the host must not start
// overwriting the same bytes while that read is in flight; and progressive
// passes plus preset thumbnails issue field jobs at several different sizes.
// Slots are reserved on the UI thread (CreateSharedBuffer must run there) and
// released by the page's `ack`, exactly like the RGBA tile slots.  A stalled
// ack cannot wedge the pool: a slot busy for more than 2 s is reclaimed.
//
// Peak memory does not grow: the removed staging vector was the same size as
// one extra slot.

static void HostLog(const char* fmt, ...);

struct FieldSlot {
  ICoreWebView2SharedBuffer* buf = nullptr;
  BYTE* ptr = nullptr;
  int w = 0, h = 0;
  /* 4 = colour plane only, 6 = colour plane followed by the value plane
   * (CHANNEL_VALUE: z at the escape point, two floats per texel).  Slots are
   * sized for the larger layout only when a scheme asks for it, so an ordinary
   * pass never pays the extra 50% of shared memory. */
  int channels = 4;
  uint64_t bytes = 0;
  bool busy = false;
  bool posted = false;           // the page owns it until it acks
  uint64_t seq = 0;
  std::chrono::steady_clock::time_point since;
};

static const int kFieldSlotCount = 3;
static FieldSlot g_fieldSlots[kFieldSlotCount];
static std::mutex g_fieldMu;
static int g_fieldCreated = 0;

/** Free a slot reserved for `seq` (ack, cancel, or a dropped job). */
static void ReleaseFieldSeq(uint64_t seq) {
  if (!seq) return;
  std::lock_guard<std::mutex> lk(g_fieldMu);
  for (int i = 0; i < kFieldSlotCount; ++i) {
    if (g_fieldSlots[i].busy && g_fieldSlots[i].seq == seq) {
      g_fieldSlots[i].busy = false;
      g_fieldSlots[i].posted = false;
      g_fieldSlots[i].seq = 0;
    }
  }
}

/**
 * Reserve a field slot of the requested size for one job.
 * Returns the slot index, or -1 when no slot can be reserved (the caller then
 * falls back to the RGBA path rather than blocking the UI thread).
 */
static int EnsureField(int w, int h, uint64_t seq, int channels) {
  if (!g_env12 || w <= 0 || h <= 0) {
    HostLog("[field] no environment (env12=%p w=%d h=%d)", (void*)g_env12, w, h);
    return -1;
  }
  if (channels != 6) channels = 4;
  std::lock_guard<std::mutex> lk(g_fieldMu);
  const uint64_t bytes =
      (uint64_t)w * (uint64_t)h * (uint64_t)channels * sizeof(float);
  const auto now = std::chrono::steady_clock::now();

  /* An ack that never arrives must not freeze the field path forever.  Only
   * *posted* slots are eligible: a reserved slot may legitimately sit in the
   * render queue behind a multi-second deep-zoom tile. */
  for (int i = 0; i < kFieldSlotCount; ++i) {
    FieldSlot& s = g_fieldSlots[i];
    if (s.busy && s.posted &&
        std::chrono::duration<double>(now - s.since).count() > 2.0) {
      HostLog("[field] slot %d stalled, reclaiming", i);
      s.busy = false;
      s.posted = false;
      s.seq = 0;
    }
  }

  /* Prefer a free slot whose buffer is already big enough.  A slot sized for
   * six channels serves a four-channel pass and vice versa: the page reads
   * `channels` from the post, and the value plane lives past the colour plane,
   * so a larger buffer is simply under-filled.  Reusing it avoids tearing down
   * a shared buffer every time a scheme toggles CHANNEL_VALUE. */
  int pick = -1;
  for (int i = 0; i < kFieldSlotCount; ++i) {
    const FieldSlot& s = g_fieldSlots[i];
    if (!s.busy && s.buf && s.w == w && s.h == h && s.bytes >= bytes) {
      pick = i;
      break;
    }
  }
  if (pick < 0) {
    for (int i = 0; i < kFieldSlotCount; ++i)
      if (!g_fieldSlots[i].busy) { pick = i; break; }
  }
  if (pick < 0) {
    HostLog("[field] all %d slots busy, falling back", kFieldSlotCount);
    return -1;
  }

  FieldSlot& s = g_fieldSlots[pick];
  if (!s.buf || s.w != w || s.h != h || s.bytes < bytes) {
    ICoreWebView2SharedBuffer* sb = nullptr;
    HRESULT hr = g_env12->CreateSharedBuffer(bytes, &sb);
    if (FAILED(hr) || !sb) {
      HostLog("[field] CreateSharedBuffer failed %llu hr=0x%08x",
              (unsigned long long)bytes, (unsigned)hr);
      return -1;
    }
    BYTE* p = nullptr;
    hr = sb->get_Buffer(&p);
    if (FAILED(hr) || !p) {
      sb->Release();
      HostLog("[field] get_Buffer failed hr=0x%08x", (unsigned)hr);
      return -1;
    }
    /* One-time cost, not per frame: a fresh buffer must start at zero so an
     * aborted first pass cannot publish uninitialised memory.  Later passes
     * write every texel (glitches explicitly zero theirs). */
    std::memset(p, 0, (size_t)bytes);
    if (s.buf) s.buf->Release();
    s.buf = sb;
    s.ptr = p;
    s.w = w;
    s.h = h;
    s.bytes = bytes;
    ++g_fieldCreated;
    HostLog("[field] created slot %d %dx%d %llu KB (total %d)", pick, w, h,
            (unsigned long long)(bytes / 1024), g_fieldCreated);
  }
  /* The layout the kernel and the page agree on for *this* pass, which may be
   * smaller than what the buffer could hold. */
  s.channels = channels;
  s.busy = true;
  s.posted = false;
  s.seq = seq;
  s.since = now;
  return pick;
}

/* The OpenGL canvas owns the picture, so the page's per-pass shared buffers are
 * bypassed entirely: the shell keeps one CPU surface for the pass, and that
 * surface is the thing uploaded to the canvas.  The page still drives *which*
 * tiles to render and in what order, which is its real job. */
static bool EnsureHostPass(int viewW, int viewH, int viewId) {
  if (viewW <= 0 || viewH <= 0) return false;
  std::lock_guard<std::mutex> lk(g_frameMu);
  if (g_frame.ptr && g_frame.cpu && g_frame.viewId == viewId &&
      g_frame.w == viewW && g_frame.h == viewH)
    return true;
  if (g_frame.buf) { g_frame.buf->Release(); g_frame.buf = nullptr; }
  g_frame.cpu = std::make_shared<std::vector<uint8_t>>(
      (size_t)viewW * (size_t)viewH * 4, 0);
  g_frame.ptr = g_frame.cpu->data();
  g_frame.w = viewW;
  g_frame.h = viewH;
  g_frame.viewId = viewId;
  g_frame.ss = 1;
  g_frame.cw = viewW;
  g_frame.ch = viewH;
  g_frame.finalPass = true;
  canvas::BeginPass(viewW, viewH, 1, viewW, viewH);
  LogMsg("[frame] gl pass " + std::to_string(viewW) + "x" + std::to_string(viewH) +
         " view=" + std::to_string(viewId));
  return true;
}

static bool EnsureFrame(int w, int h, int viewId, int ss, int cw, int ch) {
  if (!g_env12 || w <= 0 || h <= 0) return false;
  std::lock_guard<std::mutex> lk(g_frameMu);
  const bool same = g_frame.ptr && g_frame.w == w && g_frame.h == h &&
                    g_frame.viewId == viewId && g_frame.ss == ss;
  if (same) return true;
  const uint64_t bytes = (uint64_t)w * (uint64_t)h * 4;
  if (g_nativeCanvas) {
    if (ss < 1) ss = 1;
    if (ss > 4) ss = 4;
    // A shared_ptr keeps the old surface alive for any tile still in flight.
    g_frame.cpu = std::make_shared<std::vector<uint8_t>>((size_t)bytes);
    g_frame.ptr = g_frame.cpu->data();
    if (g_frame.buf) { g_frame.buf->Release(); g_frame.buf = nullptr; }
    g_frame.w = w;
    g_frame.h = h;
    g_frame.viewId = viewId;
    g_frame.ss = ss;
    g_frame.cw = cw;
    g_frame.ch = ch;
    g_frame.finalPass = cw > 0 && ch > 0 && w == cw * ss && h == ch * ss;
    canvas::BeginPass(w, h, ss, cw, ch);
    LogMsg("[frame] native " + std::to_string(w) + "x" + std::to_string(h) + " ss=" +
           std::to_string(ss) + " canvas=" + std::to_string(cw) + "x" + std::to_string(ch));
    return true;
  }
  ICoreWebView2SharedBuffer* sb = nullptr;
  if (FAILED(g_env12->CreateSharedBuffer(bytes, &sb)) || !sb) {
    LogMsg("[frame] CreateSharedBuffer failed " + std::to_string(bytes));
    return false;
  }
  BYTE* p = nullptr;
  if (FAILED(sb->get_Buffer(&p)) || !p) {
    sb->Release();
    LogMsg("[frame] get_Buffer failed");
    return false;
  }
  // The page has already consumed the previous frame by the time it asks for
  // the next one (it declares the new pass only after painting the old one).
  if (g_frame.buf) g_frame.buf->Release();
  g_frame.cpu.reset();
  g_frame.buf = sb;
  g_frame.ptr = p;
  g_frame.w = w;
  g_frame.h = h;
  g_frame.viewId = viewId;
  g_frame.ss = ss;
  g_frame.cw = cw;
  g_frame.ch = ch;
  g_frame.finalPass = false;
  LogMsg("[frame] " + std::to_string(w) + "x" + std::to_string(h) + " " +
         std::to_string(bytes / 1024) + " KB");
  return true;
}

/** Publish the finished frame in one shot (the page asked with {type:"flush"}). */
static void PostFrame(int viewId, bool keepPreview = false,
                      double sx = 1.0, double ox = 0.0,
                      double sy = 1.0, double oy = 0.0) {
  if (!g_wv17) return;
  std::lock_guard<std::mutex> lk(g_frameMu);
  if (!g_frame.ptr || g_frame.viewId != viewId) return;
  if (g_nativeCanvas) {
    /* A live pass may arrive while a wheel/drag gesture is still running.
     * Keep the accumulated preview transform so the new frame is already at
     * the current view instead of snapping back (see nc::CommitPassWithPreview). */
    const bool committed = canvas::Commit(keepPreview, sx, ox, sy, oy);
    int w = g_frame.w, h = g_frame.h, ss = g_frame.ss;
    if (g_frame.finalPass) {
      std::lock_guard<std::mutex> lk2(g_lastMu);
      if (g_glCanvas) {
        /* The pixels never came back to the CPU: they are in the GL_RGB10_A2
         * scene texture.  Read that back once, at canvas resolution, so PNG
         * export matches what the window shows. */
        const int ew = g_frame.cw > 0 ? g_frame.cw : w;
        const int eh = g_frame.ch > 0 ? g_frame.ch : h;
        g_lastFinal.assign((size_t)ew * (size_t)eh * 4, 0);
        if (!gpur::readback(g_lastFinal.data(), ew, eh)) g_lastFinal.clear();
        g_lastW = ew;
        g_lastH = eh;
        g_lastSS = 1;
      } else {
        g_lastFinal = std::move(*g_frame.cpu);
        g_lastW = w;
        g_lastH = h;
        g_lastSS = ss;
      }
    }
    g_frame.cpu.reset();
    g_frame.ptr = nullptr;
    char meta[256];
    /* `ok` tells the page whether the swapchain really took the frame.  It
     * only switches to the transparent native surface when it did, so a
     * failed present can never leave the window blank. */
    snprintf(meta, sizeof(meta),
             "{\"type\":\"nativeflush\",\"viewId\":%d,\"w\":%d,\"h\":%d,\"ss\":%d,\"ok\":%d}",
             viewId, w, h, ss, committed ? 1 : 0);
    PostJson(meta);
    if (g_framePosted < 6) {
      LogMsg("[native] commit " + std::to_string(w) + "x" + std::to_string(h) + " ss=" +
             std::to_string(ss) + " present=" + std::to_string(canvas::PresentMs()) +
             "ms frames=" + std::to_string(canvas::Frames()) + " ok=" + (committed ? "1" : "0"));
      g_framePosted++;
    }
    return;
  }
  char meta[256];
  snprintf(meta, sizeof(meta),
           "{\"type\":\"frame\",\"viewId\":%d,\"w\":%d,\"h\":%d,\"x\":0,\"y\":0}",
           viewId, g_frame.w, g_frame.h);
  std::wstring wmeta(meta, meta + strlen(meta));
  HRESULT hr = g_wv17->PostSharedBufferToScript(
      g_frame.buf, COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, wmeta.c_str());
  if (g_framePosted < 4) {
    LogMsg("[frame] post " + std::to_string(g_frame.w) + "x" + std::to_string(g_frame.h) +
           " hr=" + std::to_string((int)hr));
    g_framePosted++;
  }
  if (FAILED(hr)) LogMsg("[frame] PostSharedBufferToScript failed hr=" + std::to_string((int)hr));
}

// ---------------------------------------------------------------- render jobs

static void HostLog(const char* fmt, ...);

struct Job {
  uint64_t seq = 0;
  int viewId = 0;
  int x = 0, y = 0, w = 1, h = 1;
  int viewW = 1, viewH = 1;
  bool frame = false;
  std::string centerRe = "-0.6", centerIm = "0";
  double spanMant = 1.6;
  int spanExp = 1;
  int maxIter = 600;
  int bits = 0;
  int colorMode = 0;      // 0 Smooth 1 Iteration 2 Distance 3 Distance+levels
  double offset = 0.0;
  int quality = 1;
  bool dynamicIter = true;
  int scheme = 0;
  int shade = 0;          // band-density policy: 0 legacy / 1 stretched / 2 multi-cycle / 3 dense
  bool dither = false;    // independent 8x8 ordered dither on the LUT path
  int mode = 0, kind = 0, power = 2;
  /* User formula (kind 6): the expression the page's editor holds, plus its
   * sliders as "a=0.5;b=1".  Parsed by the core's own mathx library. */
  std::string expr;
  std::string params;
  double juliaRe = 0.0, juliaIm = 0.0, phoenixP = 0.5667;
  std::string juliaReS, juliaImS;   // arbitrary-precision Julia c (may be empty)
  int pal = 0;            // index into mbc::builtinPalette()
  /* Colour set.  `stops` is the page's gradient ([pos,r,g,b] x n, 0..1); when
   * it is present it replaces the built-in ramp entirely, which is what makes
   * the editor's changes actually reach the pixels. */
  std::vector<float> stops;
  /* Additive palette from the reference program's examples: a flat list of
   * (r, g, b, frequency, initial) quintuples.  The page sends this when the
   * selected scheme is one of the transcribed example palettes; `stops` is
   * then empty and vice versa. */
  std::vector<float> colors;
  float briFinite = 1.0f, briInfinite = 1.0f;
  double rotation = 0.0;  // view angle in radians (the reference's `rotation`)
  int aa = 1;             // n x n supersamples per pixel (1 = off)
  int detail = 3;         // 0..3, smallest feature the frame resolves
  int traps = 0;          // orbit trap 1..4, 0 = off (see Request::traps)
  /* Set by the page from the active shader's CHANNEL_* mask.  False lets the
   * kernel skip the per-iteration derivative recurrence on a field pass. */
  bool fieldDist = true;
  /* Set by the page from the active shader's CHANNEL_VALUE / CHANNEL_DERIVATIVE
   * bits.  The field then grows a second plane holding z at the escape point,
   * which is what the "Texture" scheme continues for twenty steps of its own.
   * Only that scheme pays for it, so the default stays four channels. */
  bool fieldValue = false;
  /* Field pass.  The core then computes the iteration field and the page
   * shades it on the GPU (see mbc::Field).  It is a whole-frame request: the
   * shader needs the frame's own pixel grid for its screen-space terms, so a
   * field pass is never a tile. */
  bool field = false;
  /* Index into g_fieldSlots, reserved on the UI thread.  -1 means the RGBA
   * fallback; the render thread writes the iteration field straight into this
   * slot's shared buffer and never allocates a staging vector. */
  int fieldSlot = -1;
  int bands = 0;          // palette cycles across the visible iteration range
  double cycle = 0.0;     // explicit iterations per cycle (0 = fit to frame)
  float toneBri = 1.0f, toneCon = 1.0f, toneGam = 1.0f, toneSat = 1.0f;
  float toneR = 1.0f, toneG = 1.0f, toneB = 1.0f;
};

struct Done {
  uint64_t seq = 0;
  int slot = 0;
  bool frame = false;
  int viewId = 0;
  int x = 0, y = 0, w = 1, h = 1;
  std::string id;
  int bits = 0, refLen = 0, glitches = 0, maxIter = 0, threads = 1;
  /* The budget this pass was ASKED for (not the one the core ended up using).
   * A frame that escapes at `cap` did not "run out of detail", it ran out of
   * iterations; the page must not print "no structure" for it. */
  int cap = 0;
  int escMin = 0, escMax = 0, bounded = 0, directFixed = 0;
  long long iterPixels = 0;
  int canvasVerts = 0, canvasFilled = 0, canvasCacheHit = 0;
  double iterPerCycle = 0;
  bool dd = false, perturb = false, unsupported = false;
  bool field = false;
  bool gpuField = false;      // the tile's field was produced by OpenCL and
                              // already lives in the OpenGL pass texture
  int fieldSlot = -1;
  int fieldChannels = 4;      // 4 = colour plane, 6 = colour + value plane
  bool orbitReused = false;   // the environment (reference orbit) came from the cache
  double orbitMs = 0, renderMs = 0;
};

static std::deque<Job> g_jobs;
static std::mutex g_jobMu;
static std::condition_variable g_jobCv;
static std::atomic<bool> g_stopRender{false};
static std::atomic<int> g_cancelView{0};
static std::deque<Done> g_done;
static std::mutex g_doneMu;

static mbc::Engine* g_engine = nullptr;
static int g_renderSeen = 0;
static int g_doneSeen = 0;
static int g_ackSeen = 0;

// Rolling throughput window: when the queue drains we log one line per pass.
static std::atomic<bool> g_inFlight{false};
static std::mutex g_perfMu;
static double g_perfMs = 0.0;
static uint64_t g_perfPx = 0;
static int g_perfTiles = 0;
static int g_perfView = 0;
static bool g_perfActive = false;
static std::chrono::steady_clock::time_point g_perfWallStart;

#define WM_APP_DONE (WM_APP + 1)

// ---------------------------------------------------------------- tiny JSON read helpers

static bool JsonFind(const std::string& s, const char* key, size_t& pos) {
  std::string k = std::string("\"") + key + "\"";
  size_t p = s.find(k);
  if (p == std::string::npos) return false;
  p = s.find(':', p + k.size());
  if (p == std::string::npos) return false;
  pos = p + 1;
  return true;
}

static double JsonNum(const std::string& s, const char* key, double dflt) {
  size_t p;
  if (!JsonFind(s, key, p)) return dflt;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
  char* end = nullptr;
  double v = std::strtod(s.c_str() + p, &end);
  if (end == s.c_str() + p) return dflt;
  return v;
}

static int JsonInt(const std::string& s, const char* key, int dflt) {
  return (int)std::llround(JsonNum(s, key, (double)dflt));
}

static bool JsonBool(const std::string& s, const char* key, bool dflt) {
  size_t p;
  if (!JsonFind(s, key, p)) return dflt;
  while (p < s.size() && (s[p] == ' ')) ++p;
  if (s.compare(p, 4, "true") == 0) return true;
  if (s.compare(p, 5, "false") == 0) return false;
  return dflt;
}

static std::string JsonStr(const std::string& s, const char* key, const std::string& dflt) {
  size_t p;
  if (!JsonFind(s, key, p)) return dflt;
  while (p < s.size() && (s[p] == ' ')) ++p;
  if (p >= s.size() || s[p] != '"') return dflt;
  ++p;
  std::string out;
  for (size_t i = p; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\' && i + 1 < s.size()) {
      char e = s[++i];
      if (e == 'n') out.push_back('\n');
      else if (e == 't') out.push_back('\t');
      else if (e == 'u') i += 4;
      else out.push_back(e);
      continue;
    }
    if (c == '"') break;
    out.push_back(c);
  }
  return out;
}

// ---------------------------------------------------------------- parameter file

static bool ReadAllText(const std::wstring& path, std::string& out);
static std::string JsonEscapeStr(const std::string& s);

/* The reference program keeps its knobs in `appdata\mandelbrot.json`
 * (winparameters.html): framerate / nthread / totalmemory / graphicsmemory /
 * graphicsrate / cachesize / canvas size / guiscale / linewidth / pointsize /
 * ntrace / GLFW major+minor / yreduce (+ videooptions).  We read the same file,
 * apply the same defaults and ranges, and hand the effective values to the page
 * so the settings panel can show them.  Keys that only make sense on GLFW
 * (context version, yreduce, video options) are parsed and reported, not acted
 * on; nthread is applied before the engine starts. */
struct AppConfig {
  double framerate = 60.0;
  int nthread = 0;                 // 0 = logical cores (see note in the ledger)
  int totalmemory = 0;             // MB, 0 = physical / 2
  int graphicsmemory = 2000;       // MB, doc example ("guessed from resolution")
  int graphicsrate = 400;          // MB/s, graphicsmemory / 5
  int cachesize = 10;              // 9 / 10 / 11
  int canvassize = 7;              // 7 / 8 / 9 -> 128 / 256 / 512 textures
  double guiscale = 1.0;
  double linewidth = 1.0;
  double pointsize = 5.0;
  int ntrace = 1000;
  int glmaj = 3, glmin = 0;
  int yreduce = 1;
  bool loaded = false;
  std::wstring path;
};
static AppConfig g_cfg;

static int ClampInt(double v, int lo, int hi, int dflt) {
  if (!(v == v) || v < lo || v > hi) return dflt;   // NaN or out of range
  return (int)std::llround(v);
}

static void LoadAppConfig() {
  AppConfig c;
  c.nthread = 0;
  /* Physical memory decides the documented default: half of it. */
  MEMORYSTATUSEX ms = {};
  ms.dwLength = sizeof(ms);
  if (GlobalMemoryStatusEx(&ms)) c.totalmemory = (int)(ms.ullTotalPhys / (1024ull * 1024ull) / 2);

  std::wstring p = ExeDir() + L"\\appdata\\mandelbrot.json";
  if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES)
    p = AppDir() + L"\\mandelbrot.json";
  std::string text;
  if (ReadAllText(p, text) && !text.empty()) {
    c.loaded = true;
    c.path = p;
    auto note = [&](const char* key, double raw, double lo, double hi) {
      if (raw < lo || raw > hi)
        LogMsg(std::string("[cfg] ") + key + " out of range -> default");
    };
    double v;
    v = JsonNum(text, "framerate", c.framerate);
    note("framerate", v, 1, 1000);
    c.framerate = (v >= 1 && v <= 1000) ? v : c.framerate;

    v = JsonNum(text, "nthread", 0);
    /* 0 is our "auto" (logical cores); the reference only documents 1..256. */
    note("nthread", v, 0, 256);
    c.nthread = (v >= 0 && v <= 256) ? (int)std::llround(v) : 0;

    v = JsonNum(text, "totalmemory", c.totalmemory);
    note("totalmemory", v, 0, 1048576);
    if (v >= 0 && v <= 1048576) c.totalmemory = v > 0 ? (int)std::llround(v) : c.totalmemory;

    v = JsonNum(text, "graphicsmemory", c.graphicsmemory);
    note("graphicsmemory", v, 16, 1048576);
    c.graphicsmemory = ClampInt(v, 16, 1048576, c.graphicsmemory);

    c.graphicsrate = ClampInt(JsonNum(text, "graphicsrate", c.graphicsmemory / 5.0),
                              1, 1000000, c.graphicsmemory / 5);

    v = JsonNum(text, "cachesize", c.cachesize);
    c.cachesize = (v == 9 || v == 10 || v == 11) ? (int)v : 10;
    if (v != 9 && v != 10 && v != 11) LogMsg("[cfg] cachesize not 9/10/11 -> 10");

    /* The reference spells this one "internal canvas size"; the distribution
     * ships separate exes for 8 and 9, so only 7..9 are valid. */
    v = JsonNum(text, "canvassize", JsonNum(text, "canvas size", c.canvassize));
    c.canvassize = (v == 7 || v == 8 || v == 9) ? (int)v : 7;

    v = JsonNum(text, "guiscale", c.guiscale);
    c.guiscale = (v >= 0.5 && v <= 3) ? v : 1.0;

    v = JsonNum(text, "linewidth", c.linewidth);
    c.linewidth = (v >= 0.1 && v <= 100) ? v : 1.0;
    v = JsonNum(text, "pointsize", c.pointsize);
    c.pointsize = (v >= 1 && v <= 100) ? v : 5.0;
    c.ntrace = ClampInt(JsonNum(text, "ntrace", c.ntrace), 1, 100000, c.ntrace);

    c.glmaj = ClampInt(JsonNum(text, "GLFW_CONTEXT_VERSION_MAJOR", c.glmaj), 1, 4, c.glmaj);
    c.glmin = ClampInt(JsonNum(text, "GLFW_CONTEXT_VERSION_MINOR", c.glmin), 0, 9, c.glmin);
    c.yreduce = ClampInt(JsonNum(text, "yreduce", c.yreduce), 1, 64, c.yreduce);

    LogMsg("[cfg] loaded " + WideToUtf8(p) + " nthread=" + std::to_string(c.nthread) +
           " totalmemory=" + std::to_string(c.totalmemory) +
           " graphicsmemory=" + std::to_string(c.graphicsmemory) +
           " cachesize=" + std::to_string(c.cachesize) +
           " canvassize=" + std::to_string(c.canvassize));
  } else {
    LogMsg("[cfg] no mandelbrot.json -> documented defaults");
  }
  g_cfg = c;
}

static std::string ConfigJson() {
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"loaded\":%s,\"file\":\"%s\",\"framerate\":%.1f,\"nthread\":%d,"
           "\"totalmemory\":%d,\"graphicsmemory\":%d,\"graphicsrate\":%d,"
           "\"cachesize\":%d,\"canvassize\":%d,\"guiscale\":%.3f,\"linewidth\":%.2f,"
           "\"pointsize\":%.2f,\"ntrace\":%d,\"glMajor\":%d,\"glMinor\":%d,\"yreduce\":%d}",
           g_cfg.loaded ? "true" : "false", JsonEscapeStr(WideToUtf8(g_cfg.path)).c_str(),
           g_cfg.framerate, g_cfg.nthread, g_cfg.totalmemory, g_cfg.graphicsmemory,
           g_cfg.graphicsrate, g_cfg.cachesize, g_cfg.canvassize, g_cfg.guiscale,
           g_cfg.linewidth, g_cfg.pointsize, g_cfg.ntrace, g_cfg.glmaj, g_cfg.glmin,
           g_cfg.yreduce);
  return buf;
}

// ---------------------------------------------------------------- UI resources

/* Reads `"name":[[..],[..]]` out of a render message and flattens every number
 * it finds.  Only the numbers matter, so the array is regrouped by the caller:
 * that is far less code than a JSON parser and cannot drift out of step with
 * the page's own layout. */
static bool JsonNumberArray(const std::string& s, const char* name,
                            std::vector<float>& out, size_t cap) {
  size_t p;
  if (!JsonFind(s, name, p)) return false;
  while (p < s.size() && (s[p] == ' ' || s[p] == '\t')) ++p;
  if (p >= s.size() || s[p] != '[') return false;
  int depth = 0;
  for (size_t i = p; i < s.size(); ++i) {
    const char c = s[i];
    if (c == '[') { ++depth; continue; }
    if (c == ']') { --depth; if (depth <= 0) break; continue; }
    if (c == '-' || c == '+' || c == '.' || (c >= '0' && c <= '9')) {
      char* end = nullptr;
      const double v = std::strtod(s.c_str() + i, &end);
      if (end == s.c_str() + i) continue;
      out.push_back((float)v);
      i = (size_t)(end - s.c_str()) - 1;
      if (out.size() > cap) break;
    }
  }
  return !out.empty();
}

static bool JsonStops(const std::string& s, std::vector<float>& out) {
  if (!JsonNumberArray(s, "stops", out, 4 * 64)) return false;
  return out.size() >= 8 && (out.size() % 4) == 0;
}

static bool g_logResources = false;
static const UiFile* FindUiFile(const std::string& path) {
  std::string p = path;
  if (p.empty() || p == "/") p = "index.html";
  if (p[0] == '/') p.erase(p.begin());
  for (unsigned i = 0; i < kUiFileCount; ++i) {
    if (p == kUiFiles[i].path) return &kUiFiles[i];
  }
  return nullptr;
}

static std::string UriPath(const std::wstring& uri) {
  std::string u = WideToUtf8(uri);
  const std::string prefix = "https://mb.test/";
  if (u.compare(0, prefix.size(), prefix) != 0) return std::string();
  std::string p = u.substr(prefix.size());
  size_t q = p.find_first_of("?#");
  if (q != std::string::npos) p.resize(q);
  return p;
}

class ResHandler : public Cb<ICoreWebView2WebResourceRequestedEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2WebResourceRequestedEventArgs* args) override {
    if (!args || !g_env) return S_OK;
    ICoreWebView2WebResourceRequest* req = nullptr;
    args->get_Request(&req);
    if (!req) return S_OK;
    LPWSTR uri = nullptr;
    req->get_Uri(&uri);
    std::wstring wuri = uri ? uri : L"";
    if (uri) CoTaskMemFree(uri);
    req->Release();

    std::string path = UriPath(wuri);
    const UiFile* f = FindUiFile(path);
    if (!f) {
      LogMsg("[res] 404 " + path);
      return S_OK;
    }
    if (g_logResources) LogMsg("[res] 200 " + path);

    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, f->size ? f->size : 1);
    if (!mem) return S_OK;
    void* dst = GlobalLock(mem);
    memcpy(dst, f->data, f->size);
    GlobalUnlock(mem);
    IStream* stream = nullptr;
    if (FAILED(CreateStreamOnHGlobal(mem, TRUE, &stream)) || !stream) {
      GlobalFree(mem);
      return S_OK;
    }
    std::string headers = "Content-Type: " + std::string(f->mime) +
                          "\r\nCache-Control: no-store\r\n";
    std::wstring wh = std::wstring(headers.begin(), headers.end());
    ICoreWebView2WebResourceResponse* resp = nullptr;
    g_env->CreateWebResourceResponse(stream, 200, L"OK", wh.c_str(), &resp);
    stream->Release();
    if (resp) {
      args->put_Response(resp);
      resp->Release();
    }
    return S_OK;
  }
};

// ---------------------------------------------------------------- page messages

static ICoreWebView2* WebView() {
  /* g_wv was declared but never assigned, so every PostJson() call in this
   * file silently did nothing: no nativeflush, no tilemeta, no config push.
   * Resolve it from the versioned interface on first use instead of trusting a
   * pointer that only looks initialised. */
  if (g_wv) return g_wv;
  if (!g_wv17) return nullptr;
  ICoreWebView2* wv = nullptr;
  if (SUCCEEDED(g_wv17->QueryInterface(IID_PPV_ARGS(&wv))) && wv) g_wv = wv;
  return g_wv;
}

static void PostJson(const std::string& json) {
  ICoreWebView2* wv = WebView();
  if (!wv) return;
  /* UTF-8 -> UTF-16, not a byte-wise widening.
   *
   * The page speaks UTF-8 and so does every message built here, but
   * PostWebMessageAsJson takes a wide string.  Constructing it with
   * `std::wstring(json.begin(), json.end())` maps each *byte* to one wchar,
   * which is harmless for ASCII and turns every Chinese preset name into
   * mojibake — and, because the bytes are then re-interpreted as UTF-8 by the
   * receiver, several different names can collapse into the same garbage. */
  std::wstring w = Utf8ToWide(json);
  wv->PostWebMessageAsJson(w.c_str());
}

static void ToggleFullscreen(HWND hwnd);   // defined next to ResizeWebView

/* The native canvas never hands pixels back to JS, so PNG export happens here:
 * the last completed final pass is box-reduced by its supersampling factor and
 * written with WIC through the system Save As dialog.  The page only receives
 * a one-line result for its toast. */
static bool ExportPngNative() {
  std::vector<uint8_t> frame;
  int lw = 0, lh = 0, lss = 1;
  {
    // Copy under the lock, then release it: the Save As dialog runs a nested
    // message loop, and PostFrame must not block behind it.
    std::lock_guard<std::mutex> lk(g_lastMu);
    if (g_lastFinal.empty() || g_lastW <= 0 || g_lastH <= 0) {
      PostJson("{\"type\":\"exportResult\",\"ok\":false}");
      return false;
    }
    frame = g_lastFinal;
    lw = g_lastW; lh = g_lastH; lss = g_lastSS;
  }
  const int ss = lss < 1 ? 1 : lss;
  const int W = lw / ss, H = lh / ss;
  std::vector<uint8_t> bgra((size_t)W * H * 4);
  const uint8_t* src = frame.data();
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      unsigned r = 0, g = 0, b = 0, a = 0;
      for (int sy = 0; sy < ss; ++sy) {
        const uint8_t* row = src + ((size_t)(y * ss + sy) * lw + (size_t)x * ss) * 4;
        for (int sx = 0; sx < ss; ++sx) {
          r += row[sx * 4 + 0]; g += row[sx * 4 + 1];
          b += row[sx * 4 + 2]; a += row[sx * 4 + 3];
        }
      }
      const unsigned n = (unsigned)(ss * ss);
      uint8_t* d = &bgra[((size_t)y * W + x) * 4];
      d[0] = (uint8_t)(b / n); d[1] = (uint8_t)(g / n);
      d[2] = (uint8_t)(r / n); d[3] = (uint8_t)(a / n);
    }
  }

  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t name[96];
  swprintf_s(name, L"mandelbrot-%04d%02d%02d-%02d%02d%02d.png",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
  std::wstring path;
  const char* forced = std::getenv("MB_EXPORT_TARGET");   // headless export test
  if (forced && *forced) {
    path = Utf8ToWide(forced);
  } else {
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
      COMDLG_FILTERSPEC filt[1] = {{L"PNG image", L"*.png"}};
      dlg->SetFileTypes(1, filt);
      dlg->SetDefaultExtension(L"png");
      dlg->SetFileName(name);
      dlg->SetTitle(L"Save image");
      if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
          LPWSTR p = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            path = p;
            CoTaskMemFree(p);
          }
          item->Release();
        }
      }
      dlg->Release();
    }
  }
  if (path.empty()) {
    PostJson("{\"type\":\"exportResult\",\"ok\":false}");
    return false;
  }

  bool ok = false;
  IWICImagingFactory* fac = nullptr;
  if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&fac))) && fac) {
    IWICStream* stream = nullptr;
    if (SUCCEEDED(fac->CreateStream(&stream)) && stream) {
      if (SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE))) {
        IWICBitmapEncoder* enc = nullptr;
        if (SUCCEEDED(fac->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && enc) {
          if (SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache))) {
            IWICBitmapFrameEncode* frame = nullptr;
            IPropertyBag2* props = nullptr;
            if (SUCCEEDED(enc->CreateNewFrame(&frame, &props)) && frame) {
              frame->Initialize(props);
              frame->SetSize((UINT)W, (UINT)H);
              frame->SetResolution(96.0, 96.0);
              WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
              frame->SetPixelFormat(&fmt);
              if (SUCCEEDED(frame->WritePixels((UINT)H, (UINT)W * 4,
                                               (UINT)bgra.size(), bgra.data())) &&
                  SUCCEEDED(frame->Commit()) && SUCCEEDED(enc->Commit()))
                ok = true;
              frame->Release();
              if (props) props->Release();
            }
          }
          enc->Release();
        }
      }
      stream->Release();
    }
    fac->Release();
  }
  LogMsg(std::string("[export] ") + (ok ? "ok " : "failed ") + WideToUtf8(path) +
         " " + std::to_string(W) + "x" + std::to_string(H));
  PostJson(ok ? "{\"type\":\"exportResult\",\"ok\":true}"
              : "{\"type\":\"exportResult\",\"ok\":false}");
  return ok;
}

/* ---------------------------------------------------------------- presets
 *
 * The reference program stores a whole view as one JSON file (see its
 * examples/*.json).  Both directions go through the system file dialogs here
 * rather than through the page: a WebView2 page cannot read an arbitrary path,
 * and it has no download route that lands where the user pointed.  Doing it in
 * the shell also lets the file keep its original text — a 40-digit centre has
 * to survive the round trip byte for byte, and re-serialising it in JS would
 * not guarantee that.
 * ---------------------------------------------------------------- */

static std::string JsonEscapeStr(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 16);
  for (unsigned char c : s) {
    switch (c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          o += buf;
        } else {
          o += (char)c;
        }
    }
  }
  return o;
}

static bool ReadAllText(const std::wstring& path, std::string& out) {
  FILE* f = _wfopen(path.c_str(), L"rb");
  if (!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if (n < 0) { std::fclose(f); return false; }
  out.resize((size_t)n);
  const size_t got = n > 0 ? std::fread(&out[0], 1, (size_t)n, f) : 0;
  out.resize(got);
  std::fclose(f);
  return true;
}

static bool WriteAllText(const std::wstring& path, const std::string& text) {
  FILE* f = _wfopen(path.c_str(), L"wb");
  if (!f) return false;
  const size_t put = text.empty() ? 0 : std::fwrite(text.data(), 1, text.size(), f);
  std::fclose(f);
  return put == text.size();
}

/* UTF-8 text -> UTF-16 for the Win32 dialogs (the page speaks UTF-8). */
static std::wstring Utf8ToWideLocal(const std::string& s) {
  if (s.empty()) return std::wstring();
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
  return w;
}

/** Ask for a preset file and hand its text back to the page. */
static void ImportPresetDialog() {
  std::wstring path;
  /* Read the test hook as UTF-16 directly: getenv() hands back the ANSI code
   * page, so a Chinese or Japanese folder name would be mangled before it ever
   * reaches the dialogs. */
  wchar_t wforced[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"MB_IMPORT_TARGET", wforced, MAX_PATH) && wforced[0]) {
    path = wforced;
  } else {
    IFileOpenDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
      COMDLG_FILTERSPEC filt[2] = {{L"Preset (JSON)", L"*.json"},
                                   {L"All files", L"*.*"}};
      dlg->SetFileTypes(2, filt);
      dlg->SetTitle(L"Open preset");
      if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
          LPWSTR p = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            path = p;
            CoTaskMemFree(p);
          }
          item->Release();
        }
      }
      dlg->Release();
    }
  }
  std::string text;
  const bool ok = !path.empty() && ReadAllText(path, text);
  const std::string msg =
      std::string("{\"type\":\"presetImport\",\"ok\":") + (ok ? "true" : "false") +
      ",\"name\":\"" + JsonEscapeStr(ok ? WideToUtf8(path) : std::string()) +
      "\",\"text\":\"" + JsonEscapeStr(ok ? text : std::string()) + "\"}";
  LogMsg(std::string("[preset] import ") + (ok ? "ok" : "cancelled") + " " +
         WideToUtf8(path) + " " + std::to_string(text.size()) + "B");
  PostJson(msg);
}

/** Save the page's preset text under a name the user picks. */
static void ExportPresetDialog(const std::string& json) {
  if (json.empty()) {
    PostJson("{\"type\":\"presetExport\",\"ok\":false}");
    return;
  }
  std::wstring path;
  wchar_t wforced[MAX_PATH] = {0};
  if (GetEnvironmentVariableW(L"MB_EXPORT_PRESET", wforced, MAX_PATH) && wforced[0]) {
    path = wforced;
  } else {
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
      COMDLG_FILTERSPEC filt[1] = {{L"Preset (JSON)", L"*.json"}};
      dlg->SetFileTypes(1, filt);
      dlg->SetDefaultExtension(L"json");
      dlg->SetFileName(L"preset.json");
      dlg->SetTitle(L"Save preset");
      if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
          LPWSTR p = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
            path = p;
            CoTaskMemFree(p);
          }
          item->Release();
        }
      }
      dlg->Release();
    }
  }
  const bool ok = !path.empty() && WriteAllText(path, json);
  LogMsg(std::string("[preset] export ") + (ok ? "ok " : "failed ") +
         WideToUtf8(path) + " " + std::to_string(json.size()) + "B");
  PostJson(std::string("{\"type\":\"presetExport\",\"ok\":") +
           (ok ? "true" : "false") + ",\"name\":\"" +
           JsonEscapeStr(ok ? WideToUtf8(path) : std::string()) + "\"}");
}

/* ------------------------------------------------------------------
 * Synchronous view bridge.
 *
 * The page used to do the zoom/pan arithmetic itself, in its own BigInt
 * fixed point.  That is a second implementation of the same maths the
 * renderer already owns, and its limits (16384 bits, a 2^-3322 pixel floor)
 * are exactly what turned a gesture into a no-op or into a rounding coarser
 * than a pixel at high magnification.
 *
 * WebView2's postMessage channel is asynchronous, so a page-side gesture
 * could never wait for a C++ answer.  A *host object* can: `AddHostObject
 * ToScript` exposes an IDispatch that the page reaches through
 * `chrome.webview.hostObjects.sync.mbview`, which blocks the renderer until
 * this Invoke() returns.  One method, one JSON string in, one JSON string
 * out — no VARIANT marshalling beyond a BSTR, and no chance of the two sides
 * disagreeing about the type of a 40-digit coordinate.
 *
 * The whole call is a handful of GMP operations (add + shift on a number of
 * `bits` bits), so it stays far below one frame even at 1e6 bits.
 * ------------------------------------------------------------------ */
class ViewHostObject : public IDispatch {
 public:
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (!pp) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDispatch) {
      *pp = static_cast<IDispatch*>(this);
      AddRef();
      return S_OK;
    }
    *pp = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = --ref_;
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfoCount(UINT* n) override {
    if (!n) return E_POINTER;
    *n = 0;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetTypeInfo(UINT, LCID, ITypeInfo**) override {
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE GetIDsOfNames(REFIID, LPOLESTR* names, UINT n,
                                          LCID, DISPID* ids) override {
    if (n != 1 || !names || !ids) return E_INVALIDARG;
    /* `in` / `out` are the data channel, `call` is the zero-argument probe.
     *
     * The method form takes its argument as a BSTR, and a sync proxy without
     * type information does not marshal arguments at all (measured: Invoke
     * arrives with cArgs = 0 whatever the page passes).  A property put does
     * carry its VARIANT, so the request travels that way. */
    if (!_wcsicmp(names[0], L"in")) ids[0] = 1;
    else if (!_wcsicmp(names[0], L"out")) ids[0] = 2;
    else if (!_wcsicmp(names[0], L"call")) ids[0] = 3;
    else return DISP_E_UNKNOWNNAME;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE Invoke(DISPID id, REFIID, LCID, WORD flags,
                                   DISPPARAMS* p, VARIANT* out, EXCEPINFO*,
                                   UINT*) override {
    static int told = 0;
    if (told++ < 6) {
      char b[128];
      std::snprintf(b, sizeof(b), "[view] Invoke id=%ld flags=0x%x args=%u", (long)id,
                    (unsigned)flags, p ? p->cArgs : 0u);
      LogMsg(b);
    }
    if (id == 1 && (flags & DISPATCH_PROPERTYPUT)) {
      if (p && p->cArgs >= 1 && p->rgvarg && p->rgvarg[0].vt == VT_BSTR &&
          p->rgvarg[0].bstrVal) {
        pending_ = WideToUtf8(p->rgvarg[0].bstrVal);
        answer_ = Handle(pending_);
      }
      return S_OK;
    }
    if (id == 2 && (flags & DISPATCH_PROPERTYGET)) {
      if (out) {
        out->vt = VT_BSTR;
        out->bstrVal = SysAllocString(Utf8ToWide(answer_).c_str());
      }
      return S_OK;
    }
    if (id == 3 || id == DISPID_VALUE) {
      const std::string res = Handle(std::string());
      if (out) {
        out->vt = VT_BSTR;
        out->bstrVal = SysAllocString(Utf8ToWide(res).c_str());
      }
      return S_OK;
    }
    return DISP_E_MEMBERNOTFOUND;
  }

  /** Used by the method form (kept for callers that can pass a BSTR). */
  HRESULT DispatchRequest(const std::string& req, VARIANT* out) {
    const std::string res = Handle(req);
    if (out) {
      out->vt = VT_BSTR;
      out->bstrVal = SysAllocString(Utf8ToWide(res).c_str());
    }
    return S_OK;
  }

  /** op = "zoom" | "pan" | "probe". */
  static std::string Handle(const std::string& in) {
    const std::string op = JsonStr(in, "op", "");
    const std::string cre = JsonStr(in, "re", "0");
    const std::string cim = JsonStr(in, "im", "0");
    const double spanMant = JsonNum(in, "spanMant", 1.0);
    const int spanExp = JsonInt(in, "spanExp", 0);
    const int W = JsonInt(in, "w", 0);
    const int H = JsonInt(in, "h", 0);
    const double rot = JsonNum(in, "rot", 0.0);
    const double a = JsonNum(in, "a", 0.0);
    const double b = JsonNum(in, "b", 0.0);
    const double factor = JsonNum(in, "factor", 1.0);
    int bits = JsonInt(in, "bits", 0);
    if (bits <= 0 && W > 0) {
      const double psLog2 = std::log2(std::fabs(spanMant) / (double)W) + spanExp;
      bits = mbc::view::bitsForView(psLog2, 0);
    }

    mbc::view::Result r;
    if (op == "zoom") {
      r = mbc::view::zoomAt(cre, cim, spanMant, spanExp, W, H, a, b, factor,
                            rot, bits);
    } else if (op == "pan") {
      r = mbc::view::panBy(cre, cim, spanMant, spanExp, W, H, a, b, rot, bits);
    } else {
      char buf[256];
      std::snprintf(buf, sizeof(buf),
                    "{\"ok\":%s,\"gmp\":%s,\"version\":\"%s\",\"path\":\"%s\"}",
                    mbc::gmp::available() ? "true" : "false",
                    mbc::gmp::available() ? "true" : "false",
                    mbc::gmp::version(), mbc::gmp::loadedPath());
      std::string out(buf);
      /* Windows paths carry backslashes, which JSON needs escaped. */
      std::string esc;
      for (char ch : out) {
        if (ch == '\\') esc += "\\\\";
        else esc += ch;
      }
      return esc;
    }
    if (!r.ok) {
      char buf[128];
      std::snprintf(buf, sizeof(buf), "{\"ok\":false,\"why\":\"%s\"}", r.why);
      return std::string(buf);
    }
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "\"spanMant\":%.17g,\"spanExp\":%d}", r.spanMant, r.spanExp);
    std::string out = "{\"ok\":true,\"re\":\"";
    out += r.re;
    out += "\",\"im\":\"";
    out += r.im;
    out += "\",";
    out += buf;
    return out;
  }

 private:
  ULONG ref_ = 1;
  std::string pending_;   // last request written through `in`
  std::string answer_;    // its answer, read back through `out`
};

class MsgHandler : public Cb<ICoreWebView2WebMessageReceivedEventHandler> {
 public:  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2*,
                                   ICoreWebView2WebMessageReceivedEventArgs* e) override {
    if (!e) return S_OK;
    // The page posts a JSON *string*; get_WebMessageAsString returns it verbatim,
    // while get_WebMessageAsJson would hand back an escaped JSON string literal.
    LPWSTR raw = nullptr;
    if (FAILED(e->TryGetWebMessageAsString(&raw)) || !raw) {
      if (FAILED(e->get_WebMessageAsJson(&raw)) || !raw) return S_OK;
    }
    std::string s = WideToUtf8(raw);
    CoTaskMemFree(raw);

    if (s.find("\"hello\"") != std::string::npos) {
      /* `surface` names what is actually on screen behind the page:
       *   dcomp = composition visual, the page can be transparent over it
       *   gl    = the OpenGL child window (experimental)
       *   dib   = GDI child window; a page HWND can never show it through,
       *           so the page keeps painting its own canvas in that case
       *   page  = no native canvas at all
       * `composite` is the flag the page gates transparency on. */
      const char* back = canvas::Backend();
      std::string r = "{\"type\":\"ready\",\"threads\":" +
                      std::to_string(g_engine ? g_engine->threadCount() : 1) +
                      ",\"shm\":" + (g_slotsReady ? "true" : "false") +
                      ",\"frame\":" + (g_env12 ? "true" : "false") +
                      ",\"native\":" + (g_nativeCanvas ? "true" : "false") +
                      ",\"surface\":\"" + back + "\"" +
                      ",\"composite\":" + (g_compMode ? "true" : "false") +
                      ",\"glnative\":" + (g_glCanvas ? "true" : "false") +
                      ",\"formula\":true" +
                      ",\"cfg\":" + ConfigJson() + "}";
      PostJson(r);
      /* The preset library ships inside the page bundle (presets-data.js).
       * There is no folder to index, so `ready` is the whole handshake. */
      if (g_nativeCanvas && std::getenv("MB_NATIVE_SELFTEST"))
        SetTimer(g_hwnd, 0x61, 1600, nullptr);
      /* A preset named on the command line loads itself once the page is up:
       * this is how the import path is exercised without a human clicking
       * through the file dialog. */
      if (const char* imp = std::getenv("MB_IMPORT_TARGET")) {
        if (imp[0]) ImportPresetDialog();
      }
      /* Open a preset out of the library, or a panel, by name.  This is the
       * same path a file argument takes, and it is how the preset panel and the
       * library are exercised without a human clicking. */
      {
        std::string want;
        wchar_t wbuf[MAX_PATH] = {0};
        if (GetEnvironmentVariableW(L"MB_OPEN_PRESET", wbuf, MAX_PATH) && wbuf[0]) {
          const std::string name = WideToUtf8(wbuf);
          want = "{\"type\":\"openPreset\",\"name\":\"" +
                 JsonEscapeStr(name) + "\"}";
        } else if (const char* panel = std::getenv("MB_OPEN_PANEL")) {
          if (panel[0]) {
            want = "{\"type\":\"openPanel\",\"panel\":\"" +
                   JsonEscapeStr(panel) + "\"}";
          }
        }
        if (!want.empty()) {
          SetTimer(g_hwnd, 0x67, 900, nullptr);
          g_pendingShellMsg = want;
        }
      }
      return S_OK;
    }

    const std::string ty = JsonStr(s, "type", "");
    if (ty == "splashPainted") {
      /* The page has the icon, the wordmark and the progress rail on screen:
       * this is the first pixel the user is allowed to see. */
      KillTimer(g_hwnd, 0x69);
      ShowAppWindow("page splash");
      return S_OK;
    }
    if (ty == "cmd") {
      // Window-level commands the page cannot perform itself (WebView2 has no
      // Fullscreen API); see ToggleFullscreen.
      const std::string cmd = JsonStr(s, "cmd", "");
      LogMsg("[cmd] " + cmd);
      if (cmd == "fullscreen") ToggleFullscreen(g_hwnd);
      return S_OK;
    }
    if (ty == "frame") {
      // The page declares the pass surface before dispatching its tiles.
      EnsureFrame(JsonInt(s, "w", 0), JsonInt(s, "h", 0), JsonInt(s, "viewId", 0),
                  JsonInt(s, "ss", 1), JsonInt(s, "cw", 0), JsonInt(s, "ch", 0));
      return S_OK;
    }
    if (ty == "flush") {
      const bool keep = JsonInt(s, "keep", 0) != 0;
      PostFrame(JsonInt(s, "viewId", 0), keep,
                JsonNum(s, "sx", 1.0), JsonNum(s, "ox", 0.0),
                JsonNum(s, "sy", 1.0), JsonNum(s, "oy", 0.0));
      return S_OK;
    }
    if (ty == "preview") {
      /* Any native surface takes the gesture.  Gating this on the D3D canvas
       * alone silently dropped every wheel preview once the OpenGL canvas
       * owned the picture, so the image froze for the whole gesture and then
       * snapped to the new frame -- the "jitter" the wheel reported.
       *
       * A preview is only meaningful while the frame it belongs to is what is
       * on screen.  Once a newer view has been committed, replaying an old
       * gesture would move the *new* picture by a stale offset, so it is
       * dropped instead. */
      const int pv = JsonInt(s, "view", -1);
      /* A gesture mutates the view but does not advance the page's view id --
       * that happens when the next render starts.  So a preview normally
       * carries the SAME id as the frame it is moving, and only a strictly
       * older id is stale (a frame from a previous view already landed). */
      const bool fresh = (pv < 0) || (pv >= g_glFieldView);
      /* Any native surface takes the gesture: the GL canvas, the composition
       * canvas and the DIB fallback all own the pixels and none of them can
       * move a picture the page transforms.  Gating this on g_glCanvas alone
       * left the native canvas frozen for the whole gesture and then snapping
       * to the new frame -- the exact "does not follow the hand" symptom. */
      if (g_glCanvas || g_nativeCanvas) {
        if (fresh) {
          ++g_prevApplied;
          /* `abs` carries the whole gesture transform rather than one step of
           * it.  A pan computes its offset from the frame on screen, so the
           * composing form turned every pointer event into another copy of the
           * same translation (the image accelerating away from the cursor). */
          if (JsonInt(s, "abs", 0)) {
            canvas::SetPreview(JsonNum(s, "sx", 1.0), JsonNum(s, "ox", 0.0),
                               JsonNum(s, "sy", 1.0), JsonNum(s, "oy", 0.0));
          } else {
            canvas::Preview(JsonNum(s, "scale", 1.0), JsonNum(s, "dx", 0.0),
                            JsonNum(s, "dy", 0.0), JsonNum(s, "cx", 0.5),
                            JsonNum(s, "cy", 0.5));
          }
          if ((g_prevApplied % 60) == 1)
            LogMsg("[preview] applied=" + std::to_string(g_prevApplied) +
                   " stale=" + std::to_string(g_prevStale));
        } else {
          if ((++g_prevStale % 30) == 1)
            LogMsg("[preview] stale=" + std::to_string(g_prevStale) +
                   " pv=" + std::to_string(pv) +
                   " committed=" + std::to_string(g_glFieldView));
        }
      } else if (g_nativeCanvas) {
        if (JsonInt(s, "abs", 0)) {
          canvas::SetPreview(JsonNum(s, "sx", 1.0), JsonNum(s, "ox", 0.0),
                             JsonNum(s, "sy", 1.0), JsonNum(s, "oy", 0.0));
        } else {
          canvas::Preview(JsonNum(s, "scale", 1.0), JsonNum(s, "dx", 0.0),
                          JsonNum(s, "dy", 0.0), JsonNum(s, "cx", 0.5),
                          JsonNum(s, "cy", 0.5));
        }
      }
      return S_OK;
    }
    if (ty == "exportPng") {
      ExportPngNative();
      return S_OK;
    }
    if (ty == "importPreset") {
      ImportPresetDialog();
      return S_OK;
    }
    if (ty == "exportPreset") {
      ExportPresetDialog(JsonStr(s, "json", ""));
      return S_OK;
    }
    /* ---- preset library ------------------------------------------------
     * The library is embedded in the page bundle.  The only file operations
     * left are the two dialogs above (import / export of one JSON preset);
     * "save" lives in the page's own localStorage under "mine".
     * ------------------------------------------------------------------ */
    if (ty == "log") {
      LogMsg("[page] " + JsonStr(s, "msg", ""));
      return S_OK;
    }
    if (ty == "stats") {
      LogMsg("[stats] " + JsonStr(s, "msg", ""));
      return S_OK;
    }

    if (s.find("\"log\"") != std::string::npos) {
      LogMsg("[page] " + JsonStr(s, "msg", ""));
      return S_OK;
    }

    if (s.find("\"ack\"") != std::string::npos) {
      uint64_t seq = (uint64_t)JsonNum(s, "seq", 0);
      if (g_ackSeen < 6) { LogMsg("[ack] seq=" + std::to_string(seq)); g_ackSeen++; }
      /* The page has finished reading the field buffer.  Only now may the
       * kernel write into that slot again. */
      ReleaseFieldSeq(seq);
      std::lock_guard<std::mutex> lk(g_slotMu);
      for (int i = 0; i < kSlotCount; ++i) {
        if (g_slots[i].busy && g_slots[i].seq == seq) {
          g_slots[i].busy = false;
          g_slots[i].seq = 0;
        }
      }
      g_slotCv.notify_all();
      return S_OK;
    }

    if (s.find("\"cancel\"") != std::string::npos) {
      int v = JsonInt(s, "viewId", 0);
      g_cancelView.store(v);
      if (g_engine) g_engine->cancel();
      std::lock_guard<std::mutex> lk(g_jobMu);
      std::deque<Job> keep;
      for (auto& j : g_jobs) {
        /* A dropped field job still owns a reserved slot; release it here or
         * the pool slowly fills with slots nobody will ever ack. */
        if (j.viewId < v && j.field && j.fieldSlot >= 0) ReleaseFieldSeq(j.seq);
        if (j.viewId >= v) keep.push_back(j);
      }
      g_jobs.swap(keep);
      return S_OK;
    }

    if (s.find("\"pause\"") != std::string::npos) {
      bool on = JsonBool(s, "on", false);
      if (g_engine) g_engine->setPaused(on);
      LogMsg(std::string("[bus] pause=") + (on ? "1" : "0"));
      return S_OK;
    }

    if (s.find("\"render\"") != std::string::npos) {
      Job j;
      j.seq = (uint64_t)JsonNum(s, "seq", 0);
      j.viewId = JsonInt(s, "viewId", 0);
      j.x = JsonInt(s, "x", 0); j.y = JsonInt(s, "y", 0);
      j.w = JsonInt(s, "w", 1); j.h = JsonInt(s, "h", 1);
      j.viewW = JsonInt(s, "viewWidth", j.w);
      j.viewH = JsonInt(s, "viewHeight", j.h);
      j.centerRe = JsonStr(s, "centerRe", "-0.6");
      j.centerIm = JsonStr(s, "centerIm", "0");
      j.spanMant = JsonNum(s, "spanMant", 1.6);
      j.spanExp = JsonInt(s, "spanExp", 1);
      j.maxIter = JsonInt(s, "maxIter", 600);
      j.bits = JsonInt(s, "bits", 0);
      j.colorMode = JsonInt(s, "colorMode", 0);
      j.offset = JsonNum(s, "offset", 0.0);
      j.rotation = JsonNum(s, "rotation", 0.0);
      j.quality = JsonInt(s, "quality", 1);
      j.frame = JsonBool(s, "frame", false);
      j.dynamicIter = JsonBool(s, "dynamicIter", false);
      j.scheme = JsonInt(s, "scheme", 0);
      j.shade = JsonInt(s, "shade", 0);
      j.dither = JsonBool(s, "dither", false);
      j.pal = JsonInt(s, "pal", 0);
      j.bands = JsonInt(s, "bands", 0);
      /* The page names the palette phase `cycle`; older builds of the shell
       * read `ncycle`.  Accept both so neither side has to guess. */
      j.cycle = JsonNum(s, "ncycle", JsonNum(s, "cycle", 0.0));
      j.aa = JsonInt(s, "aa", 1);
      j.detail = JsonInt(s, "detail", 3);
      j.traps = JsonInt(s, "traps", 0);
      j.fieldDist = JsonBool(s, "fieldDist", true);
      j.fieldValue = JsonBool(s, "fieldValue", false);
      /* The desktop presents its own canvas, so a "field pass" is pure
       * overhead here: the shell would compute a float field, hand it to the
       * GL canvas, and shade it there -- three extra steps for a picture the
       * renderer can produce directly as RGBA.  The field exists so a browser
       * can shade with its own shader; this window is not a browser.
       *
       * MB_FIELD=1 keeps the old path alive for A/B measurements. */
      j.field = JsonBool(s, "fieldPass", false);
      if (std::getenv("MB_LOG_PARSE")) {
        HostLog("parse view=%d fieldPass=%d -> field=%d env=%s", j.viewId,
                (int)JsonBool(s, "fieldPass", false), (int)j.field,
                std::getenv("MB_FIELD") ? "set" : "unset");
      }
      if (j.aa < 1) j.aa = 1;
      if (j.aa > 4) j.aa = 4;
      JsonNumberArray(s, "colors", j.colors, 5 * 20);
      j.briFinite = (float)JsonNum(s, "briFinite", 1.0);
      j.briInfinite = (float)JsonNum(s, "briInfinite", 1.0);
      j.toneBri = (float)JsonNum(s, "bri", 1.0);
      j.toneCon = (float)JsonNum(s, "con", 1.0);
      j.toneGam = (float)JsonNum(s, "gam", 1.0);
      j.toneSat = (float)JsonNum(s, "sat", 1.0);
      j.toneR = (float)JsonNum(s, "gainR", 1.0);
      j.toneG = (float)JsonNum(s, "gainG", 1.0);
      j.toneB = (float)JsonNum(s, "gainB", 1.0);
      JsonStops(s, j.stops);
      j.mode = JsonInt(s, "mode", 0);
      j.kind = JsonInt(s, "kind", 0);
      j.expr = JsonStr(s, "expr", "");
      j.params = JsonStr(s, "params", "");
      j.power = JsonInt(s, "power", 2);
      // The page sends Julia's c as a decimal string so the deep orbit keeps
      // every digit the frame centre has.  The double is derived from it for
      // the shallow direct kernels; old numeric senders still work.
      j.juliaReS = JsonStr(s, "juliaRe", "");
      j.juliaImS = JsonStr(s, "juliaIm", "");
      j.juliaRe = j.juliaReS.empty() ? JsonNum(s, "juliaRe", 0.0)
                                     : std::strtod(j.juliaReS.c_str(), nullptr);
      j.juliaIm = j.juliaImS.empty() ? JsonNum(s, "juliaIm", 0.0)
                                     : std::strtod(j.juliaImS.c_str(), nullptr);
      j.phoenixP = JsonNum(s, "phoenixP", 0.5667);
      /* A field pass needs one whole-frame shared buffer.  It is reserved here,
       * on the UI thread that owns the WebView2 environment: creating it from
       * the render thread fails (0x802A000C), because the environment's COM
       * objects live in the UI thread's apartment.
       *
       * The size comes from the *request*, not from a global "current view":
       * preset thumbnails issue field jobs at 316x198 while the main view may
       * be 2384x1240, so a single global buffer cannot serve both. */
      if (j.field) {
        const int fw = j.w > 0 ? j.w : j.viewW;
        const int fh = j.h > 0 ? j.h : j.viewH;
        j.fieldSlot = EnsureField(fw, fh, j.seq, j.fieldValue ? 6 : 4);
        if (j.fieldSlot < 0) {
          HostLog("[field] no buffer for %dx%d, falling back to pixels", fw, fh);
          j.field = false;
        }
      }
      g_renderSeen++;
      if (!g_perfActive) {
        g_perfActive = true;
        g_perfWallStart = std::chrono::steady_clock::now();
      }
      if (g_renderSeen <= 3 || (g_renderSeen % 60) == 0) {
        LogMsg("[job] view=" + std::to_string(j.viewId) + " tile " + std::to_string(j.x) + "," +
               std::to_string(j.y) + " " + std::to_string(j.w) + "x" + std::to_string(j.h) +
               " view=" + std::to_string(j.viewW) + "x" + std::to_string(j.viewH) +
               " iter=" + std::to_string(j.maxIter) + " q=" + std::to_string(j.quality) +
               " dist=" + std::to_string((int)j.fieldDist) +
               " center=" + j.centerRe + "," + j.centerIm);
      }
      {
        std::lock_guard<std::mutex> lk(g_jobMu);
        // A newer view invalidates anything still queued for the old one.
        if (j.viewId < g_cancelView.load()) return S_OK;
        HostLog("job view=%d rect=%d,%d %dx%d view=%dx%d iter=%d scheme=%d pal=%d",
                j.viewId, j.x, j.y, j.w, j.h, j.viewW, j.viewH, j.maxIter, j.scheme, j.pal);
        g_jobs.push_back(std::move(j));
      }
      g_jobCv.notify_one();
      return S_OK;
    }

    return S_OK;
  }
};

// ---------------------------------------------------------------- render thread

static int AcquireSlot() {
  std::unique_lock<std::mutex> lk(g_slotMu);
  g_slotCv.wait(lk, [] {
    if (g_stopRender.load()) return true;
    for (int i = 0; i < kSlotCount; ++i) if (!g_slots[i].busy) return true;
    return false;
  });
  if (g_stopRender.load()) return -1;
  for (int i = 0; i < kSlotCount; ++i) {
    if (!g_slots[i].busy) { g_slots[i].busy = true; g_slots[i].seq = 0; return i; }
  }
  return -1;
}

/* Diagnostics: MB_LOG=1 appends everything the host does to <exe>.log.  The
 * WebView2 side is invisible from the outside, so this is the only way to see
 * whether a page load or a render actually happened. */
static void HostLog(const char* fmt, ...) {
  static FILE* f = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    if (std::getenv("MB_LOG")) {
      char path[MAX_PATH];
      if (GetModuleFileNameA(nullptr, path, MAX_PATH)) {
        char* dot = std::strrchr(path, '.');
        if (dot) std::strcpy(dot, ".log");
        f = std::fopen(path, "w");
      }
    }
  }
  if (!f) return;
  va_list ap;
  va_start(ap, fmt);
  std::vfprintf(f, fmt, ap);
  va_end(ap);
  std::fputc('\n', f);
  std::fflush(f);
}

/* A GUI process that dies inside a render thread leaves no trace, which is
 * exactly what made the "process vanished" report impossible to diagnose.
 * Record the exception code, faulting address and (for an AV) whether the
 * access was a read or a write, then let WER continue as usual. */
static LONG WINAPI HostCrashFilter(EXCEPTION_POINTERS* ep) {
  const auto* er = ep->ExceptionRecord;
  HostLog("[crash] code=0x%08lX addr=%p access=%s target=%p",
          (unsigned long)er->ExceptionCode, (void*)er->ExceptionAddress,
          (er->NumberParameters > 0 && er->ExceptionInformation[0]) ? "write" : "read",
          (er->NumberParameters > 1) ? (void*)er->ExceptionInformation[1] : nullptr);
  return EXCEPTION_CONTINUE_SEARCH;
}

/* Diagnostics: dump the finished frame as a 24-bit BMP.  Enabled with
 * MB_DUMP_FRAME=<dir>; the file is always <dir>\last.bmp (overwritten), which
 * is enough to inspect what the window actually shows without a screenshot
 * tool. */
static void WriteBmpRGBA(const char* path, const uint8_t* px, int w, int h, int pitchPx) {
  if (!path || !px || w <= 0 || h <= 0) return;
  FILE* f = fopen(path, "wb");
  if (!f) return;
  const int rowBytes = w * 3 + ((4 - (w * 3) % 4) % 4);
  const int fsize = 54 + rowBytes * h;
  uint8_t hdr[54] = {0};
  hdr[0] = 'B'; hdr[1] = 'M';
  std::memcpy(hdr + 2, &fsize, 4);
  int v = 54; std::memcpy(hdr + 10, &v, 4);
  v = 40; std::memcpy(hdr + 14, &v, 4);
  std::memcpy(hdr + 18, &w, 4);
  std::memcpy(hdr + 22, &h, 4);
  uint16_t one = 1; std::memcpy(hdr + 26, &one, 2);
  uint16_t bpp = 24; std::memcpy(hdr + 28, &bpp, 2);
  std::fwrite(hdr, 1, 54, f);
  std::vector<uint8_t> row((size_t)rowBytes, 0);
  for (int y = h - 1; y >= 0; --y) {
    const uint8_t* p = px + (size_t)y * (size_t)pitchPx * 4;
    for (int x = 0; x < w; ++x) {
      row[(size_t)x * 3 + 0] = p[(size_t)x * 4 + 2];
      row[(size_t)x * 3 + 1] = p[(size_t)x * 4 + 1];
      row[(size_t)x * 3 + 2] = p[(size_t)x * 4 + 0];
    }
    std::fwrite(row.data(), 1, (size_t)rowBytes, f);
  }
  std::fclose(f);
}

static void RenderThread() {
  while (!g_stopRender.load()) {
    Job j;
    {
      std::unique_lock<std::mutex> lk(g_jobMu);
      g_jobCv.wait(lk, [] { return g_stopRender.load() || !g_jobs.empty(); });
      if (g_stopRender.load()) return;
      j = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    if (j.viewId < g_cancelView.load()) {
      /* This job never ran, so no one will ack its field slot. */
      if (j.field && j.fieldSlot >= 0) ReleaseFieldSeq(j.seq);
      continue;
    }

    BYTE* outPtr = nullptr;
    int outPitch = 0;
    int slot = -1;
    bool useFrame = false;
    ICoreWebView2SharedBuffer* keep = nullptr;
    std::shared_ptr<std::vector<uint8_t>> keepCpu;
    {
      std::lock_guard<std::mutex> lk(g_frameMu);
      if (j.frame && g_frame.ptr && (g_nativeCanvas || g_frame.buf) &&
          g_frame.viewId == j.viewId && g_frame.w == j.viewW && g_frame.h == j.viewH) {
        useFrame = true;
        outPtr = g_frame.ptr + ((size_t)j.y * g_frame.w + j.x) * 4;
        outPitch = g_frame.w;
        // The page may replace g_frame while we render: keep the surface.
        if (g_frame.buf) { keep = g_frame.buf; keep->AddRef(); }
        keepCpu = g_frame.cpu;
      }
    }
    /* The shared slots are 1 MB, i.e. 512x512x4.  A job may cover the whole
     * frame (the desktop asks for one pass per view, not 512-px tiles), and a
     * frame-sized write into a 1 MB slot corrupts the heap -- that is the
     * 0xC0000005 access=write this change fixes.  So a rect that does not fit
     * a slot always claims the host pass instead of a slot. */
    const uint64_t rectBytes = (uint64_t)j.w * (uint64_t)j.h * 4;
    if (!useFrame && !j.field && (g_glCanvas || rectBytes > kSlotBytes) &&
        EnsureHostPass(j.viewW > 0 ? j.viewW : j.w, j.viewH > 0 ? j.viewH : j.h,
                       j.viewId)) {
      std::lock_guard<std::mutex> lk(g_frameMu);
      useFrame = true;
      outPtr = g_frame.ptr + ((size_t)j.y * g_frame.w + j.x) * 4;
      outPitch = g_frame.w;
      keepCpu = g_frame.cpu;
    }
    if (!useFrame && !j.field) {
      if (rectBytes > kSlotBytes) {
        HostLog("job view=%d rect=%d,%d %dx%d has no surface (%.1f MB > slot)",
                j.viewId, j.x, j.y, j.w, j.h, (double)rectBytes / 1048576.0);
        return;
      }
      slot = AcquireSlot();
      if (slot < 0) return;
      outPtr = g_slots[slot].ptr;
    }
    g_inFlight.store(true);

    mbc::Request req;
    req.centerRe = j.centerRe;
    req.centerIm = j.centerIm;
    req.spanMant = j.spanMant;
    req.spanExp = j.spanExp;
    req.width = j.w > 0 ? j.w : 1;
    req.height = j.h > 0 ? j.h : 1;
    req.viewWidth = j.viewW;
    req.viewHeight = j.viewH;
    req.tileX = j.x;
    req.tileY = j.y;
    req.maxIter = j.maxIter;
    req.colorMode = j.colorMode;
    req.offset = j.offset;
    req.rotation = j.rotation;
    req.bands = j.bands;
    req.cycle = j.cycle;
    req.bits = j.bits;
    req.quality = j.quality;
    /* The page can either render a pass at n times the resolution (and let the
     * canvas box-reduce it) or ask the core for supersampling here.  Both are
     * supported: `aa` from the page is the reference program's `samples`. */
    req.aa = j.aa > 0 ? j.aa : 1;
    req.detail = (j.detail < 0) ? 0 : (j.detail > 3 ? 3 : j.detail);
    req.traps = (j.traps < 0) ? 0 : (j.traps > 4 ? 4 : j.traps);
    req.fieldDist = j.fieldDist;
    req.fieldValue = j.fieldValue;
    req.outPitch = outPitch;    // 0 = tightly packed (legacy per-tile buffer)
    req.spec.mode = j.mode;
    req.spec.kind = j.kind;
    req.spec.power = j.power;
    req.spec.juliaRe = j.juliaRe;
    req.spec.juliaIm = j.juliaIm;
    req.spec.juliaReS = j.juliaReS;
    req.spec.juliaImS = j.juliaImS;
    /* The renderer owns the palette: the page only names it, or hands over a
     * full gradient.  One `pal()` table in the core is the single source of
     * truth for every client, and the tone stage lives in the same struct so a
     * colour edit never needs a second code path. */
    mbc::Palette pal = mbc::builtinPalette(j.pal);
    if (!j.colors.empty() && (j.colors.size() % 5) == 0) {
      /* Additive scheme: the same pal() the reference program runs, with the
       * exact rgb/frequency/initial triples out of examples/*.json. */
      pal.mode = 0;
      pal.n = 0;
      for (size_t i = 0; i + 4 < j.colors.size() && pal.n < 20; i += 5) {
        mbc::PaletteColor& c = pal.c[pal.n++];
        c.r = j.colors[i + 0];
        c.g = j.colors[i + 1];
        c.b = j.colors[i + 2];
        c.frequency = j.colors[i + 3];
        c.initial = j.colors[i + 4];
      }
      pal.brightnessFinite = j.briFinite;
      pal.brightnessInfinite = j.briInfinite;
    } else if (j.stops.size() >= 8 && (j.stops.size() % 4) == 0) {
      pal.mode = 1;
      pal.ns = 0;
      for (size_t i = 0; i + 3 < j.stops.size() && pal.ns < 32; i += 4) {
        mbc::GradientStop& g = pal.s[pal.ns++];
        g.pos = j.stops[i];
        g.r = j.stops[i + 1];
        g.g = j.stops[i + 2];
        g.b = j.stops[i + 3];
      }
      std::sort(pal.s, pal.s + pal.ns,
                [](const mbc::GradientStop& a, const mbc::GradientStop& b) {
                  return a.pos < b.pos;
                });
    }
    pal.brightness = j.toneBri;    pal.contrast = j.toneCon;
    pal.gamma = j.toneGam;
    pal.saturation = j.toneSat;
    pal.gainR = j.toneR;
    pal.gainG = j.toneG;
    pal.gainB = j.toneB;
    req.pal = &pal;
    if (std::getenv("MB_LOG") && (g_renderSeen <= 3 || (req.aa > 1 && (g_renderSeen % 40) == 0)))
      HostLog("palette mode=%d n=%d ns=%d colors=%d stops=%d bf=%.2f bi=%.2f aa=%d cyc=%.2f",
              pal.mode, pal.n, pal.ns, (int)j.colors.size(), (int)j.stops.size(),
              (double)pal.brightnessFinite, (double)pal.brightnessInfinite, req.aa,
              j.cycle);

    mbc::Stats st;
    auto t0 = std::chrono::steady_clock::now();
    bool fieldOk = true;
    bool gpuField = false;
    if (j.field) {
      /* The kernel computes the iteration field only; the page's shader turns
       * it into pixels.  This is the reference program's own split, and it is
       * what makes a colour change free: the field does not depend on it.
       *
       * Direct write: the kernel's `Field` adopts the reserved shared buffer,
       * so no staging vector is allocated, zero-filled or memcpy'd.  The slot
       * stays reserved until the page acks the post, so this memory cannot be
       * overwritten while the shader is reading it.
       *
       * MB_FIELD_COPY=1 restores the old staging+memcpy path.  It exists only
       * to A/B the two on the same build: identical pixels, measurable time. */
      FieldSlot fs{};
      {
        std::lock_guard<std::mutex> lk(g_fieldMu);
        if (j.fieldSlot >= 0 && j.fieldSlot < kFieldSlotCount)
          fs = g_fieldSlots[j.fieldSlot];
      }
      const int fch = j.fieldValue ? 6 : 4;
      const size_t need = (size_t)req.width * (size_t)req.height *
                          (size_t)fch;
      if (!fs.ptr || !fs.busy || fs.seq != j.seq ||
          fs.bytes < need * sizeof(float) ||
          fs.w != req.width || fs.h != req.height) {
        HostLog("[field] slot %d unusable (%dx%d %llu B, need %zu B)",
                j.fieldSlot, fs.w, fs.h, (unsigned long long)fs.bytes,
                need * sizeof(float));
        fieldOk = false;
      } else {
        /* Where the field comes from: OpenCL when the frame is heavy enough to
         * pay for an fp64 kernel, the CPU core otherwise.  Either way the same
         * 4-channel field lands in the slot and in the GL canvas's field
         * texture, and the canvas colours it. */
        bool onGpu = false;
        const bool wantGlField = g_glCanvas;
        if (wantGlField) {
          const int fw = req.width, fh = req.height;
          if (g_glFieldW != fw || g_glFieldH != fh) {
            gpur::beginPass(fw, fh, 1, fw, fh);
            g_glFieldW = fw;
            g_glFieldH = fh;
          }
          g_glFieldView = j.viewId;
          /* Every landed frame is the real picture, so the gesture preview
           * that stood in for it must be dropped.  Leaving it in place made
           * the wheel transform compose pass after pass — the frame kept
           * shrinking until the fractal sat in the middle as a bright ring. */
          gpur::resetPreview();
          const double ipc = req.cycle > 0.0
                                 ? req.cycle
                                 : (pal.cycleRef > 0.0f ? (double)pal.cycleRef : 1.0);
          gpur::setPalette(pal, req.offset, ipc, req.colorMode,
                           req.aa > 1 ? req.aa : 1);
        }
        /* Measured on this machine (Radeon 780M, fp64 at 1/16 rate, 6 CUs vs
         * 15 AVX2 threads): the kernel is ~2x slower while a frame averages a
         * few iterations per pixel, and ~1.3x faster once the escape budget is
         * in the thousands.  So the GPU takes the field only for heavy frames;
         * shallow ones stay on the CPU, which is both faster and the definition
         * of the picture.  MB_GPU=1 forces it, MB_GPU=0 disables it. */
        /* MB_GPU=1 forces the GPU field, MB_GPU=0 disables it; with the
         * variable unset the measured heuristic below decides.  The comment
         * promised this switch for a long time without a getenv() behind it. */
        const char* gpuEnv = std::getenv("MB_GPU");
        const int gpuPolicy = gpuEnv ? (std::atoi(gpuEnv) != 0 ? 1 : -1) : 0;
        const bool heavyEnough = req.maxIter >= kGpuMinIter && fch == 4;
        if (g_gpuField && wantGlField && gpuPolicy != -1 &&
            (gpuPolicy == 1 || heavyEnough)) {
          mbc::Stats gst;
          if (gpur::renderFieldTile(req, 0.0, gst)) {
            st = gst;
            onGpu = true;
            /* The kernel always writes four channels.  Copy only what it
             * actually produced: a value-plane request asks for six, and
             * copying `need` floats out of a four-channel buffer reads past
             * the end of the heap block. */
            const float* gd = gpur::fieldData();
            const size_t have = gpur::fieldFloats();
            if (gd) std::memcpy(fs.ptr, gd, std::min(need, have) * sizeof(float));
          }
        }
        if (!onGpu) {
          /* B4: the rebuilt core fills this field.
           *
           * The shell's contract here is four interleaved floats per texel
           * (smooth iteration, distance, integer iteration, linear distance);
           * the page's shader turns those into pixels, so a colour edit never
           * costs another render.  The rebuilt core produces exactly this via
           * render_image(field4).  Only the six-channel value scheme still
           * falls back: the 20-step continuation it needs is not transcribed.
           */
          bool coreDrew = false;
          if (std::getenv("MB_NOMSET") == nullptr && fch == 4 &&
              j.colors.empty() && j.traps == 0 && j.pal >= 0 &&
              msetb::Supports(j.mode, j.kind, j.power)) {
            msetb::Result mr;
            coreDrew = msetb::RenderField(
                j.centerRe.c_str(), j.centerIm.c_str(), j.spanMant, j.spanExp,
                req.width, req.height, req.maxIter, j.mode, j.kind, j.power,
                j.juliaReS.empty() ? nullptr : j.juliaReS.c_str(),
                j.juliaImS.empty() ? nullptr : j.juliaImS.c_str(),
                j.colorMode, j.rotation, fch, reinterpret_cast<float*>(fs.ptr),
                &g_stopRender, &mr,
                j.expr.empty() ? nullptr : j.expr.c_str(),
                j.params.empty() ? nullptr : j.params.c_str());
            if (coreDrew) {
              st.bits = mr.digits;
              st.perturb = true;
              st.refLen = mr.refLen;
              st.glitches = mr.glitches;
              st.renderMs = mr.ms;
              st.escapeMin = mr.minEscape;
              st.escapeMax = mr.maxEscape;
              st.bounded = mr.bounded;
              /* The shader needs the same cycle the core fitted, or a deep
               * frame (whose entire escape range is a hundred iterations)
               * paints a single flat band. */
              st.iterPerCycle = mr.iterPerCycle;
              st.maxIterUsed = req.maxIter;
              static int coreLogs = 0;
              if (coreLogs < 40) {
                static int lastCoreView = -1;
                if (lastCoreView != j.viewId) {
                  lastCoreView = j.viewId;
                  ++coreLogs;
                  char line[320];
                  std::snprintf(line, sizeof line,
                                "[core] view=%d %dx%d ms=%.1f digits=%d ref=%d "
                                "esc=%d..%d bounded=%d band=%.3f rot=%.1f centre=%s",
                                j.viewId, req.width, req.height, mr.ms, mr.digits,
                                mr.refLen, mr.minEscape, mr.maxEscape, mr.bounded,
                                mr.iterPerCycle, j.rotation * 57.29577951308232,
                                j.centerRe.substr(0, 24).c_str());
                  LogMsg(line);
                }
              }
            } else {
              static int coreFails = 0;
              if (coreFails++ < 8) HostLog("core field fallback: %s", mr.error);
            }
          }
          if (!coreDrew) {
            mbc::Field f;
            if (std::getenv("MB_FIELD_COPY")) {
              f.resize(req.width, req.height, fch);
              g_engine->renderField(req, f, st);
              std::memcpy(fs.ptr, f.base(), need * sizeof(float));
            } else {
              f.adopt(req.width, req.height, fs.ptr, fs.bytes / sizeof(float), fch);
              g_engine->renderField(req, f, st);
            }
          }
        } else {
          gpuField = true;
        }
        if (wantGlField) {
          /* Band fit.  A frame's visible iteration range can be a handful of
           * steps while the palette's natural cycle is hundreds, which paints
           * the whole thing one flat colour.  The core measures the frame's own
           * range and reports the fitted cycle in `iterPerCycle`; the shader
           * has to use that same number or the picture is a black card with a
           * bright rim (the fit only ever raises the density, so a shallow view
           * keeps its authored look). */
          if (st.iterPerCycle > 0.0)
            gpur::setPalette(pal, req.offset, st.iterPerCycle, req.colorMode,
                             req.aa > 1 ? req.aa : 1);
          /* One shade per pass, from the field the shell just computed.  The
           * window shows the OpenGL canvas, so this is the picture; the page's
           * own WebGL copy is only kept so a machine that cannot present
           * natively still has one. */
          if (canvas::UploadField(fs.ptr, req.width, req.height)) {
            canvas::Commit(false, 1.0, 0.0, 1.0, 0.0);
            if (j.quality >= 2) {
              /* Keep the last final pass for PNG export.  The pixels live in
               * the GL canvas, so the export reads them back once here rather
               * than paying for it on every preview pass. */
              std::lock_guard<std::mutex> lk2(g_lastMu);
              g_lastFinal.assign((size_t)req.width * req.height * 4, 0);
              if (!gpur::readback(g_lastFinal.data(), req.width, req.height))
                g_lastFinal.clear();
              g_lastW = req.width;
              g_lastH = req.height;
              g_lastSS = 1;
            }
            if (g_glFieldPosts++ == 0) {
              char flush[160];
              std::snprintf(flush, sizeof(flush),
                            "{\"type\":\"nativeflush\",\"viewId\":%d,\"w\":%d,"
                            "\"h\":%d,\"ss\":1,\"ok\":1}",
                            j.viewId, req.width, req.height);
              PostJson(flush);
            }
          }
        }
        HostLog("field view=%d slot=%d %dx%d ch=%d engine=%s iter=%d bounded=%d esc=[%d,%d] ipc=%.3f off=%.3f cref=%.2f pal=%d/%d",
                j.viewId, j.fieldSlot, req.width, req.height, fch,
                onGpu ? "opencl" : "cpu", st.maxIterUsed, st.bounded,
                st.escapeMin, st.escapeMax, st.iterPerCycle, req.offset,
                (double)pal.cycleRef, pal.mode, pal.ns);
        /* MB_FIELD_DUMP=prefix writes the exact bytes the page will read, one
         * file per pass.  It is the deterministic A/B for MB_FIELD_COPY: a
         * screenshot can be occluded by another window, a field dump cannot. */
        if (const char* dump = std::getenv("MB_FIELD_DUMP")) {
          char path[MAX_PATH];
          std::snprintf(path, sizeof(path), "%s_%llu_%dx%d_ch%d.bin", dump,
                        (unsigned long long)j.seq, req.width, req.height, fch);
          if (FILE* fp = std::fopen(path, "wb")) {
            std::fwrite(fs.ptr, 1, need * sizeof(float), fp);
            std::fclose(fp);
          }
        }
      }
    } else {
      /* OpenCL path.  The kernel writes the same field the CPU would, the
       * GL canvas shades it, and `outPtr` is never touched — the dispatcher
       * skips its RGBA upload when `d.gpuField` is set.  Any tile the kernel
       * declines (deep zoom, another formula, an orbit trap) falls through to
       * the CPU engine, so a frame is never left half-painted. */
      bool onGpu = false;
      if (g_gpuField) {
        // The engine owns the pixel scale; recomputing it here is what puts a
        // frame one pixel off at depth.
        const double ps = mbc::pixelScale(req);
        if (req.pal) {
          const double ipc = req.cycle > 0.0
                                 ? req.cycle
                                 : (req.pal->cycleRef > 0.0f ? (double)req.pal->cycleRef : 1.0);
          gpur::setPalette(*req.pal, req.offset, ipc, req.colorMode,
                           req.aa > 1 ? req.aa : 1);
        }
        onGpu = gpur::renderFieldTile(req, ps, st);
        if (onGpu)
          HostLog("gpu view=%d rect=%d,%d %dx%d kernel=%.1fms iter=%d bounded=%d esc=[%d,%d]",
                  j.viewId, j.x, j.y, j.w, j.h, st.renderMs, st.maxIterUsed,
                  st.bounded, st.escapeMin, st.escapeMax);
      }
      if (!onGpu) {
        /* Backend swap: the pixels come from the reconstructed core
         * (native/mset) whenever it can describe the request.
         *
         * The page's gradient no longer forces a fallback: the core hands back
         * a colour coordinate and the ramp is applied here, so an edited
         * scheme re-shades instead of re-rendering.  Rotation no longer forces
         * one either -- it lives in the core's pixel->plane map.
         *
         * Still refused, because the core cannot express them yet: the folded
         * families (kind != 0), the additive example palettes (`colors`) and
         * orbit traps.  Those keep the old engine rather than paint something
         * that looks plausible and is wrong. */
        msetb::Gradient grad;
        if (j.stops.size() >= 8 && (j.stops.size() % 4) == 0) {
          const int n = std::min<int>(32, (int)(j.stops.size() / 4));
          for (int i = 0; i < n; ++i) {
            grad.pos[i] = j.stops[(size_t)i * 4 + 0];
            grad.rgb[i][0] = j.stops[(size_t)i * 4 + 1];
            grad.rgb[i][1] = j.stops[(size_t)i * 4 + 2];
            grad.rgb[i][2] = j.stops[(size_t)i * 4 + 3];
          }
          grad.count = n;
          grad.stops = j.stops.data();
        }
        /* The reconstructed core pays for a high-precision reference orbit and
         * its own full-core thread pool on every tile.  At shallow/medium depth
         * that is pure overhead: the stitched engine draws the identical frame
         * 3-5x faster here (108 ms vs 594 ms on the 2724x1416 home view).
         *
         * It is also the engine that runs out first.  Its BigFloat is base
         * 1e9 with Karatsuba multiplication, so both the precision and the
         * speed of an orbit degrade as depth grows; measured at 2^-340 its
         * anchor escapes at iteration 110 while the true orbit of the same
         * point escapes at 279 (verified independently against 260-digit
         * decimal arithmetic), which paints the whole frame a single flat
         * colour and makes the page report "no structure here".
         *
         * So mset keeps the band where it is genuinely the faster engine and
         * the stitched core, which now multiplies through GMP (FFT, runtime
         * precision up to 1.2 Mbit), owns everything deeper.  2^-300 is
         * ~1e-90: comfortably inside what base-1e9 limbs resolve and well
         * before the difference between the two engines shows up. */
        /* 2026-11-14: mset is off by default.
         *
         * It was the third rendering implementation in the process (page
         * shader, stitched core, mset) and the only one whose arithmetic did
         * not come from the core.  Every mismatch it produced showed up as a
         * visible discontinuity: at 2^-340 its anchor escapes at iteration
         * 110 while the true orbit of the same point escapes at 279, so the
         * frame collapses to one colour right where the user is zooming.  The
         * numbers that once justified keeping it are gone too -- the core now
         * draws the 1280x800 home view in 14.3 ms against mset's 108 ms,
         * because the orbit goes through GMP and the tile scheduler is warm.
         *
         * `MB_FORCEMSET=1` still routes through it for A/B work. */
        /* 2026-11-14 B4: the rebuilt core is the renderer.
         *
         * The paragraphs below describe why the *old* reconstructed core was
         * demoted: its reference orbit escaped before the frame's pixels did,
         * so at 2^-340 the anchor died at iteration 110 against a true 279 and
         * the frame collapsed to one colour right where the user was zooming.
         * That failure is fixed at the source now -- the core promotes the
         * longest-lived point of the frame to be the reference until nothing
         * outlives it, and repairs cancellation against a local reference --
         * and 1e38 matches the BigFloat oracle on 98.2% of pixels with a mean
         * error of 0.11/255.
         *
         * So the gate is inverted: the rebuilt core draws, and the stitched
         * engine is the fallback for what the core cannot express yet (folded
         * families, additive `colors` schemes, orbit traps).  MB_NOMSET=1
         * still forces the old path for A/B work. */
        const bool deepEnough = std::getenv("MB_NOMSET") == nullptr;
        const bool bridged =
            deepEnough && j.colors.empty() && j.traps == 0 && j.pal >= 0 &&
            msetb::Supports(j.mode, j.kind, j.power);
        {
          static int gateLogs = 0;
          static int lastGate = -1;
          if (!bridged && gateLogs < 20 && lastGate != j.viewId) {
            lastGate = j.viewId;
            ++gateLogs;
            HostLog("mset gate: colors=%d stops=%d traps=%d pal=%d mode=%d kind=%d "
                    "power=%d aa=%d detail=%d",
                    (int)j.colors.size(), (int)j.stops.size(), j.traps, j.pal,
                    j.mode, j.kind, j.power, j.aa, j.detail);
          }
        }
        msetb::Result mr;
        bool drew = false;
        if (bridged) {
          drew = msetb::RenderTile(
              j.centerRe.c_str(), j.centerIm.c_str(), j.spanMant, j.spanExp,
              j.viewW, j.viewH, j.x, j.y, j.w, j.h, req.maxIter,
              j.mode, j.kind, j.power,
              j.juliaReS.empty() ? nullptr : j.juliaReS.c_str(),
              j.juliaImS.empty() ? nullptr : j.juliaImS.c_str(),
              j.pal, j.colorMode, false, j.rotation,
              grad.count ? &grad : nullptr, outPtr,
              req.outPitch > 0 ? req.outPitch : j.viewW, &g_stopRender, &mr,
              j.expr.empty() ? nullptr : j.expr.c_str(),
              j.params.empty() ? nullptr : j.params.c_str());
          if (drew) {
            st.bits = mr.digits;
            st.refLen = mr.refLen;
            st.renderMs = mr.ms;
            st.glitches = mr.glitches;
            st.perturb = true;
            st.maxIterUsed = req.maxIter;
            /* The recovered core reports the largest escape count it saw.  Pass
             * it on: without it the pass metadata carries escMax=0 and the page
             * reads that as "nothing escapes here", i.e. the "no structure"
             * banner on every deep frame.  0 means "did not report", so it is
             * only forwarded when the core actually saw an escape. */
            if (mr.maxEscape > 0) st.escapeMax = mr.maxEscape;
            /* One line per distinct view is enough to tell from the log which
             * producer drew it: the reconstructed core reports the digits and
             * reference length it actually used. */
            static int msetLogs = 0;
            static int lastView = -1;
            if ((msetLogs < 40 || std::getenv("MB_LOG_MSET")) && lastView != j.viewId) {
              lastView = j.viewId;
              ++msetLogs;
              HostLog("mset view=%d rect=%d,%d %dx%d core=%.1fms digits=%d ref=%d "
                      "glitch=%d rot=%.1f grad=%d",
                      j.viewId, j.x, j.y, j.w, j.h, mr.ms, mr.digits, mr.refLen,
                      mr.glitches, j.rotation * 57.29577951308232, grad.count);
            }
          } else {
            static int logged = 0;
            if (logged++ < 8) HostLog("mset fallback: %s", mr.error);
          }
        }
        if (!drew) g_engine->render(req, outPtr, st);
      }
      gpuField = onGpu;
    }
    /* `st.renderMs` is measured inside the kernel; it cannot see the host's
     * field publication.  The wall time does, and is what an A/B of
     * MB_FIELD_COPY has to compare. */
    const double wallMs =
        std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
    HostLog("done view=%d rect=%d,%d %dx%d render=%.1fms wall=%.1fms orbit=%.1fms iter=%d ref=%d pert=%d bits=%d ps=%.2f span=%g*2^%d",
            j.viewId, j.x, j.y, j.w, j.h, st.renderMs, wallMs, st.orbitMs,
            st.maxIterUsed, st.refLen, (int)st.perturb, st.bits, st.psLog2,
            j.spanMant, j.spanExp);
    if (!j.field && outPtr && std::getenv("MB_DUMP_FRAME") &&
        j.w == j.viewW && j.h == j.viewH) {
      char path[MAX_PATH];
      if (GetModuleFileNameA(nullptr, path, MAX_PATH)) {
        char* dot = std::strrchr(path, '.');
        if (dot) std::strcpy(dot, "_frame.bmp");
        WriteBmpRGBA(path, outPtr, j.w, j.h, outPitch > 0 ? outPitch : j.w);
        HostLog("dump %s %dx%d", path, j.w, j.h);
      }
    }
    auto t1 = std::chrono::steady_clock::now();
    if (keep) keep->Release();
    keepCpu.reset();

    Done d;
    d.seq = j.seq; d.slot = slot; d.frame = useFrame; d.viewId = j.viewId;
    d.x = j.x; d.y = j.y; d.w = j.w; d.h = j.h;
    d.gpuField = gpuField;
    d.bits = st.bits; d.refLen = st.refLen; d.glitches = st.glitches;
    d.maxIter = st.maxIterUsed;
    d.cap = req.maxIter;
    d.escMin = st.escapeMin; d.escMax = st.escapeMax;
    d.bounded = st.bounded; d.directFixed = st.directFixed;
    d.iterPerCycle = st.iterPerCycle;
    d.iterPixels = st.iterPixels;
    d.canvasVerts = st.canvasVerts;
    d.canvasFilled = st.canvasFilled;
    d.canvasCacheHit = st.canvasCacheHit;
    d.threads = st.threads;
    d.dd = false; d.perturb = st.perturb; d.unsupported = !fieldOk;
    d.field = j.field;
    d.fieldSlot = j.fieldSlot;
    d.fieldChannels = j.fieldValue ? 6 : 4;
    d.orbitMs = st.orbitMs;
    d.orbitReused = st.orbitReused;
    d.renderMs = st.renderMs;
    if (d.renderMs <= 0)
      d.renderMs = std::chrono::duration<double, std::milli>(t1 - t0).count();

    {
      std::lock_guard<std::mutex> lk(g_perfMu);
      if (g_perfView != j.viewId) {
        g_perfView = j.viewId; g_perfMs = 0; g_perfPx = 0; g_perfTiles = 0;
      }
      g_perfMs += d.renderMs;
      g_perfPx += (uint64_t)d.w * (uint64_t)d.h;
      g_perfTiles++;
    }
    if (slot >= 0) {
      std::lock_guard<std::mutex> lk(g_slotMu);
      g_slots[slot].seq = j.seq;
    }
    {
      std::lock_guard<std::mutex> lk(g_doneMu);
      g_done.push_back(std::move(d));
    }
    {
      std::lock_guard<std::mutex> lk(g_jobMu);
      if (g_jobs.empty()) g_inFlight.store(false);
    }
    PostMessageW(g_hwnd, WM_APP_DONE, 0, 0);
  }
}

// ---------------------------------------------------------------- publish tiles

static void PublishDone() {
  std::deque<Done> batch;
  {
    std::lock_guard<std::mutex> lk(g_doneMu);
    batch.swap(g_done);
  }
  if (batch.empty() || !g_wv17) return;
  for (auto& d : batch) {
    if (d.viewId < g_cancelView.load()) {
      if (d.slot >= 0) {
        std::lock_guard<std::mutex> lk(g_slotMu);
        g_slots[d.slot].busy = false;
        g_slotCv.notify_all();
      }
      /* A cancelled field is never posted, so the page will never ack it. */
      if (d.field) ReleaseFieldSeq(d.seq);
      continue;
    }
    /* What the pixel loop actually did, not what family it ran.  The old
     * string was "julia-pert" for every perturbed frame -- it was a leftover
     * from when only the Julia kernel perturbed, and it made every Mandelbrot
     * pass report itself as a Julia one in the meta the page displays. */
    const char* mode = d.dd ? "dd" : (d.perturb ? "pert" : "direct");
    if (d.field) {
      /* Publish the slot this job wrote, not a global "current" buffer: the
       * pool may hold a small thumbnail and a full-size frame at once. */
      if (d.unsupported || d.fieldSlot < 0 || d.fieldSlot >= kFieldSlotCount) {
        HostLog("[field] drop view=%d slot=%d unsupported=%d", d.viewId,
                d.fieldSlot, (int)d.unsupported);
        ReleaseFieldSeq(d.seq);
        continue;
      }
      char meta[768];
      snprintf(meta, sizeof(meta),
               "{\"type\":\"field\",\"seq\":%llu,\"viewId\":%d,\"w\":%d,\"h\":%d,"
               "\"ch\":%d,"
               "\"meta\":{\"engine\":\"NATIVE\",\"mode\":\"%s\","
               "\"ms\":%.2f,\"orbitMs\":%.2f,\"refLen\":%d,\"glitches\":%d,"
               "\"orbitReused\":%s,"
               "\"iter\":%d,\"cap\":%d,\"bits\":%d,\"perturb\":%s,\"threads\":%d,"
               "\"escMin\":%d,\"escMax\":%d,\"bounded\":%d,\"directFixed\":%d,"
               "\"ipc\":%.4f}}",
               (unsigned long long)d.seq, d.viewId, d.w, d.h, d.fieldChannels,
               mode,
               d.renderMs, d.orbitMs, d.refLen, d.glitches, d.orbitReused ? "true" : "false",
               d.maxIter, d.cap, d.bits, d.perturb ? "true" : "false", d.threads,
               d.escMin, d.escMax, d.bounded, d.directFixed, d.iterPerCycle);
      std::wstring wmeta(meta, meta + strlen(meta));
      HRESULT hr = E_FAIL;
      {
        std::lock_guard<std::mutex> lk(g_fieldMu);
        FieldSlot& s = g_fieldSlots[d.fieldSlot];
        if (s.buf && s.busy && s.seq == d.seq) {
          hr = g_wv17->PostSharedBufferToScript(
              s.buf, COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY,
              wmeta.c_str());
          if (SUCCEEDED(hr)) {
            s.posted = true;
            s.since = std::chrono::steady_clock::now();
          }
        }
      }
      if (FAILED(hr)) {
        LogMsg("[field] PostSharedBufferToScript failed hr=" +
               std::to_string((int)hr));
        /* No post means no ack; do not strand the slot.  The page reports an
         * unsupported field instead of painting a stale texture. */
        ReleaseFieldSeq(d.seq);
      }
      continue;
    }
    if (d.frame) {
      if (g_nativeCanvas && !d.gpuField) {
        // Feed the finished tile straight into the staging texture; the
        // visible image only flips when the whole pass has landed.
        // A GPU tile already wrote its own rectangle of the field texture,
        // so uploading the (never written) CPU buffer would erase it.
        std::lock_guard<std::mutex> lk(g_frameMu);
        if (g_frame.ptr && g_frame.viewId == d.viewId && g_frame.w >= d.x + d.w &&
            g_frame.h >= d.y + d.h) {
          canvas::Upload(g_frame.ptr, g_frame.w, g_frame.h, d.x, d.y, d.w, d.h);
        }
      }
      if (d.frame && g_glCanvas) {
        /* One present per landed tile: the GL field accumulates in the canvas
         * texture, so the picture fills in progressively.
         *
         * Not on a composition swapchain.  There a Present waits for the
         * compositor -- 13.5 ms measured on this machine -- so 18 tiles cost 18
         * vblanks (225 ms wall for 49 ms of work) and the pass lands *later*
         * than a single commit.  The staging texture already accumulates every
         * tile; one CommitPass at the end of the pass is both faster and
         * tear-free. */
        canvas::Commit(false, 1.0, 0.0, 1.0, 0.0);
      }
      {
        static int dispLogs = 0;
        if (dispLogs < 6) {
          ++dispLogs;
          HostLog("dispatch view=%d frame=%d native=%d gl=%d gpuField=%d",
                  d.viewId, (int)d.frame, (int)g_nativeCanvas, (int)g_glCanvas,
                  (int)d.gpuField);
        }
      }
      if (g_nativeCanvas && d.frame) {
        /* Tell the page the window is showing the native surface, once per
         * view.  It only makes its own canvas transparent after this message,
         * so without it the page's (unpainted) canvas covers the picture --
         * which is exactly what a blank window with a live status bar looks
         * like.  The field path used to send this from the upload site; the
         * RGBA path has to send it from here. */
        static int lastFlushView = -1;
        if (lastFlushView != d.viewId) {
          lastFlushView = d.viewId;
          char flush[192];
          snprintf(flush, sizeof(flush),
                   "{\"type\":\"nativeflush\",\"viewId\":%d,\"w\":%d,"
                   "\"h\":%d,\"ss\":1,\"ok\":1}",
                   d.viewId, d.w, d.h);
          PostJson(flush);
          /* The page only reveals the native canvas when its own message
           * handler decides a frame landed on it -- and that decision needs a
           * pass it declared itself, which the desktop no longer declares.
           * Do it from the side that actually knows: this window owns the
           * canvas, so it tells its own UI layer to go transparent once. */
          static bool surfaceShown = false;
          ICoreWebView2* wv = WebView();
          if (!surfaceShown && wv) {
            surfaceShown = true;
            wv->ExecuteScript(
                L"document.documentElement.classList.add('native-surface')",
                nullptr);
            LogMsg("[native] page set transparent over the canvas");
          }
        }
      }
      // Pixels already landed in the frame buffer; this is only the progress
      // notification, so it travels as a plain WebView2 message.
      char meta[1024];
      snprintf(meta, sizeof(meta),
               "{\"type\":\"tilemeta\",\"seq\":%llu,\"viewId\":%d,\"x\":%d,\"y\":%d,"
               "\"bw\":%d,\"bh\":%d,\"meta\":{\"engine\":\"NATIVE\",\"mode\":\"%s\","
               "\"ms\":%.2f,\"orbitMs\":%.2f,\"refLen\":%d,\"glitches\":%d,"
               "\"orbitReused\":%s,"
               "\"iter\":%d,\"cap\":%d,\"bits\":%d,\"dd\":%s,\"perturb\":%s,\"threads\":%d,"
               "\"escMin\":%d,\"escMax\":%d,\"bounded\":%d,\"directFixed\":%d,\"cyc\":%.3f,"
               "\"iterPx\":%lld,\"cvVerts\":%d,\"cvPx\":%d,\"cvHit\":%d}}",
               (unsigned long long)d.seq, d.viewId, d.x, d.y, d.w, d.h, mode,
               d.renderMs, d.orbitMs, d.refLen, d.glitches, d.orbitReused ? "true" : "false",
               d.maxIter, d.cap, d.bits, d.dd ? "true" : "false", d.perturb ? "true" : "false", d.threads,
               d.escMin, d.escMax, d.bounded, d.directFixed, d.iterPerCycle,
               d.iterPixels, d.canvasVerts, d.canvasFilled, d.canvasCacheHit);
      PostJson(meta);
      continue;
    }
    char meta[1024];
    snprintf(meta, sizeof(meta),
             "{\"type\":\"tile\",\"seq\":%llu,\"viewId\":%d,\"x\":%d,\"y\":%d,"
             "\"bw\":%d,\"bh\":%d,\"meta\":{\"engine\":\"NATIVE\",\"mode\":\"%s\","
             "\"ms\":%.2f,\"orbitMs\":%.2f,\"refLen\":%d,\"glitches\":%d,"
               "\"orbitReused\":%s,"
             "\"iter\":%d,\"cap\":%d,\"bits\":%d,\"dd\":%s,\"perturb\":%s,\"threads\":%d,"
             "\"escMin\":%d,\"escMax\":%d,\"bounded\":%d,\"directFixed\":%d,\"cyc\":%.3f,"
             "\"iterPx\":%lld,\"cvVerts\":%d,\"cvPx\":%d,\"cvHit\":%d}}",
             (unsigned long long)d.seq, d.viewId, d.x, d.y, d.w, d.h, mode,
             d.renderMs, d.orbitMs, d.refLen, d.glitches, d.orbitReused ? "true" : "false",
             d.maxIter, d.cap, d.bits, d.dd ? "true" : "false", d.perturb ? "true" : "false", d.threads,
             d.escMin, d.escMax, d.bounded, d.directFixed, d.iterPerCycle,
             d.iterPixels, d.canvasVerts, d.canvasFilled, d.canvasCacheHit);
    std::wstring wmeta(meta, meta + strlen(meta));
    HRESULT hr = g_wv17->PostSharedBufferToScript(
        g_slots[d.slot].buf, COREWEBVIEW2_SHARED_BUFFER_ACCESS_READ_ONLY, wmeta.c_str());
    if (g_doneSeen < 4) {
      LogMsg("[done] seq=" + std::to_string(d.seq) + " " + std::to_string(d.w) + "x" +
             std::to_string(d.h) + " " + std::to_string(d.renderMs) + "ms orbit=" +
             std::to_string(d.orbitMs) + " iter=" + std::to_string(d.maxIter) +
             " bits=" + std::to_string(d.bits) + " hr=" + std::to_string((int)hr));
      g_doneSeen++;
    }
    if (FAILED(hr)) {
      LogMsg("[shm] PostSharedBufferToScript failed hr=" + std::to_string((int)hr));
      std::lock_guard<std::mutex> lk(g_slotMu);
      g_slots[d.slot].busy = false;
      g_slotCv.notify_all();
    }
  }

  // One line per drained pass: measured CPU time, not an estimate.
  if (!g_inFlight.load()) {
    std::lock_guard<std::mutex> lk(g_perfMu);
    bool empty = false;
    {
      std::lock_guard<std::mutex> jl(g_jobMu);
      empty = g_jobs.empty();
    }
    if (empty && g_perfTiles > 0) {
      double mpix = g_perfPx / 1e6;
      double sec = g_perfMs / 1000.0;
      double wallMs = std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - g_perfWallStart).count();
      char line[260];
      snprintf(line, sizeof(line),
               "[perf] view=%d tiles=%d px=%.2fMpx cpu=%.1fms wall=%.1fms "
               "cpu-throughput=%.2fMpix/s wall-throughput=%.2fMpix/s",
               g_perfView, g_perfTiles, mpix, g_perfMs, wallMs,
               sec > 0 ? mpix / sec : 0.0, wallMs > 0 ? mpix / (wallMs / 1000.0) : 0.0);
      LogMsg(line);
      g_perfTiles = 0; g_perfPx = 0; g_perfMs = 0;
      g_perfActive = false;
    }
  }
}

// ---------------------------------------------------------------- main window

/* ---------------------------------------------------------------- visual hosting input
 *
 * In visual hosting the page has no window of its own, so Windows never sends
 * it a mouse message: the host has to relay them.  This is the documented
 * contract of ICoreWebView2CompositionController::SendMouseInput, and getting
 * it wrong is invisible until someone tries to drag the picture — the page
 * simply never hears about the mouse.
 *
 * The relaying is deliberately total: the whole client area belongs to the
 * page (the native canvas is a visual underneath, not a control), so every
 * mouse message is forwarded with the same coordinates the page would have
 * received had it owned the window.  Capture is taken on the first button so a
 * drag that leaves the window keeps arriving, and LEAVE is synthesised so
 * hover popups close.
 */
static bool g_trackingLeave = false;
static bool g_mouseCapture = false;

static UINT16 WheelDelta(WPARAM wp) { return (UINT16)HIWORD(wp); }
static UINT16 XButton(WPARAM wp) { return (UINT16)HIWORD(wp); }
static COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS KeyState(WPARAM wp) {
  return (COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS)(wp & 0xFFFFu);
}

static bool IsMouseMessage(UINT msg) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
      return true;
    default:
      return false;
  }
}

/** Relay one mouse message to the page.  True = consumed. */
static bool RelayMouse(UINT msg, WPARAM wp, LPARAM lp) {
  if (!g_compMode || !g_wvc || !g_ctl) return false;

  POINT pt;
  pt.x = (LONG)(short)LOWORD(lp);
  pt.y = (LONG)(short)HIWORD(lp);
  /* Wheel and non-client variants carry screen coordinates; everything else is
   * already client-relative.  The WebView2 bounds start at (0,0) here, so the
   * client point is the page point. */
  if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) ScreenToClient(g_hwnd, &pt);

  if (msg == WM_MOUSEMOVE && !g_trackingLeave) {
    TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, g_hwnd, 0};
    if (TrackMouseEvent(&tme)) g_trackingLeave = true;
  }

  UINT32 data = 0;
  if (msg == WM_MOUSEWHEEL || msg == WM_MOUSEHWHEEL) data = WheelDelta(wp);
  else if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONUP || msg == WM_XBUTTONDBLCLK)
    data = XButton(wp);

  if (msg == WM_LBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_RBUTTONDOWN ||
      msg == WM_XBUTTONDOWN) {
    if (GetCapture() != g_hwnd) {
      SetCapture(g_hwnd);
      g_mouseCapture = true;
    }
    /* Keyboard follows the click: MoveFocus hands DOM focus (and with it the
     * keystroke stream) to the WebView2, exactly as a real click would. */
    g_ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
  } else if (msg == WM_LBUTTONUP || msg == WM_MBUTTONUP || msg == WM_RBUTTONUP ||
             msg == WM_XBUTTONUP) {
    if (g_mouseCapture && GetCapture() == g_hwnd) {
      ReleaseCapture();
      g_mouseCapture = false;
    }
  }

  g_wvc->SendMouseInput((COREWEBVIEW2_MOUSE_EVENT_KIND)msg, KeyState(wp), data, pt);
  return true;
}

/** The page owns the cursor inside the client area (text I-beam, grab, ...). */
static bool RelayCursor(LPARAM lp) {
  if (!g_compMode || !g_wvc) return false;
  if ((LOWORD(lp)) != HTCLIENT) return false;
  HCURSOR cur = nullptr;
  if (SUCCEEDED(g_wvc->get_Cursor(&cur)) && cur) {
    SetCursor(cur);
    return true;
  }
  return false;
}

static void ResizeWebView() {
  if (!g_ctl || !g_hwnd) return;
  RECT rc;
  GetClientRect(g_hwnd, &rc);
  g_ctl->put_Bounds(rc);
  /* Visual hosting draws at RasterizationScale, but the *bounds* stay in
   * window pixels: a 150 % monitor then gets a crisp page instead of a
   * upscaled one. */
  ICoreWebView2Controller3* c3 = nullptr;
  if (SUCCEEDED(g_ctl->QueryInterface(__uuidof(ICoreWebView2Controller3), (void**)&c3)) && c3) {
    const UINT dpi = GetDpiForWindow(g_hwnd);
    c3->put_ShouldDetectMonitorScaleChanges(FALSE);
    c3->put_RasterizationScale(dpi ? (double)dpi / 96.0 : 1.0);
    c3->Release();
  }
  canvas::CommitComposition();
}

/* ---------------------------------------------------------------- fullscreen
 *
 * WebView2 does not implement the DOM Fullscreen API (requestFullscreen is a
 * no-op), so the page asks the shell instead: {"type":"cmd","cmd":"fullscreen"}.
 * The window drops its frame and covers the monitor; a second call restores the
 * saved style and placement exactly.  F11 is handled in the page and lands here
 * through the same message, so both entry points stay in sync.
 */
static bool g_fullscreen = false;
static WINDOWPLACEMENT g_fsPlacement{};
static LONG_PTR g_fsStyle = 0;

static void ToggleFullscreen(HWND hwnd) {
  if (!hwnd) return;
  if (!g_fullscreen) {
    g_fsPlacement.length = sizeof(WINDOWPLACEMENT);
    if (!GetWindowPlacement(hwnd, &g_fsPlacement)) return;
    g_fsStyle = GetWindowLongPtrW(hwnd, GWL_STYLE);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) return;
    SetWindowLongPtrW(hwnd, GWL_STYLE,
                      (g_fsStyle & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX |
                                     WS_MAXIMIZEBOX | WS_SYSMENU)) | WS_POPUP);
    SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                 mi.rcMonitor.right - mi.rcMonitor.left,
                 mi.rcMonitor.bottom - mi.rcMonitor.top,
                 SWP_FRAMECHANGED | SWP_NOZORDER | SWP_NOACTIVATE);
    g_fullscreen = true;
  } else {
    SetWindowLongPtrW(hwnd, GWL_STYLE, g_fsStyle);
    SetWindowPlacement(hwnd, &g_fsPlacement);
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    g_fullscreen = false;
  }
  ResizeWebView();
  PostJson(g_fullscreen ? "{\"type\":\"fullscreen\",\"on\":true}"
                        : "{\"type\":\"fullscreen\",\"on\":false}");
}

static void SaveGeometry(HWND hwnd) {
  if (!hwnd) return;
  WINDOWPLACEMENT wp;
  wp.length = sizeof(wp);
  if (!GetWindowPlacement(hwnd, &wp)) return;
  RECT r = wp.rcNormalPosition;
  std::string json = "{\"x\":" + std::to_string(r.left) + ",\"y\":" + std::to_string(r.top) +
                     ",\"w\":" + std::to_string(r.right - r.left) +
                     ",\"h\":" + std::to_string(r.bottom - r.top) +
                     ",\"max\":" + (wp.showCmd == SW_SHOWMAXIMIZED ? "1" : "0") + "}";
  EnsureDir();
  std::wstring f = AppDir() + L"\\window.json";
  std::wstring tmp = f + L".tmp";
  HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (!h) return;
  DWORD n = 0;
  WriteFile(h, json.data(), (DWORD)json.size(), &n, nullptr);
  FlushFileBuffers(h);
  CloseHandle(h);
  MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

static bool LoadGeometry(int& x, int& y, int& w, int& h, int& maxed) {
  // Default size follows the monitor DPI; never larger than the work area.
  const double scale = (double)GetDpiForSystem() / 96.0;
  RECT work = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int sw = work.right - work.left;
  const int sh = work.bottom - work.top;
  maxed = 0;
  w = (int)(1500 * scale); h = (int)(950 * scale);
  if (w > sw) w = (int)(sw * 0.92);
  if (h > sh) h = (int)(sh * 0.92);
  x = work.left + (sw - w) / 2;
  y = work.top + (sh - h) / 2;
  HANDLE fh = CreateFileW((AppDir() + L"\\window.json").c_str(), GENERIC_READ, FILE_SHARE_READ,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (fh == INVALID_HANDLE_VALUE) return false;
  char buf[256] = {0};
  DWORD rd = 0;
  ReadFile(fh, buf, sizeof(buf) - 1, &rd, nullptr);
  CloseHandle(fh);
  std::string s(buf, rd);
  x = JsonInt(s, "x", x); y = JsonInt(s, "y", y);
  w = JsonInt(s, "w", w); h = JsonInt(s, "h", h);
  maxed = JsonInt(s, "max", 0);
  // A monitor change must not park the window off-screen.
  if (x + 120 < work.left || x > work.right - 120) x = work.left + (sw - w) / 2;
  if (y + 60 < work.top || y > work.bottom - 60) y = work.top + (sh - h) / 2;
  return true;
}

static void ApplyDarkTitleBar(HWND hwnd) {
  BOOL one = TRUE;
  DwmSetWindowAttribute(hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &one, sizeof(one));
}

static RECT g_selftestRect = {};   // native-canvas regression hook (MB_NATIVE_SELFTEST)

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  /* Visual hosting: the page has no window, so the host is its input device.
   * Every mouse message in the client area belongs to it (see RelayMouse). */
  if (g_compMode && IsMouseMessage(msg) && RelayMouse(msg, wp, lp)) return 0;

  switch (msg) {
    case WM_SETCURSOR:
      if (RelayCursor(lp)) return TRUE;
      break;
    case WM_MOUSELEAVE:
      if (g_compMode && g_wvc) {
        g_trackingLeave = false;
        POINT none = {0, 0};
        g_wvc->SendMouseInput(COREWEBVIEW2_MOUSE_EVENT_KIND_LEAVE,
                              (COREWEBVIEW2_MOUSE_EVENT_VIRTUAL_KEYS)0, 0, none);
        return 0;
      }
      break;
    case WM_SETFOCUS:
      /* Keyboard is not relayed by hand: WebView2 owns an input window and
       * MoveFocus is how it is told to take the keystroke stream. */
      if (g_compMode && g_ctl)
        g_ctl->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
      break;
    case WM_EXITSIZEMOVE: {
      /* Keep the window on the monitor's work area after a drag/resize, so a
       * maximized-then-restored or heavily dragged window cannot be lost off
       * screen.  Maximize itself is left to the system. */
      RECT rc{};
      if (GetWindowRect(hwnd, &rc)) {
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) {
          const RECT& wa = mi.rcWork;
          const int w = rc.right - rc.left, h = rc.bottom - rc.top;
          int x = rc.left, y = rc.top;
          if (x + w > wa.right) x = wa.right - w;
          if (y + h > wa.bottom) y = wa.bottom - h;
          if (x < wa.left) x = wa.left;
          if (y < wa.top) y = wa.top;
          if (x != rc.left || y != rc.top)
            SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
      }
      break;
    }
    case WM_SIZE: {
      const int w = LOWORD(lp), h = HIWORD(lp);
      if (w > 0 && h > 0) {
        ResizeWebView();
        // Canvas follows the frame exactly.  The swapchain is resized and the
        // last pass is stretched into it: no re-render is triggered here.
        if (g_nativeCanvas) canvas::SetBounds(0, 0, w, h);
      }
      if (w == 0 || h == 0) { if (g_engine) g_engine->setPaused(true); }
      else if (g_engine) g_engine->setPaused(false);
      return 0;
    }
    case WM_DPICHANGED: {
      RECT* r = reinterpret_cast<RECT*>(lp);
      SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      ResizeWebView();
      {
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (g_nativeCanvas) canvas::SetBounds(0, 0, rc.right, rc.bottom);
        LogMsg("[dpi] " + std::to_string((int)LOWORD(wp)) + " client=" +
               std::to_string(rc.right) + "x" + std::to_string(rc.bottom));
      }
      return 0;
    }
    case WM_TIMER:
      /* 0x69: the page did not report a painted splash in time.  Show the
       * window anyway; an invisible product is worse than a dark one. */
      if (wp == 0x69) {
        KillTimer(hwnd, 0x69);
        ShowAppWindow("failsafe");
        return 0;
      }
      if (wp >= 0x61 && wp <= 0x67) {
        KillTimer(hwnd, wp);
        if (wp == 0x61) {
          LogMsg("[selftest] native frames=" + std::to_string(canvas::Frames()) +
                 " upload=" + std::to_string(canvas::UploadMs()) +
                 "ms present=" + std::to_string(canvas::PresentMs()) + "ms");
          PostJson("{\"type\":\"selftest\"}");
          if (std::getenv("MB_SELFTEST_EXPORT")) ExportPngNative();
          SetTimer(hwnd, 0x62, 2600, nullptr);
        } else if (wp == 0x67) {
          /* Deferred page instruction: the page has finished loading and has
           * asked for `hello`, so its message loop is live. */
          if (!g_pendingShellMsg.empty()) {
            PostJson(g_pendingShellMsg);
            g_pendingShellMsg.clear();
          }
        } else if (wp == 0x62) {
          RECT rc{};
          GetWindowRect(hwnd, &rc);
          g_selftestRect = rc;
          LogMsg("[selftest] resize-start frames=" + std::to_string(canvas::Frames()));
          SetWindowPos(hwnd, nullptr, 0, 0, rc.right - rc.left + 160, rc.bottom - rc.top + 100,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
          SetTimer(hwnd, 0x63, 600, nullptr);
        } else if (wp == 0x63) {
          LogMsg("[selftest] resize-done frames=" + std::to_string(canvas::Frames()) +
                 " (must equal resize-start: no re-render)");
          SetWindowPos(hwnd, nullptr, 0, 0, g_selftestRect.right - g_selftestRect.left,
                       g_selftestRect.bottom - g_selftestRect.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
          SetTimer(hwnd, 0x64, 1600, nullptr);
        } else if (wp == 0x64) {
          LogMsg("[selftest] maximize frames=" + std::to_string(canvas::Frames()));
          ShowWindow(hwnd, SW_MAXIMIZE);
          SetTimer(hwnd, 0x65, 1600, nullptr);
        } else if (wp == 0x65) {
          LogMsg("[selftest] restore frames=" + std::to_string(canvas::Frames()));
          ShowWindow(hwnd, SW_RESTORE);
          SetTimer(hwnd, 0x66, 1400, nullptr);
        } else {
          LogMsg("[selftest] final frames=" + std::to_string(canvas::Frames()));
          if (std::getenv("MB_SELFTEST_EXIT")) PostMessageW(hwnd, WM_CLOSE, 0, 0);
        }
        return 0;
      }
      if (wp == 0x51) {
        KillTimer(hwnd, 0x51);
        RECT a{}, b{}, c{};
        GetWindowRect(hwnd, &a);
        LONG_PTR style0 = GetWindowLongPtrW(hwnd, GWL_STYLE);
        ToggleFullscreen(hwnd);
        GetWindowRect(hwnd, &b);
        ToggleFullscreen(hwnd);
        GetWindowRect(hwnd, &c);
        LogMsg("[selftest] before=" + std::to_string(a.right - a.left) + "x" +
               std::to_string(a.bottom - a.top) + " style=" + std::to_string((long long)style0) +
               " | fullscreen=" + std::to_string(b.right - b.left) + "x" +
               std::to_string(b.bottom - b.top) + " | restored=" +
               std::to_string(c.right - c.left) + "x" + std::to_string(c.bottom - c.top));
        return 0;
      }
      break;
    case WM_SYSCOMMAND: {
      const UINT sc = (UINT)(wp & 0xFFF0);
      if (sc == SC_MINIMIZE && g_engine) g_engine->setPaused(true);
      if (sc == SC_RESTORE && g_engine) g_engine->setPaused(false);
      break;
    }
    case WM_APP_DONE:
      PublishDone();
      return 0;
    case WM_GETMINMAXINFO: {
      MINMAXINFO* mm = reinterpret_cast<MINMAXINFO*>(lp);
      // Let the system fill ptMaxSize / ptMaxPosition first; overriding the
      // message without those defaults would make a maximise land on a 0x0 box.
      DefWindowProcW(hwnd, msg, wp, lp);
      mm->ptMinTrackSize.x = 900;
      mm->ptMinTrackSize.y = 600;
      return 0;
    }
    case WM_CLOSE:
      SaveGeometry(hwnd);
      DestroyWindow(hwnd);
      return 0;
    case WM_DESTROY:
      if (g_glCanvas) gpur::shutdown(); else nc::Shutdown();
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- WebView2 callbacks

/* The page exports screenshots with an <a download>; WebView2 routes that
 * through its own download shelf.  Intercept it and show the system "Save As"
 * dialog instead, pre-filled with a timestamped file name. */
class DownloadHandler : public Cb<ICoreWebView2DownloadStartingEventHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(ICoreWebView2* sender, ICoreWebView2DownloadStartingEventArgs* args) override {
    (void)sender;
    if (!args) return S_OK;

    // Default name: mandelbrot-YYYYMMDD-HHMMSS.png
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t name[96];
    swprintf_s(name, L"mandelbrot-%04d%02d%02d-%02d%02d%02d.png",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // Default folder: whatever WebView2 proposed (usually the Downloads dir).
    IFileSaveDialog* dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dlg))) && dlg) {
      COMDLG_FILTERSPEC filt[1] = {{L"PNG image", L"*.png"}};
      dlg->SetFileTypes(1, filt);
      dlg->SetDefaultExtension(L"png");
      dlg->SetFileName(name);
      dlg->SetTitle(L"Save image");
      if (SUCCEEDED(dlg->Show(g_hwnd))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item)) && item) {
          LPWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
            args->put_ResultFilePath(path);
            CoTaskMemFree(path);
          }
          item->Release();
        }
      } else {
        args->put_Cancel(TRUE);   // the user dismissed the dialog
      }
      dlg->Release();
    }
    args->put_Handled(TRUE);     // no download shelf; write straight to the chosen path
    return S_OK;
  }
};

/* ------------------------------------------------------------------ WebView2 bring-up
 *
 * Two hosting modes, one body of setup:
 *
 *  visual hosting  Environment3::CreateCoreWebView2CompositionController.  The
 *                  page is a visual in *our* DirectComposition tree, stacked
 *                  above the canvas visual.  A transparent DefaultBackgroundColor
 *                  then really is transparent: the C++ picture shows through.
 *                  Input is the host's job (RelayMouse / MoveFocus / RelayCursor).
 *
 *  windowed        CreateCoreWebView2Controller.  The page is a child HWND and
 *                  can never reveal anything behind it, so the page keeps its
 *                  own canvas and this is only a fallback.
 */
static void NavigateToApp();

/** Settings, message plumbing, shared buffers — identical in both modes. */
static void SetupWebView(ICoreWebView2Controller* ctl, bool transparent) {
  g_ctl = ctl;
  g_ctl->AddRef();
  g_ctl->get_CoreWebView2(&g_wv);
  if (!g_wv) { LogMsg("[wv2] no core"); return; }
  g_wv->AddRef();

  ICoreWebView2Controller2* ctl2 = nullptr;
  if (SUCCEEDED(g_ctl->QueryInterface(__uuidof(ICoreWebView2Controller2), (void**)&ctl2)) && ctl2) {
    /* Transparent is only meaningful in visual hosting (there is a picture
     * underneath).  In windowed mode an opaque background keeps the page from
     * showing the window's own fill. */
    COREWEBVIEW2_COLOR bg = transparent ? COREWEBVIEW2_COLOR{0, 0, 0, 0}
                                        : COREWEBVIEW2_COLOR{255, 8, 9, 11};
    ctl2->put_DefaultBackgroundColor(bg);
    ctl2->Release();
  }

  g_wv->QueryInterface(__uuidof(ICoreWebView2_17), (void**)&g_wv17);
  if (g_env) g_env->QueryInterface(__uuidof(ICoreWebView2Environment12), (void**)&g_env12);

  ICoreWebView2Settings* st = nullptr;
  if (SUCCEEDED(g_wv->get_Settings(&st)) && st) {
    st->put_AreDefaultContextMenusEnabled(FALSE);
    st->put_IsStatusBarEnabled(FALSE);
    st->put_AreDevToolsEnabled(TRUE);
    st->Release();
  }

  EventRegistrationToken tok = {};
  g_wv->add_WebMessageReceived(new MsgHandler(), &tok);
  EventRegistrationToken dtok = {};
  ICoreWebView2_4* wv4 = nullptr;
  if (SUCCEEDED(g_wv->QueryInterface(IID_PPV_ARGS(&wv4))) && wv4) {
    wv4->add_DownloadStarting(new DownloadHandler(), &dtok);
    wv4->Release();
  }
  g_wv->AddWebResourceRequestedFilter(L"https://mb.test/*", COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
  EventRegistrationToken rtok = {};
  g_wv->add_WebResourceRequested(new ResHandler(), &rtok);

  /* Expose the C++ view arithmetic.  The page probes `mbview` once and uses
   * it only when `probe().gmp` is true, so a browser run (no object) keeps
   * its own path and nothing has to be deployed in lockstep. */
  {
    ViewHostObject* vo = new ViewHostObject();
    VARIANT v;
    VariantInit(&v);
    v.vt = VT_DISPATCH;
    v.pdispVal = vo;
    const HRESULT hr = g_wv->AddHostObjectToScript(L"mbview", &v);
    vo->Release();
    LogMsg(std::string("[view] host object ") + (SUCCEEDED(hr) ? "ok" : "FAILED") +
           " gmp=" + (mbc::gmp::available() ? mbc::gmp::version() : "none"));
  }

  if (!g_wv17 || !g_env12 || !CreateSlots()) {
    LogMsg("[shm] shared buffers unavailable -> worker fallback");
  }

  ResizeWebView();
  g_ctl->put_IsVisible(TRUE);
  NavigateToApp();
}

static void NavigateToApp() {
  if (!g_wv) return;
  std::wstring url = L"https://mb.test/index.html";
  if (const char* suffix = std::getenv("MB_URL_SUFFIX")) {
    url.append(suffix, suffix + std::strlen(suffix));
  }
  g_wv->Navigate(url.c_str());
  LogMsg("[wv2] navigated");
}

static void ControllerFailed(HRESULT hr) {
  /* The only realistic cause is the WebView2 user-data folder being held by
   * another process (a leaked msedgewebview2 from an older run).  Without a
   * controller the window has no UI at all, which reads as a black screen. */
  LogMsg("[wv2] controller failed hr=" + std::to_string((int)hr));
  ShowAppWindow("controller failed");
  if (!std::getenv("MB_NO_BOX")) {
    MessageBoxW(g_hwnd, L"界面未能启动：WebView2 配置目录被占用。\n请关闭其他 Mandelbrot View 窗口后重试。",
                L"Mandelbrot View", MB_ICONWARNING | MB_OK);
  }
  PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}

class CtlHandler : public Cb<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Controller* ctl) override {
    if (FAILED(hr) || !ctl) { ControllerFailed(hr); return S_OK; }
    SetupWebView(ctl, false);
    return S_OK;
  }
};

class CompHandler : public Cb<ICoreWebView2CreateCoreWebView2CompositionControllerCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr,
                                   ICoreWebView2CompositionController* comp) override {
    auto fallback = [&](const char* why) {
      LogMsg(std::string("[wv2] visual hosting unavailable (") + why + ") -> windowed");
      /* `comp` belongs to the runtime and dies with this callback: never
       * Release it here, only stop using it. */
      g_compMode = false;
      if (g_env) g_env->CreateCoreWebView2Controller(g_hwnd, new CtlHandler());
    };
    if (FAILED(hr) || !comp) { ControllerFailed(hr); return S_OK; }

    /* Documented: an object implementing ICoreWebView2CompositionController
     * also implements ICoreWebView2Controller — bounds, visibility, focus,
     * background all come from there. */
    ICoreWebView2Controller* ctl = nullptr;
    if (FAILED(comp->QueryInterface(IID_PPV_ARGS(&ctl))) || !ctl) {
      fallback("no ICoreWebView2Controller");
      return S_OK;
    }

    IUnknown* overlay = canvas::OverlayVisual();
    if (!overlay) {
      ctl->Release();
      fallback("no canvas visual");
      return S_OK;
    }
    if (FAILED(comp->put_RootVisualTarget(overlay))) {
      ctl->Release();
      fallback("put_RootVisualTarget");
      return S_OK;
    }
    canvas::CommitComposition();

    g_wvc = comp;
    g_wvc->AddRef();
    SetupWebView(ctl, true);
    ctl->Release();
    LogMsg(std::string("[wv2] visual hosting active, canvas=") + canvas::Backend());
    return S_OK;
  }
};

class EnvHandler : public Cb<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler> {
 public:
  HRESULT STDMETHODCALLTYPE Invoke(HRESULT hr, ICoreWebView2Environment* env) override {
    if (FAILED(hr) || !env) { LogMsg("[wv2] env failed"); return S_OK; }
    g_env = env;
    g_env->AddRef();
    if (g_compMode) {
      ICoreWebView2Environment3* env3 = nullptr;
      if (SUCCEEDED(g_env->QueryInterface(__uuidof(ICoreWebView2Environment3), (void**)&env3)) && env3) {
        const HRESULT chr = env3->CreateCoreWebView2CompositionController(g_hwnd, new CompHandler());
        env3->Release();
        if (SUCCEEDED(chr)) return S_OK;
        LogMsg("[wv2] composition controller create failed hr=" + std::to_string((int)chr));
      } else {
        LogMsg("[wv2] runtime has no visual hosting (Environment3)");
      }
      g_compMode = false;
    }
    g_env->CreateCoreWebView2Controller(g_hwnd, new CtlHandler());
    return S_OK;
  }
};

// Minimal options object so we can pass the same un-throttled Chromium flags
// the reference shell uses (occlusion/backgrounding otherwise starves the
// renderer whenever another window covers ours).
class EnvOptions : public ICoreWebView2EnvironmentOptions {
 public:
  explicit EnvOptions(std::wstring args) : args_(std::move(args)) {}
  ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&rc_); }
  ULONG STDMETHODCALLTYPE Release() override {
    ULONG r = InterlockedDecrement(&rc_);
    if (!r) delete this;
    return r;
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override {
    if (!pp) return E_POINTER;
    *pp = nullptr;
    if (riid == __uuidof(ICoreWebView2EnvironmentOptions) || riid == IID_IUnknown) {
      *pp = static_cast<ICoreWebView2EnvironmentOptions*>(this);
      AddRef();
      return S_OK;
    }
    return E_NOINTERFACE;
  }
  HRESULT get_AdditionalBrowserArguments(LPWSTR* v) override { return Dup(v, args_); }
  HRESULT put_AdditionalBrowserArguments(LPCWSTR v) override { args_ = v ? v : L""; return S_OK; }
  HRESULT get_Language(LPWSTR* v) override { return Dup(v, L""); }
  HRESULT put_Language(LPCWSTR) override { return S_OK; }
  HRESULT get_TargetCompatibleBrowserVersion(LPWSTR* v) override { return Dup(v, L"100.0.0.0"); }
  HRESULT put_TargetCompatibleBrowserVersion(LPCWSTR) override { return S_OK; }
  HRESULT get_AllowSingleSignOnUsingOSPrimaryAccount(BOOL* a) override {
    if (a) *a = FALSE;
    return S_OK;
  }
  HRESULT put_AllowSingleSignOnUsingOSPrimaryAccount(BOOL) override { return S_OK; }

 private:
  static HRESULT Dup(LPWSTR* v, const std::wstring& s) {
    if (!v) return E_POINTER;
    size_t n = (s.size() + 1) * sizeof(wchar_t);
    *v = static_cast<LPWSTR>(CoTaskMemAlloc(n));
    if (!*v) return E_OUTOFMEMORY;
    memcpy(*v, s.c_str(), n);
    return S_OK;
  }
  LONG rc_ = 1;
  std::wstring args_;
};

// ---------------------------------------------------------------- entry

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  SetUnhandledExceptionFilter(HostCrashFilter);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  EnsureDir();
  /* Presets are compiled into ui_resources.cpp (presets-data.js); no folder,
   * no scan and no first-run creation.  Import/export still uses real files
   * through the two dialogs, and "mine" presets live in localStorage. */
  LogMsg("[boot] shell start presets=embedded");

  /* One product instance per desktop.  Both instances would use the same
   * <AppDir>\WebView2 user-data folder, and the second one cannot create its
   * controller: it only painted a black, UI-less window while the first
   * instance kept showing its last frame — which is what looked like "the app
   * restored the previous frame and the previous timing on startup".  The
   * mutex lives for the whole process (never closed on the success path); the
   * second launch hands the existing window back to the user instead. */
  static HANDLE g_instance = CreateMutexW(nullptr, FALSE, L"Local\\MandViewHost.Instance");
  if (g_instance && GetLastError() == ERROR_ALREADY_EXISTS) {
    if (HWND prev = FindWindowW(L"MandViewHost", nullptr)) {
      if (IsIconic(prev)) ShowWindow(prev, SW_RESTORE);
      SetForegroundWindow(prev);
      LogMsg("[boot] already running -> activate the existing window");
    }
    return 0;
  }

  int x, y, w, h, maxed;
  LoadGeometry(x, y, w, h, maxed);

  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInst;
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.hbrBackground = CreateSolidBrush(RGB(8, 9, 11));
  wc.lpszClassName = L"MandViewHost";
  /* CS_DBLCLKS: the page's canvas zooms on double-click, and in visual hosting
   * the host window is the one that has to receive WM_*DBLCLK and relay it. */
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  // Icon comes from the .exe resource (app.rc -> app.ico), so Explorer,
  // Alt+Tab, the taskbar and the title bar all show the same artwork.
  wc.hIcon = LoadIcon(hInst, MAKEINTRESOURCE(1));
  wc.hIconSm = (HICON)LoadImage(hInst, MAKEINTRESOURCE(1), IMAGE_ICON,
                                GetSystemMetrics(SM_CXSMICON),
                                GetSystemMetrics(SM_CYSMICON), 0);
  if (!RegisterClassExW(&wc)) { LogMsg("[boot] RegisterClass failed"); return 1; }

  // A maximised window is a supported state: the page is fluid, WM_SIZE resizes
  // the WebView2 child and the engine keeps its pause/resume handling.
  g_hwnd = CreateWindowExW(0, L"MandViewHost", L"Mandelbrot View",
                           WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           x, y, w, h, nullptr, nullptr, hInst, nullptr);
  if (!g_hwnd) { LogMsg("[boot] CreateWindow failed"); return 1; }
  ApplyDarkTitleBar(g_hwnd);
  /* The window stays hidden until the page has painted its own splash (the
   * `splashPainted` message below).  Showing it earlier is what put a black
   * rectangle on screen for the 1-2 s WebView2 needs to create its renderer,
   * parse the bundle and compile shaders.  The failsafe timer keeps a broken
   * WebView2 from leaving the product invisible. */
  g_startMaxed = maxed != 0;
  SetTimer(g_hwnd, 0x69, 2500, nullptr);

  // Route 1: a native child window under the transparent WebView2 carries the
  // picture.  The OpenGL canvas is the product path; the D3D11 bitblt canvas
  // remains as a fallback, and MB_GL=0 / MB_CL=0 pin either half off for A/B.
  {
    RECT rc{};
    GetClientRect(g_hwnd, &rc);
    /* Canvas selection.
     *
     * The page's own canvas is the product path.  The renderer that owns the
     * deep views is WebGPU (`js/wgpu`), which draws the whole frame in one
     * dispatch and moves the picture with a compositor transform: measured
     * 60 fps through an eight-notch burst (p90 16.8 ms, exactly one frame over
     * 33 ms) on the same 1e-20 view where the CPU canvas needed 1.4 s of
     * compute and 6.4 s of wall time.  A native canvas can only be *shown* by
     * making this page a composition visual and hiding its pixels, which
     * excludes that renderer outright.
     *
     * MB_NATIVE=1 brings the DirectComposition canvas back for A/B and for a
     * machine whose WebGPU device fails; MB_GL=1 selects the older OpenGL
     * child window (kept for measurement, it cannot show through a page).
     * MB_CANVAS=none still disables every native canvas. */
    const char* glEnv = std::getenv("MB_GL");
    const bool wantGL = glEnv && std::atoi(glEnv) != 0;
    const char* nativeEnv = std::getenv("MB_NATIVE");
    const bool wantNative = nativeEnv && std::atoi(nativeEnv) != 0;
    if (wantGL && gpur::init(g_hwnd)) {
      g_glCanvas = true;
      g_nativeCanvas = true;
      canvas::SetBounds(0, 0, rc.right, rc.bottom);
      const gpur::Info& gi = gpur::info();
      LogMsg("[gl] canvas ready GL=" + gi.glRenderer + " (" + gi.glVersion + ")");
      if (gi.cl) {
        const char* clEnv = std::getenv("MB_CL");
        /* OpenCL is opt-in until the new core owns the compute path.  Left on
         * by default it hijacks the frame: the kernel writes a float field for
         * the OLD engine's request, the dispatcher skips the RGBA upload, and
         * the reconstructed core never runs at all (log: bits=53, ref=0).
         * MB_CL=1 enables it for measurement. */
        g_gpuField = clEnv && std::atoi(clEnv) != 0;
        LogMsg("[cl] device " + gi.clDevice + " " + gi.clVersion +
               (g_gpuField ? " -> field on GPU" : " -> field disabled"));
      } else {
        LogMsg("[cl] unavailable: " + gi.why + " -> field on CPU");
      }
    } else if (wantNative && nc::Init(g_hwnd)) {
      g_nativeCanvas = true;
      canvas::SetBounds(0, 0, rc.right, rc.bottom);
      /* Visual hosting is only possible when the canvas really is a visual:
       * a page HWND over a child window can never show it through. */
      g_compMode = canvas::Composited();
      LogMsg(std::string("[native] canvas=") + canvas::Backend() + " " +
             std::to_string(rc.right) + "x" + std::to_string(rc.bottom) +
             (g_compMode ? " -> visual hosting" : " -> page stays opaque"));
    } else {
      LogMsg(wantNative
                 ? std::string("[native] canvas init failed (") + nc::CanvasWhy() +
                       ") -> JS canvas fallback"
                 : std::string("[native] off by default -> page canvas"));
    }
  }

  // Regression hook: MB_FS_SELFTEST=1 toggles fullscreen twice 1.2 s after
  // start and logs the window rect/style, so the path can be verified without
  // a human clicking the menu.
  if (std::getenv("MB_FS_SELFTEST")) SetTimer(g_hwnd, 0x51, 1200, nullptr);

  LoadAppConfig();
  g_engine = new mbc::Engine(g_cfg.nthread);
  LogMsg("[boot] engine threads=" + std::to_string(g_engine->threadCount()));
  /* Boost <-> MPFR bridge: MB_BRIDGECHECK=1 runs the limb/orbit self-test,
   * and MB_BRIDGE_JSON=<file> validates a gmp-wasm orbit exported by
   * tools/bridge-crosscheck.mjs against a fresh Boost recomputation. */
  if (std::getenv("MB_BRIDGECHECK")) {
    std::string blog;
    const bool bok = mbc::bridgeSelfTest(blog);
    LogMsg("[bridge] self-test " + std::string(bok ? "PASS" : "FAIL"));
    if (!bok) LogMsg("[bridge] " + blog);
    if (const char* jf = std::getenv("MB_BRIDGE_JSON")) {
      std::ifstream f(jf, std::ios::binary);
      if (f) {
        std::ostringstream ss;
        ss << f.rdbuf();
        std::string jlog;
        const bool jok = mbc::bridgeCheckJson(ss.str(), jlog);
        LogMsg("[bridge] MPFR orbit " + std::string(jok ? "PASS" : "FAIL") +
               " (" + jf + ")");
        LogMsg("[bridge] " + jlog);
      } else {
        LogMsg("[bridge] cannot open MB_BRIDGE_JSON");
      }
    }
  }

  std::thread(RenderThread).detach();

  std::wstring udf = AppDir() + L"\\WebView2";
  std::wstring args =
      L"--disable-background-timer-throttling"
      L" --disable-backgrounding-occluded-windows"
      L" --disable-renderer-backgrounding"
      L" --disable-features=CalculateNativeWinOcclusion"
      L" --enable-gpu-rasterization"
      L" --enable-zero-copy"
      L" --enable-features=CanvasOopRasterization"
      L" --ignore-gpu-blocklist"
      /* Diagnostics: MB_CDP=1 exposes the page on a local DevTools port so a
       * headless script can inspect and drive the UI.  Nothing is enabled
       * unless the variable is set at launch. */
      ;
  if (std::getenv("MB_CDP")) args += L" --remote-debugging-port=9222";

  HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(),
                                                        new EnvOptions(args), new EnvHandler());
  if (FAILED(hr)) {
    LogMsg("[wv2] env create failed hr=" + std::to_string((int)hr) + " -> retry defaults");
    CreateCoreWebView2EnvironmentWithOptions(nullptr, udf.c_str(), nullptr, new EnvHandler());
  }

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  g_stopRender.store(true);
  g_jobCv.notify_all();
  g_slotCv.notify_all();
  delete g_engine;
  g_engine = nullptr;
  /* Close() before releasing: without it the browser process can outlive the
   * host and keep <AppDir>\WebView2 locked, which makes the next launch fail
   * to create its controller (black, UI-less window). */
  if (g_ctl) g_ctl->Close();
  if (g_wv17) g_wv17->Release();
  if (g_env12) g_env12->Release();
  if (g_wv) g_wv->Release();
  if (g_ctl) g_ctl->Release();
  if (g_env) g_env->Release();
  for (int i = 0; i < kSlotCount; ++i) if (g_slots[i].buf) g_slots[i].buf->Release();
  for (int i = 0; i < kFieldSlotCount; ++i)
    if (g_fieldSlots[i].buf) g_fieldSlots[i].buf->Release();
  CoUninitialize();
  LogMsg("[boot] exit");
  return 0;
}
