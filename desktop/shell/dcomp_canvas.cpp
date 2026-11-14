// ---------------------------------------------------------------------------
// dcomp_canvas.cpp — the canvas as a DirectComposition visual (no child HWND).
//
// Why this shape
// --------------
// A page rendered into its own child HWND is an opaque window: nothing behind
// it can ever show through, whatever `background: transparent` and
// DefaultBackgroundColor say.  Visual hosting inverts that: WebView2 renders
// into a visual *we* own, and the page's alpha is composited against whatever
// sits below it in our tree.  So the picture is a visual, the page is a visual
// above it, and transparency means what it says.
//
// The tree (one IDCompositionDevice, one target for the host HWND):
//
//   root
//    ├── canvas   content = composition swapchain (the fractal)
//    └── overlay  handed to ICoreWebView2CompositionController
//                 put_RootVisualTarget — WebView2 grows its tree under it
//
// Presentation is the same pipeline the old child-window canvas used, which is
// already verified against the CPU reference:
//   staging texture  ← UploadTile / UpdateSubresource (one box per tile)
//   display texture  ← promoted only when the whole pass has landed
//   quad in the shader ← preview transform (scale + offset) and supersample
//                        reduction, so a gesture is one draw, not a JS blit
//
// Coordinates are pixels (IDCompositionVisual offsets), the window is per-
// monitor DPI aware, so a buffer of clientW×clientH maps 1:1 and no DPI
// compensation is needed.
// ---------------------------------------------------------------------------
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dcomp.h>
#include <dxgi1_2.h>

#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "canvas_backend.h"
#include "native_canvas.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "uuid.lib")

namespace {

struct Tex {
  ID3D11Texture2D* tex = nullptr;
  ID3D11ShaderResourceView* srv = nullptr;
  int w = 0, h = 0;
};

struct Gpu {
  ID3D11Device* dev = nullptr;
  ID3D11DeviceContext* ctx = nullptr;
  IDXGISwapChain1* sc = nullptr;
  ID3D11RenderTargetView* rtv = nullptr;

  IDCompositionDevice* dcomp = nullptr;
  IDCompositionTarget* target = nullptr;
  IDCompositionVisual* root = nullptr;
  IDCompositionVisual* canvasVis = nullptr;
  IDCompositionVisual* overlayVis = nullptr;

  ID3D11VertexShader* vs = nullptr;
  ID3D11PixelShader* ps = nullptr;
  ID3D11RasterizerState* rs = nullptr;
  ID3D11SamplerState* sm = nullptr;
  ID3D11Buffer* cb = nullptr;

  Tex slots[2];
  int display = -1;
  int pendingSS = 1;
  int displaySS = 1;
  int w = 1, h = 1;
  bool ready = false;
};

Gpu g;
HWND g_host = nullptr;
std::mutex g_mu;
double g_sx = 1.0, g_sy = 1.0, g_ox = 0.0, g_oy = 0.0;
double g_upMs = 0.0, g_prMs = 0.0;
int g_frames = 0;
std::string g_why = "not started";

struct CbData {
  float sx, sy, ox, oy;      // dest-normalised -> source-normalised
  float tw, th, ss, pad;
  float bgr, bgg, bgb, bga;
};
static_assert(sizeof(CbData) == 48, "cbuffer packing");
CbData g_cbData = {1, 1, 0, 0, 1, 1, 1, 0, 0.03137f, 0.03529f, 0.04314f, 1.0f};

/* Premultiplied alpha: the canvas is the bottom layer, so every pixel it
 * writes is opaque.  Forcing a=1 keeps the page's blend predictable even if a
 * pass ever produces a zero alpha channel. */
const char* kHLSL =
    "Texture2D tex0 : register(t0);\n"
    "SamplerState samp0 : register(s0);\n"
    "cbuffer U : register(b0) {\n"
    "  float4 xf;   // sx, sy, ox, oy\n"
    "  float4 tex;  // texW, texH, ss, unused\n"
    "  float4 bg;\n"
    "};\n"
    "struct V { float4 p : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "V vsmain(uint id : SV_VertexID) {\n"
    "  float2 q = float2((float)((id << 1) & 2), (float)(id & 2));\n"
    "  V o; o.p = float4(q * 2.0f - 1.0f, 0.0f, 1.0f);\n"
    "  o.uv = float2(q.x, 1.0f - q.y); return o;\n"
    "}\n"
    "float4 psmain(V i) : SV_TARGET {\n"
    "  float2 uv = float2(xf.x * i.uv.x + xf.z, xf.y * i.uv.y + xf.w);\n"
    "  if (uv.x < 0.0f || uv.x > 1.0f || uv.y < 0.0f || uv.y > 1.0f)\n"
    "    return float4(bg.rgb * bg.a, bg.a);\n"
    "  float ss = tex.z;\n"
    "  if (ss < 1.5f) { float4 c = tex0.SampleLevel(samp0, uv, 0); return float4(c.rgb, 1.0f); }\n"
    "  float4 c = 0.0f;\n"
    "  int is = (int)ss;\n"
    "  for (int j = 0; j < 4; ++j) {\n"
    "    if (j >= is) break;\n"
    "    for (int k = 0; k < 4; ++k) {\n"
    "      if (k >= is) break;\n"
    "      float2 o = float2((j + 0.5f - 0.5f * ss) / tex.x,\n"
    "                        (k + 0.5f - 0.5f * ss) / tex.y);\n"
    "      c += tex0.SampleLevel(samp0, uv + o, 0);\n"
    "    }\n"
    "  }\n"
    "  c /= (ss * ss);\n"
    "  return float4(c.rgb, 1.0f);\n"
    "}\n";

void DcLog(const char* s) {
  if (!std::getenv("MB_DC_LOG")) return;
  wchar_t dir[MAX_PATH] = {0};
  if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
  std::wstring path = std::wstring(dir) + L"\\MandelbrotView\\dcomp.log";
  CreateDirectoryW((std::wstring(dir) + L"\\MandelbrotView").c_str(), nullptr);
  FILE* f = _wfopen(path.c_str(), L"ab");
  if (!f) return;
  SYSTEMTIME st{};
  GetLocalTime(&st);
  fprintf(f, "%02d:%02d:%02d %s\n", st.wHour, st.wMinute, st.wSecond, s);
  fclose(f);
}

std::string Hr(const char* what, HRESULT hr) {
  char buf[160];
  std::snprintf(buf, sizeof(buf), "%s hr=0x%08lx", what, (unsigned long)hr);
  return buf;
}

double NowMs() {
  LARGE_INTEGER f, c;
  QueryPerformanceFrequency(&f);
  QueryPerformanceCounter(&c);
  return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

void ReleaseTex(Tex& t) {
  if (t.srv) { t.srv->Release(); t.srv = nullptr; }
  if (t.tex) { t.tex->Release(); t.tex = nullptr; }
  t.w = t.h = 0;
}

void SetupDraw() {
  if (!g.rtv) return;
  D3D11_VIEWPORT vp = {0, 0, (float)g.w, (float)g.h, 0, 1};
  g.ctx->OMSetRenderTargets(1, &g.rtv, nullptr);
  g.ctx->RSSetViewports(1, &vp);
  g.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g.ctx->VSSetShader(g.vs, nullptr, 0);
  g.ctx->PSSetShader(g.ps, nullptr, 0);
  g.ctx->PSSetConstantBuffers(0, 1, &g.cb);
  g.ctx->PSSetSamplers(0, 1, &g.sm);
  g.ctx->RSSetState(g.rs);
}

void DrawQuad(const Tex& t, float sx, float sy, float ox, float oy, float ss) {
  if (!t.srv) return;
  CbData cb = g_cbData;
  cb.sx = sx; cb.sy = sy; cb.ox = ox; cb.oy = oy;
  cb.tw = (float)t.w; cb.th = (float)t.h;
  cb.ss = ss;
  g.ctx->UpdateSubresource(g.cb, 0, nullptr, &cb, 0, 0);
  g.ctx->PSSetShaderResources(0, 1, &t.srv);
  g.ctx->Draw(3, 0);
}

bool EnsureTex(Tex& t, int w, int h) {
  if (t.tex && t.w == w && t.h == h) return true;
  ReleaseTex(t);
  D3D11_TEXTURE2D_DESC td = {};
  td.Width = (UINT)w;
  td.Height = (UINT)h;
  td.MipLevels = 1;
  td.ArraySize = 1;
  td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  td.SampleDesc.Count = 1;
  td.Usage = D3D11_USAGE_DEFAULT;
  td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (FAILED(g.dev->CreateTexture2D(&td, nullptr, &t.tex)) || !t.tex) return false;
  D3D11_SHADER_RESOURCE_VIEW_DESC sd = {};
  sd.Format = td.Format;
  sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  sd.Texture2D.MipLevels = 1;
  if (FAILED(g.dev->CreateShaderResourceView(t.tex, &sd, &t.srv)) || !t.srv) return false;
  t.w = w;
  t.h = h;
  return true;
}

/** Redraw the committed pass (or the shell backdrop) at the current size. */
void RepaintLocked() {
  if (!g.ready || !g.rtv) return;
  SetupDraw();
  if (g.display >= 0 && g.slots[g.display].srv) {
    DrawQuad(g.slots[g.display], (float)g_sx, (float)g_sy, (float)g_ox, (float)g_oy,
             (float)g.displaySS);
  } else {
    float bg[4] = {0.03137f, 0.03529f, 0.04314f, 1.0f};
    g.ctx->ClearRenderTargetView(g.rtv, bg);
  }
  g.sc->Present(1, 0);
}

bool CreateDevice() {
  D3D_FEATURE_LEVEL fl = {};
  const D3D_FEATURE_LEVEL want[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1};
  /* BGRA_SUPPORT is mandatory for DirectComposition to accept this device. */
  HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, want, 2,
                                 D3D11_SDK_VERSION, &g.dev, &fl, &g.ctx);
  if (FAILED(hr) || !g.dev) {
    g_why = Hr("D3D11CreateDevice", hr);
    return false;
  }

  ID3DBlob* blob = nullptr;
  ID3DBlob* err = nullptr;
  HRESULT hrv = D3DCompile(kHLSL, strlen(kHLSL), nullptr, nullptr, nullptr, "vsmain",
                           "vs_4_0", 0, 0, &blob, &err);
  if (FAILED(hrv) || !blob) { g_why = Hr("compile vs", hrv); return false; }
  g.dev->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.vs);
  blob->Release();
  blob = nullptr;
  HRESULT hrp = D3DCompile(kHLSL, strlen(kHLSL), nullptr, nullptr, nullptr, "psmain",
                           "ps_4_0", 0, 0, &blob, &err);
  if (FAILED(hrp) || !blob) { g_why = Hr("compile ps", hrp); return false; }
  g.dev->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &g.ps);
  blob->Release();

  D3D11_RASTERIZER_DESC rd = {};
  rd.FillMode = D3D11_FILL_SOLID;
  rd.CullMode = D3D11_CULL_NONE;
  g.dev->CreateRasterizerState(&rd, &g.rs);

  D3D11_SAMPLER_DESC sd = {};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  g.dev->CreateSamplerState(&sd, &g.sm);

  D3D11_BUFFER_DESC bd = {};
  bd.ByteWidth = sizeof(CbData);
  bd.Usage = D3D11_USAGE_DEFAULT;
  bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  g.dev->CreateBuffer(&bd, nullptr, &g.cb);

  if (!g.vs || !g.ps || !g.rs || !g.sm || !g.cb) { g_why = "shader objects"; return false; }
  return true;
}

bool CreateSwapchain() {
  IDXGIDevice* dxdev = nullptr;
  if (FAILED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev)) || !dxdev) {
    g_why = "IDXGIDevice";
    return false;
  }
  IDXGIAdapter* adapter = nullptr;
  dxdev->GetAdapter(&adapter);
  IDXGIFactory2* factory = nullptr;
  if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
  if (!factory) {
    if (adapter) adapter->Release();
    dxdev->Release();
    g_why = "IDXGIFactory2";
    return false;
  }

  RECT rc = {0, 0, 1, 1};
  GetClientRect(g_host, &rc);
  DXGI_SWAP_CHAIN_DESC1 d = {};
  d.Width = (UINT)(rc.right > 0 ? rc.right : 1);
  d.Height = (UINT)(rc.bottom > 0 ? rc.bottom : 1);
  d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // what the composition engine wants
  d.SampleDesc.Count = 1;
  d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  d.BufferCount = 2;
  d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  d.Scaling = DXGI_SCALING_STRETCH;
  d.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  HRESULT hr = factory->CreateSwapChainForComposition(g.dev, &d, nullptr, &g.sc);
  factory->Release();
  if (adapter) adapter->Release();
  dxdev->Release();
  if (FAILED(hr) || !g.sc) { g_why = Hr("CreateSwapChainForComposition", hr); return false; }

  IDXGISwapChain2* sc2 = nullptr;
  if (SUCCEEDED(g.sc->QueryInterface(__uuidof(IDXGISwapChain2), (void**)&sc2)) && sc2) {
    sc2->SetMaximumFrameLatency(1);
    sc2->Release();
  }
  ID3D11Texture2D* bb = nullptr;
  if (FAILED(g.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) {
    g_why = "swapchain GetBuffer";
    return false;
  }
  g.dev->CreateRenderTargetView(bb, nullptr, &g.rtv);
  bb->Release();
  if (!g.rtv) { g_why = "CreateRenderTargetView"; return false; }
  g.w = (int)d.Width;
  g.h = (int)d.Height;
  return true;
}

bool CreateTree() {
  IDXGIDevice* dxdev = nullptr;
  if (FAILED(g.dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev)) || !dxdev) {
    g_why = "dxgi for dcomp";
    return false;
  }
  HRESULT hr = DCompositionCreateDevice(dxdev, __uuidof(IDCompositionDevice), (void**)&g.dcomp);
  dxdev->Release();
  if (FAILED(hr) || !g.dcomp) { g_why = Hr("DCompositionCreateDevice", hr); return false; }

  hr = g.dcomp->CreateTargetForHwnd(g_host, TRUE, &g.target);
  if (FAILED(hr) || !g.target) { g_why = Hr("CreateTargetForHwnd", hr); return false; }
  hr = g.dcomp->CreateVisual(&g.root);
  if (FAILED(hr) || !g.root) { g_why = Hr("CreateVisual root", hr); return false; }
  hr = g.target->SetRoot(g.root);
  if (FAILED(hr)) { g_why = Hr("SetRoot", hr); return false; }
  hr = g.dcomp->CreateVisual(&g.canvasVis);
  if (FAILED(hr) || !g.canvasVis) { g_why = Hr("CreateVisual canvas", hr); return false; }
  hr = g.canvasVis->SetContent(g.sc);
  if (FAILED(hr)) { g_why = Hr("SetContent", hr); return false; }
  /* Inserted first, so the WebView2 visual added later lands above it. */
  hr = g.root->AddVisual(g.canvasVis, FALSE, nullptr);
  if (FAILED(hr)) { g_why = Hr("AddVisual canvas", hr); return false; }
  g.dcomp->Commit();
  return true;
}

void ResizeSwapchain(int w, int h) {
  if (!g.sc || w <= 0 || h <= 0) return;
  if (w == g.w && h == g.h && g.rtv) return;
  if (g.rtv) { g.rtv->Release(); g.rtv = nullptr; }
  if (SUCCEEDED(g.sc->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0))) {
    ID3D11Texture2D* bb = nullptr;
    if (SUCCEEDED(g.sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) && bb) {
      g.dev->CreateRenderTargetView(bb, nullptr, &g.rtv);
      bb->Release();
    }
  }
  g.w = w;
  g.h = h;
}

}  // namespace

namespace ncd {

bool Init(HWND host) {
  g_host = host;
  DcLog("init-start");
  if (!CreateDevice()) { DcLog(g_why.c_str()); return false; }
  if (!CreateSwapchain()) { DcLog(g_why.c_str()); return false; }
  if (!CreateTree()) { DcLog(g_why.c_str()); return false; }

  {
    std::lock_guard<std::mutex> lk(g_mu);
    SetupDraw();
    float bg[4] = {0.03137f, 0.03529f, 0.04314f, 1.0f};
    g.ctx->ClearRenderTargetView(g.rtv, bg);
    g.sc->Present(1, 0);
    g.ready = true;
  }
  DcLog("init-ok");
  return true;
}

bool Ready() { return g.ready; }

HWND Hwnd() { return nullptr; }   // no child window: it is a visual

void SetBounds(int x, int y, int w, int h) {
  (void)x; (void)y;   // the visual is the whole client area, by definition
  if (!g.ready) return;
  std::lock_guard<std::mutex> lk(g_mu);
  ResizeSwapchain(w, h);
  RepaintLocked();
}

void BeginPass(int passW, int passH, int ss, int cw, int ch) {
  (void)cw; (void)ch;
  if (!g.ready || passW <= 0 || passH <= 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  const int slot = g.display < 0 ? 0 : 1 - g.display;
  if (!EnsureTex(g.slots[slot], passW, passH)) {
    DcLog("EnsureTex failed");
    return;
  }
  if (ss < 1) ss = 1;
  if (ss > 4) ss = 4;
  g.pendingSS = ss;
}

void UploadTile(const uint8_t* frame, int frameW, int frameH,
                int x, int y, int w, int h) {
  if (!g.ready || !frame) return;
  std::lock_guard<std::mutex> lk(g_mu);
  const int slot = g.display < 0 ? 0 : 1 - g.display;
  Tex& t = g.slots[slot];
  if (!t.tex || x < 0 || y < 0 || x + w > t.w || y + h > t.h) return;
  if (x + w > frameW || y + h > frameH) return;
  D3D11_BOX box = {(UINT)x, (UINT)y, 0, (UINT)(x + w), (UINT)(y + h), 1};
  const uint8_t* src = frame + ((size_t)y * (size_t)frameW + (size_t)x) * 4;
  const double t0 = NowMs();
  g.ctx->UpdateSubresource(t.tex, 0, &box, src, (UINT)frameW * 4, 0);
  g_upMs = NowMs() - t0;
}

bool CommitPass() {
  if (!g.ready) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  const int slot = g.display < 0 ? 0 : 1 - g.display;
  if (!g.slots[slot].tex) return false;
  g.display = slot;
  g.displaySS = g.pendingSS;
  g_sx = g_sy = 1.0;
  g_ox = g_oy = 0.0;
  ++g_frames;
  const double t0 = NowMs();
  SetupDraw();
  DrawQuad(g.slots[slot], 1.0f, 1.0f, 0.0f, 0.0f, (float)g.pendingSS);
  g.sc->Present(1, 0);
  g_prMs = NowMs() - t0;
  return true;
}

bool CommitPassWithPreview(double sx, double ox, double sy, double oy) {
  if (!g.ready) return false;
  std::lock_guard<std::mutex> lk(g_mu);
  const int slot = g.display < 0 ? 0 : 1 - g.display;
  if (!g.slots[slot].tex) return false;
  g.display = slot;
  g.displaySS = g.pendingSS;
  /* The pass depicts the view as of the moment it STARTED; the gesture that
   * accumulated since then is carried here, so the first present after the
   * commit already matches what the user is looking at. */
  g_sx = sx; g_ox = ox;
  g_sy = sy; g_oy = oy;
  ++g_frames;
  const double t0 = NowMs();
  SetupDraw();
  DrawQuad(g.slots[slot], (float)g_sx, (float)g_sy, (float)g_ox, (float)g_oy,
           (float)g.displaySS);
  g.sc->Present(1, 0);
  g_prMs = NowMs() - t0;
  return true;
}

void Preview(double scale, double dx, double dy, double cx, double cy) {
  if (!g.ready || g.display < 0 || scale <= 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  const double inv = 1.0 / scale;
  const double nsx = inv, nox = cx * (1.0 - inv) - dx * inv;
  const double nsy = inv, noy = cy * (1.0 - inv) - dy * inv;
  const double sxo = g_sx, oxo = g_ox;
  const double syo = g_sy, oyo = g_oy;
  g_sx = sxo * nsx;
  g_ox = sxo * nox + oxo;
  g_sy = syo * nsy;
  g_oy = syo * noy + oyo;
  SetupDraw();
  DrawQuad(g.slots[g.display], (float)g_sx, (float)g_sy, (float)g_ox, (float)g_oy,
           (float)g.displaySS);
  g.sc->Present(1, 0);
}

void SetPreview(double sx, double ox, double sy, double oy) {
  if (!g.ready || g.display < 0) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_sx = sx; g_ox = ox;
  g_sy = sy; g_oy = oy;
  SetupDraw();
  DrawQuad(g.slots[g.display], (float)g_sx, (float)g_sy, (float)g_ox, (float)g_oy,
           (float)g.displaySS);
  g.sc->Present(1, 0);
}

void ResetPreview() {
  std::lock_guard<std::mutex> lk(g_mu);
  g_sx = g_sy = 1.0;
  g_ox = g_oy = 0.0;
}

void Present() {
  std::lock_guard<std::mutex> lk(g_mu);
  RepaintLocked();
}

IUnknown* OverlayVisual() {
  if (!g.ready || !g.dcomp) return nullptr;
  if (g.overlayVis) return g.overlayVis;
  IDCompositionVisual* v = nullptr;
  if (FAILED(g.dcomp->CreateVisual(&v)) || !v) return nullptr;
  /* Above the canvas.  WebView2 grows its own tree under this visual. */
  if (FAILED(g.root->AddVisual(v, TRUE, g.canvasVis))) { v->Release(); return nullptr; }
  g.dcomp->Commit();
  g.overlayVis = v;
  DcLog("overlay visual created");
  return g.overlayVis;
}

void CommitComposition() {
  if (g.dcomp) g.dcomp->Commit();
}

void Shutdown() {
  g.ready = false;
  std::lock_guard<std::mutex> lk(g_mu);
  for (int i = 0; i < 2; ++i) ReleaseTex(g.slots[i]);
  if (g.overlayVis) { g.overlayVis->Release(); g.overlayVis = nullptr; }
  if (g.canvasVis) { g.canvasVis->Release(); g.canvasVis = nullptr; }
  if (g.root) { g.root->Release(); g.root = nullptr; }
  if (g.target) { g.target->Release(); g.target = nullptr; }
  if (g.dcomp) { g.dcomp->Release(); g.dcomp = nullptr; }
  if (g.rtv) { g.rtv->Release(); g.rtv = nullptr; }
  if (g.sc) { g.sc->Release(); g.sc = nullptr; }
  if (g.cb) { g.cb->Release(); g.cb = nullptr; }
  if (g.sm) { g.sm->Release(); g.sm = nullptr; }
  if (g.rs) { g.rs->Release(); g.rs = nullptr; }
  if (g.ps) { g.ps->Release(); g.ps = nullptr; }
  if (g.vs) { g.vs->Release(); g.vs = nullptr; }
  if (g.ctx) { g.ctx->Release(); g.ctx = nullptr; }
  if (g.dev) { g.dev->Release(); g.dev = nullptr; }
  g.display = -1;
  g_host = nullptr;
}

double LastUploadMs() { return g_upMs; }
double LastPresentMs() { return g_prMs; }
int FrameCount() { return g_frames; }

const char* Why() { return g_why.c_str(); }

}  // namespace ncd
