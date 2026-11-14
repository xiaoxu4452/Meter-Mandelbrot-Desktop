// ---------------------------------------------------------------------------
// dib_canvas.cpp - the product canvas, as a plain GDI DIB child window.
//
// This replaces the D3D11 swapchain that used to sit here.  The old canvas
// needed a device, a swapchain, a fullscreen-triangle shader and an
// UpdateSubresource per tile, and it only painted inside Present(): any bare
// invalidation (a resize, the WebView2 sibling recompositing, a DPI change)
// left the client area undefined until the next frame.  The window is a
// picture, so the picture is a bitmap:
//
//   staging  : the pass being computed, one RGBA DIB
//   display  : the last committed pass, drawn with a preview transform
//
// Present is one StretchBlt in the identity case and one StretchBlt with a
// source rect otherwise; no device, no context, nothing to lose.
//
// The public API is unchanged (see native_canvas.h) so the shell does not know
// which canvas it got.
// ---------------------------------------------------------------------------
#include "canvas_backend.h"
#include "native_canvas.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>
#include <windows.h>

#include <cstdarg>
namespace {
void dtrace(const char* fmt, ...) {
    const char* on = std::getenv("MB_DIB_TRACE");
    if (!on) return;
    const char* tmp = std::getenv("TEMP");
    char path[512];
    std::snprintf(path, sizeof(path), "%s\\dib.log", tmp ? tmp : ".");
    FILE* f = std::fopen(path, "ab");
    if (!f) return;
    va_list ap; va_start(ap, fmt); std::vfprintf(f, fmt, ap); va_end(ap);
    std::fputc('\n', f); std::fclose(f);
}
}

namespace {

struct Surface {
    int w = 0;
    int h = 0;
    std::vector<uint8_t> px;      // RGBA, top-down

    void resize(int nw, int nh) {
        if (nw <= 0 || nh <= 0) return;
        if (nw == w && nh == h && px.size() == (size_t)nw * nh * 4) return;
        w = nw;
        h = nh;
        px.assign((size_t)nw * nh * 4, 0);
    }
};

HWND g_hwnd = nullptr;
HWND g_host = nullptr;
int g_w = 1, g_h = 1;
std::mutex g_mu;
Surface g_staging;
Surface g_display;
bool g_haveDisplay = false;

/* src = s * dst + o, in normalised units (see the GL canvas, same contract). */
double g_sx = 1.0, g_sy = 1.0, g_ox = 0.0, g_oy = 0.0;
int g_frames = 0;
double g_uploadMs = 0.0;
double g_presentMs = 0.0;

double NowMs() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point t0 = clock::now();
    return std::chrono::duration<double, std::milli>(clock::now() - t0).count();
}

void FillBackground(HDC dc, int w, int h) {
    /* The shell's own backdrop.  A fresh window has no picture yet, and a
     * black rectangle reads as a crash. */
    HBRUSH b = CreateSolidBrush(RGB(8, 12, 18));
    RECT r{0, 0, w, h};
    FillRect(dc, &r, b);
    DeleteObject(b);
}

/** Draw the committed pass through the preview transform. */
void PresentLocked() {
    if (!g_hwnd) return;
    const double t0 = NowMs();
    HDC dc = GetDC(g_hwnd);
    if (!dc) return;
    FillBackground(dc, g_w, g_h);
    if (g_haveDisplay && g_display.w > 0 && g_display.h > 0) {
        BITMAPINFO bi = {};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = g_display.w;
        bi.bmiHeader.biHeight = -g_display.h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        HDC mem = CreateCompatibleDC(dc);
        void* bits = nullptr;
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (mem && bmp && bits) {
            std::memcpy(bits, g_display.px.data(), g_display.px.size());
            HGDIOBJ old = SelectObject(mem, bmp);
            /* The DIB is RGBA (BGRA in memory, which is what GDI wants); its
             * fourth byte is ignored by the blit. */
            const int sw = (int)std::lround(g_sx * g_display.w);
            const int sh = (int)std::lround(g_sy * g_display.h);
            const int sox = (int)std::lround(g_ox * g_display.w);
            const int soy = (int)std::lround(g_oy * g_display.h);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            if (sw > 0 && sh > 0) {
                StretchBlt(dc, 0, 0, g_w, g_h, mem, sox, soy, sw, sh, SRCCOPY);
            }
            SelectObject(mem, old);
        }
        if (bmp) DeleteObject(bmp);
        if (mem) DeleteDC(mem);
    }
    ReleaseDC(g_hwnd, dc);
    g_presentMs = NowMs() - t0;
}

LRESULT CALLBACK CanvasProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;   // PresentLocked paints every pixel it owns
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            std::lock_guard<std::mutex> lk(g_mu);
            PresentLocked();
            return 0;
        }
        case WM_SIZE:
            return 0;
        case WM_LBUTTONDOWN:
            SetFocus(hwnd);
            return 0;
        case WM_MOUSEWHEEL:
            if (g_host) PostMessageW(g_host, WM_APP + 6, wp, lp);
            return 0;
        default:
            return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

}  // namespace

namespace ncg {

bool Init(HWND host) {
    g_host = host;
    WNDCLASSEXW cc = {};
    cc.cbSize = sizeof(cc);
    cc.lpfnWndProc = CanvasProc;
    cc.hInstance = GetModuleHandleW(nullptr);
    cc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    cc.lpszClassName = L"MandCanvasDib";
    cc.style = CS_HREDRAW | CS_VREDRAW;
    RegisterClassExW(&cc);

    RECT rc{};
    GetClientRect(host, &rc);
    g_w = std::max(1L, rc.right);
    g_h = std::max(1L, rc.bottom);
    g_hwnd = CreateWindowExW(0, L"MandCanvasDib", L"",
                             WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN, 0, 0, g_w,
                             g_h, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g_hwnd) return false;
    SetWindowPos(g_hwnd, HWND_BOTTOM, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
    dtrace("init hwnd=%p %dx%d", (void*)g_hwnd, g_w, g_h);
    if (std::getenv("MB_DIB_TEST")) {
        g_staging.resize(g_w, g_h);
        for (size_t i = 0; i < g_staging.px.size(); i += 4) {
            g_staging.px[i + 0] = 0;
            g_staging.px[i + 1] = 255;
            g_staging.px[i + 2] = 0;
            g_staging.px[i + 3] = 255;
        }
        CommitPass();
    }
    return true;
}

void Shutdown() {
    if (g_hwnd) {
        DestroyWindow(g_hwnd);
        g_hwnd = nullptr;
    }
}

bool Ready() { return g_hwnd != nullptr; }

void SetBounds(int x, int y, int w, int h) {
    if (!g_hwnd) return;
    if (w <= 0 || h <= 0) return;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        g_w = w;
        g_h = h;
    }
    MoveWindow(g_hwnd, x, y, w, h, TRUE);
    InvalidateRect(g_hwnd, nullptr, FALSE);
}

void BeginPass(int passW, int passH, int ss, int cw, int ch) {
    (void)ss;
    (void)cw;
    (void)ch;
    std::lock_guard<std::mutex> lk(g_mu);
    g_staging.resize(passW, passH);
}

void UploadTile(const uint8_t* frame, int frameW, int frameH,
                int x, int y, int w, int h) {
    if (!frame || w <= 0 || h <= 0) return;
    const double t0 = NowMs();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_staging.w <= 0 || g_staging.h <= 0) {
        g_staging.resize(frameW, frameH);
    }
    const int copyW = std::min(w, g_staging.w - x);
    const int copyH = std::min(h, g_staging.h - y);
    if (copyW <= 0 || copyH <= 0) return;
    for (int j = 0; j < copyH; ++j) {
        const uint8_t* src = frame + ((size_t)(y + j) * (size_t)frameW + (size_t)x) * 4;
        uint8_t* dst = g_staging.px.data() +
                       ((size_t)(y + j) * (size_t)g_staging.w + (size_t)x) * 4;
        std::memcpy(dst, src, (size_t)copyW * 4);
    }
    g_uploadMs = NowMs() - t0;
}

bool CommitPass() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_staging.w <= 0 || g_staging.h <= 0) return false;
    g_display.w = g_staging.w;
    g_display.h = g_staging.h;
    g_display.px.swap(g_staging.px);
    g_staging.px.assign(g_staging.px.size(), 0);
    g_haveDisplay = true;
    g_sx = g_sy = 1.0;
    g_ox = g_oy = 0.0;
    ++g_frames;
    PresentLocked();
    return true;
}

bool CommitPassWithPreview(double sx, double ox, double sy, double oy) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_staging.w <= 0 || g_staging.h <= 0) return false;
    g_display.w = g_staging.w;
    g_display.h = g_staging.h;
    g_display.px.swap(g_staging.px);
    g_staging.px.assign(g_staging.px.size(), 0);
    g_haveDisplay = true;
    if (sx > 0.0 && sy > 0.0) {
        g_sx = sx;
        g_sy = sy;
        g_ox = ox;
        g_oy = oy;
    }
    ++g_frames;
    dtrace("commit %dx%d frames=%d", g_display.w, g_display.h, g_frames);
    PresentLocked();
    return true;
}

void Preview(double scale, double dx, double dy, double cx, double cy) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (scale <= 0.0) return;
    /* Scale about (cx, cy), then translate -- the same transform the page's
     * blitSnapshot() describes. */
    const double nx = cx - scale * cx + dx;
    const double ny = cy - scale * cy + dy;
    g_ox = g_ox * scale + nx * g_sx;
    g_oy = g_oy * scale + ny * g_sy;
    g_sx *= scale;
    g_sy *= scale;
    PresentLocked();
}

void SetPreview(double sx, double ox, double sy, double oy) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (sx <= 0.0 || sy <= 0.0) return;
    g_sx = sx;
    g_sy = sy;
    g_ox = ox;
    g_oy = oy;
    PresentLocked();
}

void ResetPreview() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_sx = g_sy = 1.0;
    g_ox = g_oy = 0.0;
}

void Present() {
    std::lock_guard<std::mutex> lk(g_mu);
    PresentLocked();
}

HWND Hwnd() { return g_hwnd; }
double LastUploadMs() { return g_uploadMs; }
double LastPresentMs() { return g_presentMs; }
int FrameCount() { return g_frames; }

}  // namespace ncg
