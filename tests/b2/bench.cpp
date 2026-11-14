// B2 bench: render a ladder of depths, report time and the reference stats.
//   b2 bench [width height]
#include "bigfloat.hpp"
#include "floatexp.hpp"
#include "image.hpp"
#include "palette.hpp"
#include "reference.hpp"
#include "render.hpp"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char* CRE =
    "-1.0069743306013604554903205072737466724498341569620852222340047322401250216077881286791860894912078";
static const char* CIM =
    "-0.30929306234822977280937688333252666569592449443536899596506161915291164038903410366922816112723778";

// The classic sea-horse valley point, which has structure at every depth.
static const char* SRE =
    "-0.743643887037151";
static const char* SIM = "0.131825904205330";

static bool write_ppm(Image& im, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    fprintf(f, "P6\n%d %d\n255\n", im.width, im.height);
    for (int y = 0; y < im.height; ++y) fwrite(im.pixel(0, y), 1, (size_t)im.width * 3, f);
    fclose(f);
    return true;
}

int main(int argc, char** argv) {
    const int W = argc > 1 ? atoi(argv[1]) : 800;
    const int H = argc > 2 ? atoi(argv[2]) : 600;
    const bool structure = getenv("B2_SEA") != nullptr;
    unsigned hw = std::thread::hardware_concurrency();
    printf("hardware_concurrency=%u\n", hw);
    fflush(stdout);

    struct Case { int e; int maxiter; };
    // Iteration counts roughly follow the depth; the point here is scaling, not
    // the exact budget a UI would pick.
    std::vector<Case> cases = {{10, 800},   {20, 1600},  {38, 3000},
                               {60, 4000},  {100, 6000}, {300, 12000},
                               {1000, 20000}, {6000, 40000}};

    const char* only = getenv("B2_ONLY");   // e.g. "38" runs that depth only
    std::vector<Case> run;
    for (const Case& c : cases) {
        if (!only || atoi(only) == c.e) run.push_back(c);
    }
    for (const Case& c : run) {
        RenderRequest req;
        req.center_re = structure ? SRE : CRE;
        req.center_im = structure ? SIM : CIM;
        req.zoom_mantissa = 1.0;
        req.zoom_exp10 = c.e;
        req.width = W;
        req.height = H;
        req.maxiter = c.maxiter;
        req.palette = "rainbow";
        req.color_mode = ColorMode::Smooth;
        req.tile_size = 64;
        if (const char* t = getenv("B2_THREADS")) req.threads = atoi(t);

        Image im;
        RenderStats st;
        std::string err;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = render_image(req, im, st, &err);
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!ok) {
            printf("1e%-6d FAILED: %s\n", c.e, err.c_str());
            continue;
        }
        const long long px = (long long)W * H;
        const long long steps = st.steps;
        printf("1e%-6d %4dx%-4d it=%-6d ref=%-6d refs=%-2lld saSkip=%-6lld steps=%-10lld "
               "%8.1f ms  %6.2f us/px  %5.1f ns/step\n",
               c.e, W, H, c.maxiter, st.reference_length, st.tile_references,
               st.sa_skipped_iters / std::max(1LL, px), steps, ms,
               ms * 1000.0 / (double)px, ms * 1e6 / (double)std::max(1LL, steps));
        fflush(stdout);
        if (structure) {
            char path[256];
            snprintf(path, sizeof path, "build/sea_1e%d.ppm", c.e);
            write_ppm(im, path);
        }
    }
    return 0;
}
