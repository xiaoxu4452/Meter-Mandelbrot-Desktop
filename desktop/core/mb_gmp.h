// ------------------------------------------------------------------
// mb_gmp.h — runtime binding to the system GMP (libgmp-10.dll).
//
// Why this exists
// ---------------
// The reference orbit is the only piece of the renderer whose cost grows
// with zoom depth: at 10^300000 the anchor point needs ~1e6 bits, and the
// hand-written Mpf in mb_mpf.cpp multiplies in O(n^1.585) (Karatsuba, no
// FFT).  That is fine to ~20k bits and hopeless past ~200k.  GMP's
// multiply is FFT-based above a few thousand limbs, so the same orbit that
// costs minutes with Mpf costs a second with GMP.
//
// GMP is not vendored: the DLL ships with Git for Windows (mingw64) and is
// picked up from the executable directory first, then from the usual
// locations.  Nothing links against it at build time, so the program still
// builds and runs when the DLL is absent — the caller falls back to Mpf.
//
// The wrapper deliberately never looks inside `__mpf_struct`: the struct is
// treated as an opaque 32-byte blob (GMP's own layout on Win64 is 24 bytes)
// and every field is read through the API.  That keeps this file correct
// even if the DLL is a different GMP build than the one we measured.
// ------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>

namespace mbc {
namespace gmp {

/** True once the DLL is loaded and every entry point below is resolved. */
bool available();

/** Reason the last load attempt failed (empty when available()). */
const char* loadError();

/** Version string reported by the DLL, e.g. "6.3.0". */
const char* version();

/** Path the DLL was loaded from (empty when unavailable). */
const char* loadedPath();

/** Re-run the load (used by the diagnostics tools). */
bool reload();

/**
 * One arbitrary-precision float.  Not copyable: GMP allocates a limb array
 * per value and a silent copy would alias it.  Move is allowed and just
 * swaps the handles.
 */
class Num {
 public:
  Num() = default;
  ~Num();
  Num(const Num&) = delete;
  Num& operator=(const Num&) = delete;
  Num(Num&& o) noexcept;
  Num& operator=(Num&& o) noexcept;

  /** mpf_init2; safe to call twice (re-inits). */
  void init(int bits);
  void release();

  bool isInit() const { return p_ != nullptr; }
  int prec() const;
  void setPrec(int bits);

  void setStr(const std::string& decimal);
  void setD(double v);
  void setUi(uint64_t v);
  void setZero();

  void add(const Num& a, const Num& b);
  void sub(const Num& a, const Num& b);
  void mul(const Num& a, const Num& b);
  void sqr(const Num& a);
  void mul2exp(const Num& a, long bits);
  /** a * 2^bits for either sign of `bits` (mul_2exp / div_2exp). */
  void shiftExp(const Num& a, long bits);
  void neg(const Num& a);  int cmp(const Num& o) const;
  int cmpD(double d) const;
  double toDouble() const;
  /** value = mant * 2^exp, |mant| in [0.5, 1); returns mant. */
  double toDoubleExp(long* exp2) const;
  /** floor(log2 |v|) + 1, or 0 for zero — matches Mpf::expBits(). */
  int expBits() const;

  /** Mantissa digits only (diagnostics). */
  std::string toString(int digits) const;
  /** Plain decimal text with `digits` significant digits, e.g. "-0.74364". */
  std::string toDecimal(int digits) const;

 private:
  void* p_ = nullptr;  // opaque __mpf_struct*, 32 bytes
};

}  // namespace gmp
}  // namespace mbc
