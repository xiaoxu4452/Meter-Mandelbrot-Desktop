// B2 acceptance: perturbation engine vs exact BigFloat oracle, pixel by pixel.
//
//   b2 cmp <zoom_exp10> <maxiter> <grid_step> [x0 y0 w h]
#include "bigfloat.hpp"
#include "deep.hpp"
#include "exact.hpp"
#include "floatexp.hpp"
#include "image.hpp"
#include "palette.hpp"
#include "reference.hpp"
#include "render.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

static const char* CRE =
    "-1.0069743306013604554903205072737466724498341569620852222340047322401250216077881286791860894912078";
static const char* CIM =
    "-0.30929306234822977280937688333252666569592449443536899596506161915291164038903410366922816112723778";

int main(int argc, char** argv) {
    const int ze = argc > 1 ? atoi(argv[1]) : 38;
    const int maxiter = argc > 2 ? atoi(argv[2]) : 3000;
    const int step = argc > 3 ? atoi(argv[3]) : 7;
    int X0 = 0, Y0 = 0, W = 300, H = 200;
    if (argc > 7) { X0 = atoi(argv[4]); Y0 = atoi(argv[5]); W = atoi(argv[6]); H = atoi(argv[7]); }

    PrecisionPlan plan = plan_precision(1.0, ze, 0, 1200000);
    BigFloat::precision_limbs = plan.limbs;
    BigFloat cr = BigFloat::from_string(CRE);
    BigFloat ci = BigFloat::from_string(CIM);
    Fexp scale = (Fexp(3.0) / Fexp(1.0, ze)) / Fexp((double)W);

    DeepParams dp;
    dp.cr = &cr;
    dp.ci = &ci;
    dp.scale = scale;
    dp.width = W;
    dp.height = H;
    dp.maxiter = maxiter;
    dp.digits = plan.digits;
    const char* sa_env = getenv("B2_SA");
    dp.sa_order = sa_env ? atoi(sa_env) : 8;
    const char* rr = getenv("B2_REPAIR");
    if (rr) dp.max_repair_rounds = atoi(rr);
    const char* rf = getenv("B2_REF");
    if (rf) dp.max_ref_rounds = atoi(rf);
    dp.threads = 8;
    dp.use_distance = false;

    std::vector<PixelOut> px;
    DeepStats ds;
    deep_render(dp, px, &ds);
    printf("1e%d %dx%d maxiter=%d digits=%d refLen=%d refs=%d rounds=%d saSkip=%d glitches=%lld\n",
           ze, W, H, maxiter, plan.digits, ds.reference_len, ds.references, ds.rounds_used,
           ds.sa_skip, ds.glitches);

    if (X0 || Y0) { /* sub-frame offset is expressed via the scale above */ }

    {   // where do the unresolved pixels break?
        std::map<int,int> gh;
        for (auto& q : px) if (q.glitch) gh[q.glitch]++;
        printf("glitch-n histogram (first 8):");
        int k = 0;
        for (auto& kv : gh) { printf(" n=%d:%d", kv.first, kv.second); if (++k >= 8) break; }
        printf("  [total %zu of %zu]\n", gh.size(), px.size());
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 6; ++x)
                printf("  px(%d,%d) iter=%d glitch=%d interior=%d skipped=%d\n", x, y,
                       px[(size_t)y*W+x].iter, px[(size_t)y*W+x].glitch,
                       (int)px[(size_t)y*W+x].interior, px[(size_t)y*W+x].skipped);
    }

    // Optional: the same oracle at a much higher precision.  Pixels that
    // disagree with *itself* are chaotic, not wrong.
    const bool floor_mode = getenv("B2_FLOOR") != nullptr;
    int fl_same = 0, fl_diff = 0;

    int same = 0, diff = 0, exact_int = 0, pert_int = 0, unresolved = 0;
    long long sumd = 0, maxd = 0;
    std::map<int, int> hist;
    int shown = 0;
    for (int y = step / 2; y < H; y += step) {
        for (int x = step / 2; x < W; x += step) {
            const double dx = ((double)(x + X0) + 0.5) - (double)W * 0.5;
            const double dy = (double)H * 0.5 - ((double)(y + Y0) + 0.5);
            BigFloat::precision_limbs = plan.limbs;
            PixelOut pe = pixel_exact(cr, ci, scale, dx, dy, maxiter);
            if (floor_mode) {
                BigFloat::precision_limbs = plan.limbs + 20;
                PixelOut pf = pixel_exact(cr, ci, scale, dx, dy, maxiter);
                BigFloat::precision_limbs = plan.limbs;
                const int a = pe.interior ? maxiter : pe.iter;
                const int b = pf.interior ? maxiter : pf.iter;
                if (a == b) ++fl_same; else ++fl_diff;
            }
            PixelOut pp = px[(size_t)y * W + x];
            if (pp.glitch) ++unresolved;
            const int ie = pe.interior ? maxiter : pe.iter;
            const int ip = pp.interior ? maxiter : pp.iter;
            if (pe.interior) ++exact_int;
            if (pp.interior) ++pert_int;
            const int d = ip - ie;
            if (d == 0) ++same; else { ++diff; hist[d] += 1; }
            sumd += std::abs(d);
            maxd = std::max<long long>(maxd, std::abs(d));
            if (d != 0 && shown < 10) {
                printf("  (%3d,%3d) exact=%5d pert=%5d d=%+d%s\n", x, y, ie, ip, d,
                       pp.glitch ? " [unresolved]" : "");
                ++shown;
            }
        }
    }
    const int n = std::max(1, same + diff);
    printf("same=%d diff=%d -> %.2f%% exact; interior exact/pert=%d/%d unresolved=%d "
           "meanAbs=%lld maxAbs=%lld\n",
           same, diff, 100.0 * same / n, exact_int, pert_int, unresolved,
           sumd / n, maxd);
    if (floor_mode)
        printf("oracle self-consistency (digits %d vs %d): same=%d diff=%d\n", plan.digits,
               plan.digits + 180, fl_same, fl_diff);
    printf("delta histogram:");
    for (auto& kv : hist) printf(" %+d:%d", kv.first, kv.second);
    printf("\n");
    return diff == 0 ? 0 : (diff * 100 > n ? 2 : 1);
}
