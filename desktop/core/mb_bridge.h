// ------------------------------------------------------------------
// mb_bridge.h - Boost <-> MPFR reference-orbit bridge.
//
// The web engine (js/wgpu/ref-orbit.js) builds the reference orbit with
// gmp-wasm (real MPFR) and exports one decimal string per Float value via
// ctx.Float(...).toString(10).  The desktop engine has two independent
// arbitrary-precision backends: mbc::Mpf (hand-written GMP mpf_t semantics)
// and Boost.Multiprecision cpp_bin_float (the gpu/mb_hp adaptive tiers).
// This module puts both sides on ONE contract:
//
//   1. LimbView - little-endian 32-bit limbs plus a binary exponent:
//      |v| = sum(data[i] * 2^(32*i)) * 2^expBits, neg = sign bit.  This is
//      exactly the raw view exposed by Mpf::limbs()/limbExponent()/isNeg(),
//      so converting to and from Boost never needs a lossy double hop.
//   2. A decimal string per value, formatted with
//      number::str(decimalDigits, std::ios_base::scientific), matching the
//      web side's one-value-per-element toString(10) export.  A reference
//      orbit is therefore two strings per step: Re(z_n), Im(z_n), z_0 = 0.
//
// Precision rule: this is the Boost-side counterpart of the rule recorded
// in `07_反汇编与伪代码` for `initcanvas_hfloat`: depth + 0x40, i.e. 64
// guard bits above the physical bit depth of the view.  bridgeTierForBits()
// applies the same clamp as mbhp::bitsForDepth() and then picks the smallest
// compile-time cpp_bin_float tier (gpu/mb_hp.h: 50/100/200/350/700 digits)
// that still holds the view.  Limb conversion and the JSON referee use a
// fixed 800-digit judge type (mb_boost.cpp's cpp_bin_float<800>, ~2659 bits)
// so a 2304-bit Mpf limb array round-trips bit-for-bit.
// ------------------------------------------------------------------
#pragma once

#include "mb_hp.h"  // Hp64..Hp1024: the shared adaptive tier table

#include <boost/multiprecision/cpp_bin_float.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mbc {

// Non-owning view of a signed dyadic value.  limbs are little-endian 32-bit
// words: |v| = sum(data[i] * 2^(32*i)) * 2^expBits.
struct LimbView {
  const uint32_t* data;
  size_t count;
  long expBits;
  bool neg;
};

// Decimal digits of the five adaptive tiers; mirrors gpu/mb_hp.h.
constexpr unsigned kBridgeTierDigits[5] = {50u, 100u, 200u, 350u, 700u};

// Judge / referee type: 800 decimal digits ~= 2659 bits, same as mb_boost.cpp.
// Wide enough to hold every Mpf/Big limb array in this project exactly.
using BridgeFloat =
    boost::multiprecision::number<boost::multiprecision::cpp_bin_float<800>>;

// depth + 0x40 (64 guard bits), then smallest tier index in 0..4.
// `depthBits` is the physical bit depth of the view.
int bridgeTierForBits(int depthBits);
// Decimal flavour: depthBits = decimalDigits * log2(10).
int bridgeTierForDecimalDigits(int decimalDigits);

// Exact, double-free conversions.  limbViewFromBoost() fills `storage` and
// returns a non-owning view into it; the caller keeps `storage` alive.  The
// returned view is normalized: the magnitude is odd and has no leading zero
// limb, so a normalized LimbView round-trips limb-for-limb, exponent-for-
// exponent and sign-for-sign.
BridgeFloat boostFromLimbView(const LimbView& view);
LimbView limbViewFromBoost(const BridgeFloat& v,
                           std::vector<uint32_t>& storage);

// Recompute z^2 + c from z = 0.  `out` receives two decimal strings per
// step: out[2*n] = Re(z_n), out[2*n+1] = Im(z_n), formatted with
// str(decimalDigits, scientific).  Iteration stops early when |z|^2 > 1e20;
// the escape step is appended to `log` when it is non-null.  Returns false
// for invalid arguments.  The Boost variant selects the smallest adaptive
// tier from decimalDigits; the Mpf variant uses a wide working precision so
// the decimal output is never the limiting factor in a step-by-step diff.
bool boostReferenceOrbit(const std::string& centerRe,
                         const std::string& centerIm, int maxIter,
                         int decimalDigits, std::vector<std::string>& out,
                         std::string* log = nullptr);
bool mpfReferenceOrbit(const std::string& centerRe,
                       const std::string& centerIm, int maxIter,
                       int decimalDigits, std::vector<std::string>& out,
                       std::string* log = nullptr);

// Validate an external MPFR (web gmp-wasm) decimal orbit against a fresh
// Boost recomputation of the same {centerRe, centerIm, maxIter, digits}.
// Exact string equality passes directly; otherwise the relative error must
// stay below 10^-(digits-6).  Writes step count, worst relative error (via
// Boost log10) and mismatched step numbers into `log`.
bool bridgeCheckJson(const std::string& json, std::string& log);

// (a) limb <-> Boost round-trip at several widths / signs / exponents;
// (b) Boost orbit vs Mpf orbit, first 64 steps, step-by-step;
// (c) bridgeCheckJson on an embedded MPFR-style decimal orbit.
bool bridgeSelfTest(std::string& log);

}  // namespace mbc
