// ------------------------------------------------------------------
// mb_bridge.cpp - see mb_bridge.h.
// ------------------------------------------------------------------
#include "mb_bridge.h"

#include "mb_mpf.h"

#include <boost/multiprecision/cpp_int.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <ios>
#include <iterator>
#include <string>
#include <vector>

namespace mbc {
namespace {

constexpr double kLog2_10 = 3.321928094887362;

double relLog10(const BridgeFloat& got, const BridgeFloat& ref) {
  if (got == ref) return -999.0;
  const BridgeFloat den =
      boost::multiprecision::abs(ref) + BridgeFloat("1e-300");
  const double e = static_cast<double>(boost::multiprecision::log10(
      boost::multiprecision::abs(got - ref) / den));
  return std::max(-999.0, std::min(999.0, e));
}

std::string mpfToDecimal(const Mpf& m, int decimalDigits) {
  const std::vector<uint32_t>& d = m.limbs();
  const LimbView view{d.data(), d.size(), m.limbExponent(), m.isNeg()};
  return boostFromLimbView(view).str(decimalDigits,
                                     std::ios_base::scientific);
}

// One orbit at a fixed compile-time precision.  cpp_bin_float<N> stores the
// mantissa inline, so a step allocates nothing.  Two strings are emitted per
// iteration (real, imaginary); the escape test uses the full-precision |z|^2.
template <class BF>
void computeBoostOrbit(const std::string& sre, const std::string& sim,
                       int maxIter, int decimalDigits,
                       std::vector<std::string>& out, int& escapeStep) {
  BF cr(sre), ci(sim);
  BF zr(0), zi(0);
  const BF bail2("1e20");
  out.clear();
  out.reserve(static_cast<size_t>(maxIter) * 2);
  escapeStep = -1;
  for (int n = 0; n < maxIter; ++n) {
    out.push_back(zr.str(decimalDigits, std::ios_base::scientific));
    out.push_back(zi.str(decimalDigits, std::ios_base::scientific));
    const BF rr = zr * zr, ii = zi * zi;
    if (rr + ii > bail2) {
      escapeStep = n;
      break;
    }
    const BF cross = zr * zi;
    zr = rr - ii + cr;
    zi = cross + cross + ci;
  }
}

void appendEscape(std::string* log, const char* kind, int tier, int digits,
                  size_t steps, int escapeStep) {
  if (!log) return;
  char line[192];
  std::snprintf(line, sizeof line,
                "[bridge] %s orbit: tier=%d digits=%d steps=%zu escape=%d\n",
                kind, tier, digits, steps, escapeStep);
  *log += line;
}

// ------------------------------------------------------------------
// Minimal JSON reader for the fixed bridge contract.  Only string, integer
// and string-array values are interpreted; unknown members are skipped.
// A full Boost.JSON dependency is deliberately avoided (header weight and
// link requirements) - the payload is a flat object of known fields.
// ------------------------------------------------------------------
class JsonReader {
 public:
  explicit JsonReader(const std::string& s) : s_(s) {}

  bool parse(std::string& centerRe, std::string& centerIm, int& maxIter,
             int& digits, std::vector<std::string>& orbit,
             std::string& err) {
    skipWs();
    if (!eat('{')) {
      err = "expected '{'";
      return false;
    }
    skipWs();
    if (eat('}')) {
      err = "empty object";
      return false;
    }
    bool haveRe = false, haveIm = false, haveIter = false;
    bool haveDigits = false, haveOrbit = false;
    for (;;) {
      skipWs();
      std::string key;
      if (!readString(key)) {
        err = "expected key string";
        return false;
      }
      skipWs();
      if (!eat(':')) {
        err = "expected ':'";
        return false;
      }
      skipWs();
      if (key == "centerRe") {
        if (!readString(centerRe)) {
          err = "bad centerRe";
          return false;
        }
        haveRe = true;
      } else if (key == "centerIm") {
        if (!readString(centerIm)) {
          err = "bad centerIm";
          return false;
        }
        haveIm = true;
      } else if (key == "maxIter") {
        if (!readNumber(maxIter)) {
          err = "bad maxIter";
          return false;
        }
        haveIter = true;
      } else if (key == "digits") {
        if (!readNumber(digits)) {
          err = "bad digits";
          return false;
        }
        haveDigits = true;
      } else if (key == "orbit") {
        if (!readOrbit(orbit)) {
          err = "bad orbit array";
          return false;
        }
        haveOrbit = true;
      } else if (!skipValue()) {
        err = "bad value";
        return false;
      }
      skipWs();
      if (eat('}')) break;
      if (!eat(',')) {
        err = "expected ',' or '}'";
        return false;
      }
    }
    if (!haveRe || !haveIm || !haveIter || !haveDigits || !haveOrbit) {
      err = "missing field(s)";
      return false;
    }
    if (maxIter <= 0 || digits <= 0 || orbit.empty()) {
      err = "non-positive maxIter/digits or empty orbit";
      return false;
    }
    return true;
  }

 private:
  const std::string& s_;
  size_t p_ = 0;

  void skipWs() {
    while (p_ < s_.size()) {
      const char c = s_[p_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        ++p_;
      else
        break;
    }
  }

  bool eat(char c) {
    if (p_ < s_.size() && s_[p_] == c) {
      ++p_;
      return true;
    }
    return false;
  }

  bool readHex4(unsigned& out) {
    out = 0;
    for (int i = 0; i < 4; ++i) {
      if (p_ >= s_.size()) return false;
      const char c = s_[p_++];
      unsigned d;
      if (c >= '0' && c <= '9')
        d = static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f')
        d = static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F')
        d = static_cast<unsigned>(c - 'A' + 10);
      else
        return false;
      out = (out << 4) | d;
    }
    return true;
  }

  bool readString(std::string& out) {
    if (!eat('"')) return false;
    out.clear();
    while (p_ < s_.size()) {
      const char c = s_[p_++];
      if (c == '"') return true;
      if (c == '\\') {
        if (p_ >= s_.size()) return false;
        const char e = s_[p_++];
        switch (e) {
          case '"': out += '"'; break;
          case '\\': out += '\\'; break;
          case '/': out += '/'; break;
          case 'b': out += '\b'; break;
          case 'f': out += '\f'; break;
          case 'n': out += '\n'; break;
          case 'r': out += '\r'; break;
          case 't': out += '\t'; break;
          case 'u': {
            unsigned u = 0;
            if (!readHex4(u)) return false;
            out += static_cast<char>(u & 0x7fu);  // contract is ASCII decimal
            break;
          }
          default: return false;
        }
      } else {
        out += c;
      }
    }
    return false;
  }

  bool readNumber(int& out) {
    const size_t begin = p_;
    if (p_ < s_.size() && (s_[p_] == '-' || s_[p_] == '+')) ++p_;
    while (p_ < s_.size()) {
      const char c = s_[p_];
      if ((c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' ||
          c == '+' || c == '-')
        ++p_;
      else
        break;
    }
    if (p_ == begin) return false;
    out = static_cast<int>(std::atof(s_.substr(begin, p_ - begin).c_str()));
    return true;
  }

  bool readOrbit(std::vector<std::string>& out) {
    if (!eat('[')) return false;
    skipWs();
    if (eat(']')) return true;
    for (;;) {
      skipWs();
      std::string v;
      if (!readString(v)) return false;
      out.push_back(v);
      skipWs();
      if (eat(']')) return true;
      if (!eat(',')) return false;
    }
  }

  bool skipValue() {
    skipWs();
    if (p_ >= s_.size()) return false;
    const char c = s_[p_];
    if (c == '"') {
      std::string tmp;
      return readString(tmp);
    }
    if (c == '{' || c == '[') {
      const char open = c;
      const char close = (c == '{') ? '}' : ']';
      ++p_;
      int depth = 1;
      while (p_ < s_.size() && depth > 0) {
        const char d = s_[p_++];
        if (d == '"') {
          while (p_ < s_.size()) {
            const char e = s_[p_++];
            if (e == '\\') {
              if (p_ < s_.size()) ++p_;
            } else if (e == '"') {
              break;
            }
          }
        } else if (d == open) {
          ++depth;
        } else if (d == close) {
          --depth;
        }
      }
      return depth == 0;
    }
    const size_t begin = p_;
    while (p_ < s_.size()) {
      const char d = s_[p_];
      if (d == ',' || d == '}' || d == ']' || d == ' ' || d == '\t' ||
          d == '\r' || d == '\n')
        break;
      ++p_;
    }
    return p_ > begin;
  }
};

// ------------------------------------------------------------------
// Self-test (a): limb <-> Boost round-trip, bit for bit, at several widths,
// signs and exponents.  A trailing zero limb must not change the value.
// ------------------------------------------------------------------
bool checkLimbRoundTrip(std::string& log) {
  struct Case {
    int nlimbs;
    long exp;
    bool neg;
  };
  static const Case cases[] = {
      {1, 0, false},      {1, -2304, true},   {3, 37, false},
      {3, -1000, true},   {20, 128, false},   {20, -2048, true},
      {72, -2304, false}, {72, -2304, true},  {80, 11, false},
      {80, -1744, true},
  };
  const int total = static_cast<int>(sizeof(cases) / sizeof(cases[0]));
  bool ok = true;
  int passed = 0;
  for (const Case& c : cases) {
    std::vector<uint32_t> limbs(static_cast<size_t>(c.nlimbs));
    for (int i = 0; i < c.nlimbs; ++i) {
      limbs[static_cast<size_t>(i)] =
          0x9e3779b9u ^
          static_cast<uint32_t>(static_cast<uint64_t>(i) * 2654435761u);
    }
    limbs[0] |= 1u;                                   // odd mantissa
    limbs[static_cast<size_t>(c.nlimbs - 1)] |= 0x80000000u;  // no high zero
    const LimbView in{limbs.data(), limbs.size(), c.exp, c.neg};
    const BridgeFloat bf = boostFromLimbView(in);
    std::vector<uint32_t> storage;
    const LimbView outv = limbViewFromBoost(bf, storage);
    const bool same =
        outv.count == in.count && outv.expBits == in.expBits &&
        outv.neg == in.neg && outv.data != nullptr &&
        std::equal(in.data, in.data + in.count, outv.data);

    std::vector<uint32_t> padded(limbs);
    padded.push_back(0u);  // non-normalized view, same numeric value
    const LimbView pv{padded.data(), padded.size(), c.exp, c.neg};
    const bool valueOk = (boostFromLimbView(pv) == bf);

    if (same && valueOk) ++passed;
    else ok = false;
    const int tier = bridgeTierForBits(static_cast<int>(in.count * 32));
    char line[192];
    std::snprintf(line, sizeof line,
                  "[bridge] limb<->boost n=%2d exp=%6ld neg=%d tier=%d %s\n",
                  c.nlimbs, c.exp, static_cast<int>(c.neg), tier,
                  (same && valueOk) ? "PASS" : "FAIL");
    log += line;
  }
  char line[128];
  std::snprintf(line, sizeof line,
                "[bridge] limb round-trip: %d/%d cases -> %s\n", passed,
                total, ok ? "PASS" : "FAIL");
  log += line;
  return ok;
}

// ------------------------------------------------------------------
// Self-test (b): Boost orbit vs Mpf orbit, first 64 steps, value by value.
// Exact string equality counts as a pass; otherwise the relative error must
// stay below 10^-(digits-6), the same criterion as bridgeCheckJson.
// ------------------------------------------------------------------
bool checkBoostVsMpf(std::string& log) {
  const std::string cr = "-0.743643887037158704752191506114774";
  const std::string ci = "0.131825904205311970493132056385139";
  const int steps = 64;
  const int digits = 100;
  std::vector<std::string> bo, mo;
  const bool bok = boostReferenceOrbit(cr, ci, steps, digits, bo);
  const bool mok = mpfReferenceOrbit(cr, ci, steps, digits, mo);
  const size_t n = std::min(bo.size(), mo.size());
  const double threshold = -static_cast<double>(digits - 6);
  int exact = 0;
  double worst = -999.0;
  int worstAt = -1;
  bool ok = bok && mok && (bo.size() == mo.size());
  try {
    for (size_t k = 0; k < n; ++k) {
      if (bo[k] == mo[k]) {
        ++exact;
        continue;
      }
      const double rel = relLog10(BridgeFloat(mo[k]), BridgeFloat(bo[k]));
      if (rel > worst) {
        worst = rel;
        worstAt = static_cast<int>(k / 2);
      }
      if (!(rel < threshold)) ok = false;
    }
  } catch (const std::exception&) {
    ok = false;
  }
  char line[256];
  if (worst < -900.0) {
    std::snprintf(line, sizeof line,
                  "[bridge] boost-vs-mpf: steps=%zu values=%zu exact=%d "
                  "worst=exact threshold=1e%.1f -> %s\n",
                  n / 2, n, exact, threshold, ok ? "PASS" : "FAIL");
  } else {
    std::snprintf(line, sizeof line,
                  "[bridge] boost-vs-mpf: steps=%zu values=%zu exact=%d "
                  "worst=1e%.1f at step=%d threshold=1e%.1f -> %s\n",
                  n / 2, n, exact, worst, worstAt, threshold,
                  ok ? "PASS" : "FAIL");
  }
  log += line;
  return ok;
}

// ------------------------------------------------------------------
// Self-test (c): bridgeCheckJson on an embedded MPFR-style decimal orbit.
// The eight steps below were produced once with boostReferenceOrbit() and
// are frozen here, exactly like a gmp-wasm ctx.Float(...).toString(10)
// export from js/wgpu/ref-orbit.js.  A raw string literal keeps the payload
// byte-identical to the web side's JSON with no quote escaping at all.
// ------------------------------------------------------------------
bool checkEmbeddedMpfr(std::string& log) {
  static const char kEmbeddedOrbit[] = R"__bridge__({"centerRe":"-0.743643887037158704752191506114774","centerIm":"0.131825904205311970493132056385139","maxIter":8,"digits":40,"orbit":["0.0000000000000000000000000000000000000000e+00","0.0000000000000000000000000000000000000000e+00","-7.4364388703715870475219150611477400000000e-01","1.3182590420531197049313205638513900000000e-01","-2.0801572532897233674191053350123641926472e-01","-6.4237151425540668783263738380649987451473e-02","-7.0449975667628808295497125160942004749124e-01","1.5855057949901371358121113151077173996921e-01","-2.7246226613968265712335298666282888843767e-01","-9.1571785150567291345677701476719421118664e-02","-6.7779359240284909845616869642670438923732e-01","1.8172561639847138179890320309482949215429e-01","-3.1726393279020353642710082689608831323929e-01","-1.1451901253537207332926347553942459130693e-01","-6.5610208821972862410017626244366527970269e-01","2.0449140879775749401518198295161874603069e-01"]})__bridge__";
  return bridgeCheckJson(kEmbeddedOrbit, log);
}

}  // namespace

int bridgeTierForBits(int depthBits) {
  const double want = static_cast<double>(depthBits) + 64.0;  // + 0x40
  if (!(want > 0.0)) return 0;
  if (want <= 160.0) return 0;
  if (want <= 330.0) return 1;
  if (want <= 660.0) return 2;
  if (want <= 1160.0) return 3;
  return 4;
}

int bridgeTierForDecimalDigits(int decimalDigits) {
  const int depth =
      static_cast<int>(static_cast<double>(decimalDigits) * kLog2_10) + 1;
  return bridgeTierForBits(depth);
}

BridgeFloat boostFromLimbView(const LimbView& view) {
  BridgeFloat acc = 0;
  const BridgeFloat two32 = BridgeFloat(4294967296ULL);
  for (size_t i = view.count; i-- > 0;) {
    acc *= two32;
    acc += view.data[i];
  }
  if (view.expBits != 0)
    acc = boost::multiprecision::ldexp(acc, static_cast<int>(view.expBits));
  return view.neg ? -acc : acc;
}

LimbView limbViewFromBoost(const BridgeFloat& v,
                           std::vector<uint32_t>& storage) {
  storage.clear();
  const auto& be = v.backend();
  boost::multiprecision::cpp_int M(be.bits());
  if (M == 0) return LimbView{nullptr, 0, 0, false};

  // cpp_bin_float normalizes its mantissa to bit_count bits with the top bit
  // set, so |v| = M * 2^(exponent - (bit_count - 1)).  Strip every factor of
  // two from M to obtain the unique odd-mantissa LimbView.
  const long bit_count =
      static_cast<long>(BridgeFloat::backend_type::bit_count);
  long E = static_cast<long>(be.exponent()) - (bit_count - 1);
  const int t = static_cast<int>(boost::multiprecision::lsb(M));
  M >>= t;
  E += t;
  boost::multiprecision::export_bits(M, std::back_inserter(storage), 32,
                                     false);
  return LimbView{storage.data(), storage.size(), E,
                  static_cast<bool>(be.sign())};
}

bool boostReferenceOrbit(const std::string& centerRe,
                         const std::string& centerIm, int maxIter,
                         int decimalDigits, std::vector<std::string>& out,
                         std::string* log) {
  if (maxIter <= 0 || decimalDigits <= 0) {
    if (log) *log += "[bridge] boost orbit: invalid arguments\n";
    return false;
  }
  const int tier = bridgeTierForDecimalDigits(decimalDigits);
  int escapeStep = -1;
  try {
    switch (tier) {
      case 0:
        computeBoostOrbit<mbhp::Hp64>(centerRe, centerIm, maxIter,
                                      decimalDigits, out, escapeStep);
        break;
      case 1:
        computeBoostOrbit<mbhp::Hp128>(centerRe, centerIm, maxIter,
                                       decimalDigits, out, escapeStep);
        break;
      case 2:
        computeBoostOrbit<mbhp::Hp256>(centerRe, centerIm, maxIter,
                                       decimalDigits, out, escapeStep);
        break;
      case 3:
        computeBoostOrbit<mbhp::Hp512>(centerRe, centerIm, maxIter,
                                       decimalDigits, out, escapeStep);
        break;
      default:
        computeBoostOrbit<mbhp::Hp1024>(centerRe, centerIm, maxIter,
                                        decimalDigits, out, escapeStep);
        break;
    }
  } catch (const std::exception& e) {
    if (log) {
      *log += "[bridge] boost orbit error: ";
      *log += e.what();
      *log += "\n";
    }
    return false;
  }
  appendEscape(log, "boost", tier, decimalDigits, out.size() / 2,
               escapeStep);
  return true;
}

bool mpfReferenceOrbit(const std::string& centerRe,
                       const std::string& centerIm, int maxIter,
                       int decimalDigits, std::vector<std::string>& out,
                       std::string* log) {
  if (maxIter <= 0 || decimalDigits <= 0) {
    if (log) *log += "[bridge] mpf orbit: invalid arguments\n";
    return false;
  }
  // Wide working precision: 64 guard bits plus headroom for 64 iterations of
  // the z^2 map, so the decimal output is never the limiting factor.
  const int bits = std::max(1024, decimalDigits * 4 + 256);
  Mpf cr(bits), ci(bits), zr(bits), zi(bits), rr(bits), ii(bits),
      cross(bits), t(bits), t2(bits), m2(bits);
  try {
    cr.setStr(centerRe);
    ci.setStr(centerIm);
  } catch (const std::exception& e) {
    if (log) {
      *log += "[bridge] mpf orbit error: ";
      *log += e.what();
      *log += "\n";
    }
    return false;
  }
  zr.setZero();
  zi.setZero();
  out.clear();
  out.reserve(static_cast<size_t>(maxIter) * 2);
  const BridgeFloat bail2("1e20");
  int escapeStep = -1;
  for (int n = 0; n < maxIter; ++n) {
    out.push_back(mpfToDecimal(zr, decimalDigits));
    out.push_back(mpfToDecimal(zi, decimalDigits));
    Mpf::mul(rr, zr, zr);
    Mpf::mul(ii, zi, zi);
    Mpf::add(m2, rr, ii);
    const std::vector<uint32_t>& ml = m2.limbs();
    const BridgeFloat m2b = boostFromLimbView(
        LimbView{ml.data(), ml.size(), m2.limbExponent(), m2.isNeg()});
    if (m2b > bail2) {
      escapeStep = n;
      break;
    }
    Mpf::mul(cross, zr, zi);
    Mpf::sub(t, rr, ii);
    Mpf::add(zr, t, cr);
    Mpf::mul2exp(t2, cross, 1);
    Mpf::add(zi, t2, ci);
  }
  appendEscape(log, "mpf", -1, decimalDigits, out.size() / 2, escapeStep);
  return true;
}

bool bridgeCheckJson(const std::string& json, std::string& log) {
  JsonReader reader(json);
  std::string cr, ci, err;
  int maxIter = 0, digits = 0;
  std::vector<std::string> orbit;
  if (!reader.parse(cr, ci, maxIter, digits, orbit, err)) {
    log += "[bridge] JSON parse error: " + err + "\n";
    return false;
  }

  std::vector<std::string> calc;
  if (!boostReferenceOrbit(cr, ci, maxIter, digits, calc, &log)) {
    log += "[bridge] recomputation failed\n";
    return false;
  }

  const size_t n = std::min(orbit.size(), calc.size());
  const double threshold = -static_cast<double>(digits - 6);
  double worst = -999.0;
  int worstAt = -1;
  int exact = 0;
  std::vector<int> badSteps;
  try {
    for (size_t k = 0; k < n; ++k) {
      if (orbit[k] == calc[k]) {
        ++exact;
        continue;
      }
      const BridgeFloat got(orbit[k]);
      const BridgeFloat ref(calc[k]);
      const double rel = relLog10(got, ref);
      if (rel > worst) {
        worst = rel;
        worstAt = static_cast<int>(k / 2);
      }
      if (!(rel < threshold)) {
        const int step = static_cast<int>(k / 2);
        if (badSteps.empty() || badSteps.back() != step)
          badSteps.push_back(step);
      }
    }
  } catch (const std::exception& e) {
    log += "[bridge] decimal parse error: ";
    log += e.what();
    log += "\n";
    return false;
  }

  char line[256];
  if (worst < -900.0) {
    std::snprintf(line, sizeof line,
                  "[bridge] check: steps=%zu orbitValues=%zu calcValues=%d "
                  "exact=%d worst=exact threshold=1e%.1f\n",
                  n / 2, orbit.size(), static_cast<int>(calc.size()), exact,
                  threshold);
  } else {
    std::snprintf(line, sizeof line,
                  "[bridge] check: steps=%zu orbitValues=%zu calcValues=%d "
                  "exact=%d worst=1e%.1f at step=%d threshold=1e%.1f\n",
                  n / 2, orbit.size(), static_cast<int>(calc.size()), exact,
                  worst, worstAt, threshold);
  }
  log += line;
  log += "[bridge] mismatched steps: ";
  if (badSteps.empty()) {
    log += "none";
  } else {
    for (size_t i = 0; i < badSteps.size(); ++i) {
      if (i) log += ",";
      log += std::to_string(badSteps[i]);
    }
  }
  log += "\n";
  const bool countOk = (orbit.size() == calc.size());
  if (!countOk) {
    std::snprintf(line, sizeof line,
                  "[bridge] note: orbit length %zu differs from Boost length "
                  "%d\n",
                  orbit.size(), static_cast<int>(calc.size()));
    log += line;
  }
  const bool ok = badSteps.empty() && countOk;
  log += ok ? "[bridge] check PASS\n" : "[bridge] check FAIL\n";
  return ok;
}

bool bridgeSelfTest(std::string& log) {
  log += "[bridge] backend=boost::multiprecision::cpp_bin_float "
         "judge=800 digits tiers=50/100/200/350/700\n";
  const bool a = checkLimbRoundTrip(log);
  const bool b = checkBoostVsMpf(log);
  const bool c = checkEmbeddedMpfr(log);
  log += (a && b && c) ? "[bridge] ALL PASS\n" : "[bridge] FAILURES\n";
  return a && b && c;
}

}  // namespace mbc
