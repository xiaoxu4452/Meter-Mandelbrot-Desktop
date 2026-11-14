// Ground truth for the 1e-30 Julia frame: iterate a small grid with 256-bit
// arithmetic and report the escape-count distribution.  If every sample lands
// on the same n, the flat frame is real; if not, the perturbation collapsed.
#include <boost/multiprecision/cpp_bin_float.hpp>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using Big = boost::multiprecision::cpp_bin_float<256>;

int main(int argc, char** argv) {
  const char* cre = argc > 1 ? argv[1] : "-0.8380311640480313";
  const char* cim = argc > 2 ? argv[2] : "0.2078952618343974";
  const double mant = argc > 3 ? std::atof(argv[3]) : 1.6;
  const int sexp = argc > 4 ? std::atoi(argv[4]) : -100;
  const char* jre = argc > 5 ? argv[5] : "-0.838312";
  const char* jim = argc > 6 ? argv[6] : "0.2078951";
  const int W = argc > 7 ? std::atoi(argv[7]) : 128;
  const int H = argc > 8 ? std::atoi(argv[8]) : 64;
  const int maxIter = argc > 9 ? std::atoi(argv[9]) : 63950;

  const Big cRe(cre), cIm(cim), jRe(jre), jIm(jim);
  // span = mant * 2^sexp, split into a double factor times a power of two so
  // the magnitude never overflows the exponent range of the type.
  Big span = Big(mant);
  if (sexp >= 0) for (int i = 0; i < sexp; ++i) span *= 2;
  else for (int i = 0; i < -sexp; ++i) span /= 2;
  const Big ps = span / Big(std::max(W, H));

  std::vector<int> hist;
  int lo = 1 << 30, hi = -1, inside = 0, samples = 0;
  std::vector<int> grid((size_t)W * H, 0);
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const Big dr = Big(x - W / 2) * ps;
      const Big di = Big(H / 2 - y) * ps;
      Big zr = cRe + dr, zi = cIm + di;
      int n = -1;
      for (int k = 0; k < maxIter; ++k) {
        const Big zr2 = zr * zr, zi2 = zi * zi;
        if (zr2 + zi2 > Big(4)) { n = k; break; }
        const Big nr = zr2 - zi2 + jRe;
        const Big ni = 2 * zr * zi + jIm;
        zr = nr; zi = ni;
      }
      grid[(size_t)y * W + x] = n;
      ++samples;
      if (n < 0) ++inside;
      else { if (n < lo) lo = n; if (n > hi) hi = n; }
    }
  }
  std::printf("samples=%d inside=%d esc=[%d,%d] distinct=", samples, inside,
              lo == (1 << 30) ? 0 : lo, hi);
  // count distinct escape counts
  std::vector<int> seen;
  for (int v : grid) {
    bool f = false;
    for (int s : seen) if (s == v) { f = true; break; }
    if (!f) seen.push_back(v);
  }
  std::printf("%zu\n", seen.size());
  for (int y = 0; y < H; y += std::max(1, H / 16)) {
    for (int x = 0; x < W; x += std::max(1, W / 48))
      std::printf("%6d", grid[(size_t)y * W + x]);
    std::printf("\n");
  }
  return 0;
}
