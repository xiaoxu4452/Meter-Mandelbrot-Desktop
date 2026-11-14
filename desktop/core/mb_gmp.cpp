#include "mb_gmp.h"

#include <windows.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

namespace mbc {
namespace gmp {
namespace {

/* --- the imported surface ------------------------------------------------ */
struct Api {
  void (*init2)(void*, unsigned long) = nullptr;
  void (*clear)(void*) = nullptr;
  void (*set)(void*, const void*) = nullptr;
  int (*set_str)(void*, const char*, int) = nullptr;
  void (*set_d)(void*, double) = nullptr;
  void (*set_ui)(void*, unsigned long) = nullptr;
  unsigned long (*set_prec)(void*, unsigned long) = nullptr;
  unsigned long (*get_prec)(const void*) = nullptr;
  void (*add)(void*, const void*, const void*) = nullptr;
  void (*sub)(void*, const void*, const void*) = nullptr;
  void (*mul)(void*, const void*, const void*) = nullptr;
  void (*mul_2exp)(void*, const void*, unsigned long) = nullptr;
  void (*div_2exp)(void*, const void*, unsigned long) = nullptr;
  void (*neg)(void*, const void*) = nullptr;
  int (*cmp)(const void*, const void*) = nullptr;
  int (*cmp_d)(const void*, double) = nullptr;
  double (*get_d)(const void*) = nullptr;
  double (*get_d_2exp)(int32_t*, const void*) = nullptr;
  char* (*get_str)(char*, int32_t*, int, size_t, const void*) = nullptr;
};

Api g_api;
bool g_ok = false;
std::string g_err = "not loaded";
std::string g_path;
std::string g_version;

struct LoadState {
  std::once_flag once;
};

LoadState& loadState() {
  static LoadState s;
  return s;
}

/* Candidate locations.  The executable directory comes first so a shipped
 * copy always wins over whatever happens to be on PATH. */
std::vector<std::string> candidates() {
  std::vector<std::string> v;
  char exe[MAX_PATH * 2];
  DWORD n = GetModuleFileNameA(nullptr, exe, (DWORD)sizeof(exe));
  if (n > 0 && n < sizeof(exe)) {
    std::string dir(exe, n);
    const size_t cut = dir.find_last_of("\\/");
    if (cut != std::string::npos) dir.resize(cut + 1);
    v.push_back(dir + "libgmp-10.dll");
    v.push_back(dir + "gmp-10.dll");
    v.push_back(dir + "libgmp.dll");
  }
  v.push_back("libgmp-10.dll");
  v.push_back("C:\\Program Files\\Git\\mingw64\\bin\\libgmp-10.dll");
  v.push_back("C:\\Program Files (x86)\\Git\\mingw64\\bin\\libgmp-10.dll");
  v.push_back("C:\\msys64\\mingw64\\bin\\libgmp-10.dll");
  return v;
}

template <typename T>
void bind(HMODULE m, const char* name, T& fn, bool& ok) {
  fn = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(m, name)));
  if (!fn) ok = false;
}

bool doLoad() {
  for (const std::string& p : candidates()) {
    HMODULE m = LoadLibraryA(p.c_str());
    if (!m) continue;
    bool ok = true;
    bind(m, "__gmpf_init2", g_api.init2, ok);
    bind(m, "__gmpf_clear", g_api.clear, ok);
    bind(m, "__gmpf_set", g_api.set, ok);
    bind(m, "__gmpf_set_str", g_api.set_str, ok);
    bind(m, "__gmpf_set_d", g_api.set_d, ok);
    bind(m, "__gmpf_set_ui", g_api.set_ui, ok);
    bind(m, "__gmpf_set_prec", g_api.set_prec, ok);
    bind(m, "__gmpf_get_prec", g_api.get_prec, ok);
    bind(m, "__gmpf_add", g_api.add, ok);
    bind(m, "__gmpf_sub", g_api.sub, ok);
    bind(m, "__gmpf_mul", g_api.mul, ok);
    bind(m, "__gmpf_mul_2exp", g_api.mul_2exp, ok);
    bind(m, "__gmpf_div_2exp", g_api.div_2exp, ok);
    bind(m, "__gmpf_neg", g_api.neg, ok);
    bind(m, "__gmpf_cmp", g_api.cmp, ok);
    bind(m, "__gmpf_cmp_d", g_api.cmp_d, ok);
    bind(m, "__gmpf_get_d", g_api.get_d, ok);
    bind(m, "__gmpf_get_d_2exp", g_api.get_d_2exp, ok);
    bind(m, "__gmpf_get_str", g_api.get_str, ok);
    if (!ok) {
      FreeLibrary(m);
      g_err = "missing exports in " + p;
      continue;
    }
    g_path = p;
    /* `__gmp_version` is a *pointer* variable, not a string: read the pointer
     * out of the DLL and follow it. */
    const char** v = reinterpret_cast<const char**>(
        reinterpret_cast<void*>(GetProcAddress(m, "__gmp_version")));
    g_version = (v && *v) ? *v : "unknown";
    g_ok = true;
    g_err.clear();
    return true;
  }
  if (g_err == "not loaded") g_err = "libgmp-10.dll not found";
  return false;
}

/* The opaque `__mpf_struct` is 24 bytes on Win64 (int, int, long, padding,
 * pointer).  Allocate 32 and 16-align it so a different GMP revision with a
 * wider exponent field still fits. */
constexpr size_t kNumBytes = 32;

void* allocNum() {
  void* p = _aligned_malloc(kNumBytes, 16);
  if (p) std::memset(p, 0, kNumBytes);
  return p;
}

}  // namespace

bool available() {
  std::call_once(loadState().once, [] { doLoad(); });
  return g_ok;
}

bool reload() {
  g_ok = false;
  g_err = "not loaded";
  g_api = Api{};
  return doLoad();
}

const char* loadError() { return g_err.c_str(); }
const char* version() { return g_version.c_str(); }
const char* loadedPath() { return g_path.c_str(); }

/* --- Num ----------------------------------------------------------------- */

Num::~Num() { release(); }

Num::Num(Num&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }

Num& Num::operator=(Num&& o) noexcept {
  if (this != &o) {
    release();
    p_ = o.p_;
    o.p_ = nullptr;
  }
  return *this;
}

void Num::release() {
  if (p_ && available()) {
    g_api.clear(p_);
    _aligned_free(p_);
  }
  p_ = nullptr;
}

void Num::init(int bits) {
  if (!available()) return;
  release();
  p_ = allocNum();
  if (p_) g_api.init2(p_, (unsigned long)(bits < 64 ? 64 : bits));
}

int Num::prec() const {
  return (p_ && available()) ? (int)g_api.get_prec(p_) : 0;
}

void Num::setPrec(int bits) {
  if (p_ && available()) g_api.set_prec(p_, (unsigned long)(bits < 64 ? 64 : bits));
}

void Num::setStr(const std::string& decimal) {
  if (!p_ || !available()) return;
  g_api.set_str(p_, decimal.c_str(), 10);
}

void Num::setD(double v) {
  if (p_ && available()) g_api.set_d(p_, v);
}

void Num::setUi(uint64_t v) {
  if (p_ && available()) g_api.set_ui(p_, (unsigned long)v);
}

void Num::setZero() {
  if (p_ && available()) g_api.set_ui(p_, 0UL);
}

void Num::add(const Num& a, const Num& b) {
  if (p_ && a.p_ && b.p_) g_api.add(p_, a.p_, b.p_);
}
void Num::sub(const Num& a, const Num& b) {
  if (p_ && a.p_ && b.p_) g_api.sub(p_, a.p_, b.p_);
}
void Num::mul(const Num& a, const Num& b) {
  if (p_ && a.p_ && b.p_) g_api.mul(p_, a.p_, b.p_);
}
void Num::sqr(const Num& a) {
  if (p_ && a.p_) g_api.mul(p_, a.p_, a.p_);
}
void Num::mul2exp(const Num& a, long bits) {
  if (p_ && a.p_) g_api.mul_2exp(p_, a.p_, (unsigned long)bits);
}
void Num::shiftExp(const Num& a, long bits) {
  if (!p_ || !a.p_) return;
  if (bits >= 0) g_api.mul_2exp(p_, a.p_, (unsigned long)bits);
  else g_api.div_2exp(p_, a.p_, (unsigned long)(-bits));
}
void Num::neg(const Num& a) {
  if (p_ && a.p_) g_api.neg(p_, a.p_);
}

int Num::cmp(const Num& o) const {
  if (!p_ || !o.p_) return 0;
  return g_api.cmp(p_, o.p_);
}
int Num::cmpD(double d) const {
  if (!p_) return 0;
  return g_api.cmp_d(p_, d);
}
double Num::toDouble() const {
  if (!p_) return 0.0;
  return g_api.get_d(p_);
}

double Num::toDoubleExp(long* exp2) const {
  if (!p_) {
    if (exp2) *exp2 = 0;
    return 0.0;
  }
  int32_t e = 0;
  double m = g_api.get_d_2exp(&e, p_);
  if (exp2) *exp2 = (long)e;
  return m;
}

int Num::expBits() const {
  if (!p_) return 0;
  if (g_api.cmp_d(p_, 0.0) == 0) return 0;
  int32_t e = 0;
  g_api.get_d_2exp(&e, p_);
  return (int)e;
}

std::string Num::toDecimal(int digits) const {
  if (!p_ || !available()) return std::string();
  if (g_api.cmp_d(p_, 0.0) == 0) return "0";
  const int nd = digits > 0 ? digits : 1;
  std::vector<char> buf((size_t)nd + 8, '\0');
  int32_t e = 0;
  g_api.get_str(buf.data(), &e, 10, (size_t)nd, p_);
  const char* s = buf.data();
  std::string out;
  if (*s == '-') {
    out = "-";
    ++s;
  }
  const size_t n = std::strlen(s);
  if (e <= 0) {
    out += "0.";
    out.append((size_t)(-e), '0');
    out.append(s, n);
  } else if ((size_t)e >= n) {
    out.append(s, n);
    out.append((size_t)e - n, '0');
  } else {
    out.append(s, (size_t)e);
    out += '.';
    out.append(s + e, n - (size_t)e);
  }
  return out;
}

std::string Num::toString(int digits) const {
  if (!p_ || !available()) return std::string();
  const int nd = digits > 0 ? digits : 1;
  std::vector<char> buf((size_t)nd + 4, '\0');
  int32_t exp10 = 0;
  g_api.get_str(buf.data(), &exp10, 10, (size_t)nd, p_);
  return std::string(buf.data());
}

}  // namespace gmp
}  // namespace mbc
