// B2: render the same view through the perturbation engine and through the
// exact oracle, and write both to PNG so they can be compared by eye.
#include "bigfloat.hpp"
#include "image.hpp"
#include "render.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

static const char* CRE =
    "-1.0069743306013604554903205072737466724498341569620852222340047322401250216077881286791860894912078";
static const char* CIM =
    "-0.30929306234822977280937688333252666569592449443536899596506161915291164038903410366922816112723778";

int main(int argc, char** argv) {
    const int ze = argc > 1 ? atoi(argv[1]) : 38;
    const int W = argc > 2 ? atoi(argv[2]) : 300;
    const int H = argc > 3 ? atoi(argv[3]) : 200;
    const int maxiter = argc > 4 ? atoi(argv[4]) : 3000;
    const bool do_exact = argc > 5 && atoi(argv[5]) != 0;

    auto run = [&](bool exact, const char* tag) {
        RenderRequest req;
        req.center_re = CRE;
        req.center_im = CIM;
        req.zoom_mantissa = 1.0;
        req.zoom_exp10 = ze;
        req.width = W;
        req.height = H;
        req.maxiter = maxiter;
        req.exact = exact;
        req.palette = "rainbow";
        req.color_mode = ColorMode::Smooth;
        req.interior_palette = true;

        Image im;
        RenderStats st;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        if (!render_image(req, im, st, &err)) {
            printf("%s FAILED: %s\n", tag, err.c_str());
            return;
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        char path[256];
        snprintf(path, sizeof path, "build/%s_1e%d.png", tag, ze);
        if (!write_png(path, im, &err)) printf("write failed: %s\n", err.c_str());
        printf("%-10s %4dx%-4d it=%-6d ref=%-6d refs=%-2lld glitch=%-6lld steps=%-10lld %8.1f ms -> %s\n",
               tag, W, H, maxiter, st.reference_length, st.tile_references, st.glitches, st.steps,
               ms, path);
        fflush(stdout);
    };

    run(false, "perturb");
    if (do_exact) run(true, "exact");
    return 0;
}
