#include "mb_view.h"

#include "mb_gmp.h"

#include <cmath>

namespace mbc {
namespace view {
namespace {

int digitsForBits(int bits) {
  int d = (int)std::floor((double)bits * 0.30103) + 2;
  return d < 24 ? 24 : d;
}

void normalise(double& mant, int& exp) {
  if (!(std::isfinite(mant)) || mant == 0.0) {
    mant = 1.0;
    return;
  }
  while (std::fabs(mant) >= 2.0) {
    mant *= 0.5;
    exp += 1;
  }
  while (std::fabs(mant) < 1.0) {
    mant *= 2.0;
    exp -= 1;
  }
}

/* Shared body: the pixel offset (du,dv) is expressed in units of 2^psExp and
 * is at most a few thousand, so the addition into the centre is the only
 * operation that needs the wide mantissa. */
Result apply(const std::string& cre, const std::string& cim, double spanMant,
             int spanExp, int W, double u, double v, int bits) {
  Result r;
  r.spanMant = spanMant;
  r.spanExp = spanExp;
  if (!gmp::available()) {
    r.why = "no-gmp";
    return r;
  }
  if (W <= 0 || !std::isfinite(u) || !std::isfinite(v)) {
    r.why = "bad-args";
    return r;
  }
  const int prec = (bits + 64) < 256 ? 256 : bits + 64;
  gmp::Num cr, ci, d;
  cr.init(prec);
  ci.init(prec);
  d.init(prec);
  if (!cr.isInit() || !ci.isInit() || !d.isInit()) {
    r.why = "alloc";
    return r;
  }
  cr.setStr(cre);
  ci.setStr(cim);

  d.setD(u);
  d.shiftExp(d, spanExp);
  cr.add(cr, d);

  d.setD(v);
  d.shiftExp(d, spanExp);
  ci.add(ci, d);

  const int digits = digitsForBits(bits);
  r.re = cr.toDecimal(digits);
  r.im = ci.toDecimal(digits);
  r.ok = !r.re.empty() && !r.im.empty();
  if (!r.ok) r.why = "format";
  return r;
}

}  // namespace

int bitsForView(double psLog2, int maxIter) {
  double need = -psLog2 + 64.0;
  if (maxIter > 1) need += std::log2((double)maxIter);
  if (!(need > 64.0)) need = 64.0;
  if (need > 1200000.0) need = 1200000.0;
  int b = (int)(std::ceil(need / 64.0) * 64.0);
  return b < 96 ? 96 : b;
}

Result zoomAt(const std::string& centerRe, const std::string& centerIm,
              double spanMant, int spanExp, int W, int H, double px, double py,
              double factor, double rotation, int bits) {
  Result r;
  if (W <= 0 || H <= 0 || !std::isfinite(factor) || factor <= 0.0) {
    r.why = "bad-args";
    return r;
  }
  const double psMant = spanMant / (double)W;
  const double th = rotation;
  const double c = std::cos(th), s = std::sin(th);
  const double dxPx = px - (double)W * 0.5;
  const double dyPx = py - (double)H * 0.5;
  const double x = dxPx * psMant;
  const double y = -dyPx * psMant;
  /* before - after = d*ps_old*(1 - factor); see applyZoomAt in js/app.js. */
  const double k = 1.0 - factor;
  const double u = (x * c - y * s) * k;
  const double v = (x * s + y * c) * k;

  r = apply(centerRe, centerIm, spanMant, spanExp, W, u, v, bits);
  if (!r.ok) return r;
  double m = spanMant * factor;
  int e = spanExp;
  normalise(m, e);
  r.spanMant = m;
  r.spanExp = e;
  return r;
}

Result panBy(const std::string& centerRe, const std::string& centerIm,
             double spanMant, int spanExp, int W, int H, double dxPx,
             double dyPx, double rotation, int bits) {
  if (W <= 0) {
    Result r;
    r.why = "bad-args";
    return r;
  }
  const double psMant = spanMant / (double)W;
  const double th = rotation;
  const double c = std::cos(th), s = std::sin(th);
  const double x = dxPx * psMant;
  const double y = -dyPx * psMant;
  /* The content follows the hand, so the centre moves the other way. */
  const double u = -(x * c - y * s);
  const double v = -(x * s + y * c);
  return apply(centerRe, centerIm, spanMant, spanExp, W, u, v, bits);
}

}  // namespace view
}  // namespace mbc
