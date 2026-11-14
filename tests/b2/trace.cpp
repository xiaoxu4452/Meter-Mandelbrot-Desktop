// B2 trace: follow one pixel in both kernels side by side.
#include "bigfloat.hpp"
#include "deep.hpp"
#include "exact.hpp"
#include "floatexp.hpp"
#include "reference.hpp"
#include "render.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

static const char* CRE =
    "-1.0069743306013604554903205072737466724498341569620852222340047322401250216077881286791860894912078";
static const char* CIM =
    "-0.30929306234822977280937688333252666569592449443536899596506161915291164038903410366922816112723778";

int main(int argc, char** argv) {
    const int ze = argc > 1 ? atoi(argv[1]) : 6;
    const int maxiter = argc > 2 ? atoi(argv[2]) : 200;
    const int px = argc > 3 ? atoi(argv[3]) : 0;
    const int py = argc > 4 ? atoi(argv[4]) : 0;
    const int W = 300, H = 200;

    PrecisionPlan plan = plan_precision(1.0, ze, 0, 1200000);
    BigFloat::precision_limbs = plan.limbs;
    BigFloat cr = BigFloat::from_string(CRE);
    BigFloat ci = BigFloat::from_string(CIM);
    Fexp scale = (Fexp(3.0) / Fexp(1.0, ze)) / Fexp((double)W);

    // reference at the frame centre
    const int rdx = W / 2, rdy = H / 2;
    BigFloat rre = cr + BigFloat::from_floatexp((scale * Fexp((double)rdx + 0.5 - W * 0.5)).m,
                                                (scale * Fexp((double)rdx + 0.5 - W * 0.5)).e);
    BigFloat rim = ci + BigFloat::from_floatexp((scale * Fexp(H * 0.5 - (double)rdy - 0.5)).m,
                                                (scale * Fexp(H * 0.5 - (double)rdy - 0.5)).e);
    Orbit orb;
    orb.digits = plan.digits;
    Fexp dmax = scale * Fexp(std::sqrt((double)W * W + (double)H * H) * 0.5 + 1.0);
    std::string err;
    compute_orbit(orb, rre, rim, maxiter, false, 0, dmax, &err);

    // the pixel's own constant, at full precision
    const double pdx = ((double)px + 0.5) - W * 0.5;
    const double pdy = H * 0.5 - ((double)py + 0.5);
    BigFloat pcre = cr + BigFloat::from_floatexp((scale * Fexp(pdx)).m, (scale * Fexp(pdx)).e);
    BigFloat pcim = ci + BigFloat::from_floatexp((scale * Fexp(pdy)).m, (scale * Fexp(pdy)).e);

    printf("1e%d pixel(%d,%d) d0=(%.6e,%.6e) ref(%d,%d) refLen=%d\n", ze, px, py,
           scale.to_double() * (pdx - ((double)rdx + 0.5 - W * 0.5)),
           scale.to_double() * (pdy - (H * 0.5 - (double)rdy - 0.5)), rdx, rdy,
           (int)orb.z.size());
    printf("  c_pixel = %s + %si\n", pcre.to_scientific(30).c_str(), pcim.to_scientific(30).c_str());
    printf("  c_ref   = %s + %si\n", rre.to_scientific(30).c_str(), rim.to_scientific(30).c_str());

    // exact
    PixelOut pe = pixel_exact(cr, ci, scale, pdx, pdy, maxiter);
    printf("exact iter=%d smooth=%.3f interior=%d\n", pe.iter, pe.smooth, (int)pe.interior);

    // perturbation, in double complex (what the F2 kernel approximates)
    {
        double dcr = scale.to_double() * (pdx - ((double)rdx + 0.5 - W * 0.5));
        double dci = scale.to_double() * (pdy - (H * 0.5 - (double)rdy - 0.5));
        double dr = dcr, di = dci;
        printf("  n   |Z_exact|      |z_exact|      |z_pert|       |d|\n");
        for (int n = 0; n < (int)orb.z.size(); ++n) {
            const double zr = orb.z[(size_t)n].re.to_double();
            const double zi = orb.z[(size_t)n].im.to_double();
            const double ar = zr + dr, ai = zi + di;
            if (n < 6 || n > 120) {
                printf("  %3d  %.10e  %.10e  %.10e  %.3e\n", n,
                       std::sqrt(zr * zr + zi * zi), std::sqrt(ar * ar + ai * ai),
                       std::sqrt(ar * ar + ai * ai), std::sqrt(dr * dr + di * di));
            }
            if (ar * ar + ai * ai > 4.0) {
                printf("  double-perturbation escapes at n=%d\n", n);
                break;
            }
            const double nr = 2.0 * (zr * dr - zi * di) + (dr * dr - di * di) + dcr;
            const double ni = 2.0 * (zr * di + zi * dr) + 2.0 * dr * di + dci;
            dr = nr;
            di = ni;
            if (n + 1 >= (int)orb.z.size()) break;
        }
    }
    return 0;
}
