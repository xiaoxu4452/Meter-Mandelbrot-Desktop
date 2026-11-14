// ------------------------------------------------------------------
// mb_view.h — the view arithmetic (zoom about a pixel, pan by pixels)
// evaluated in the C++ core with GMP.
//
// Why this is not done in the page any more
// -----------------------------------------
// The centre of a deep frame is a decimal string with as many digits as the
// zoom is deep (361 000 digits at 10^300000).  The page used to update it
// with its own BigInt fixed-point, and that had two problems: it is a second
// implementation of the same arithmetic as the renderer, and every limit it
// carried (16384 bits, a 2^-3322 pixel floor) silently turned a gesture into
// a no-op or into a rounding to a grid coarser than a pixel — the "huge
// deviation" seen at very high magnification.
//
// Here the same `mpf` that builds the reference orbit does the addition, at
// the same width, so a gesture can never disagree with the frame it produces.
// Every entry point is pure (no COM, no globals) and returns `ok=false` when
// GMP is absent, in which case the caller keeps using the page's own path.
// ------------------------------------------------------------------
#pragma once

#include <string>

namespace mbc {
namespace view {

struct Result {
  bool ok = false;
  std::string re, im;      // new centre, decimal
  double spanMant = 1.0;   // new span mantissa, normalised to [1,2)
  int spanExp = 0;         // new span exponent, 1×2^e
  const char* why = "";    // reason when ok == false
};

/** Zoom by `factor` about canvas pixel (px,py); equivalent to the recovered
 *  program's AppState::zoom_at_pixel — the plane point under the cursor is
 *  the one that does not move. */
Result zoomAt(const std::string& centerRe, const std::string& centerIm,
              double spanMant, int spanExp, int W, int H,
              double px, double py, double factor, double rotation, int bits);

/** Move the view by (dxPx,dyPx) canvas pixels: the content follows the hand,
 *  so the centre moves the other way. */
Result panBy(const std::string& centerRe, const std::string& centerIm,
             double spanMant, int spanExp, int W, int H,
             double dxPx, double dyPx, double rotation, int bits);

/** Width the centre must be carried at for a given frame, in bits — the same
 *  `depth + 64 + log2(maxIter)` rule the renderer uses. */
int bitsForView(double psLog2, int maxIter);

}  // namespace view
}  // namespace mbc
