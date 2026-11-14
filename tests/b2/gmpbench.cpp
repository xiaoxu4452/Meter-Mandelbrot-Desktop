/* Micro-benchmark for the GMP path: where does a reference-orbit step go? */
#include "gmp_bridge.hpp"
#include "floatexp.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using Clock = std::chrono::steady_clock;
static double ms(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

int main(int argc, char** argv) {
    if (!gmpb::available()) {
        std::printf("gmp unavailable: %s\n", gmpb::error());
        return 1;
    }
    std::printf("gmp %s  %s\n", gmpb::version(), gmpb::path());
    const int N = argc > 1 ? std::atoi(argv[1]) : 2000;
    /* Up to 120000 digits: the range where GMP switches between Toom-Cook and
     * its FFT multiply.  A flat exponent across the whole table means no FFT
     * threshold was crossed and swapping in an FFT backend cannot help. */
    for (int digits : {1000, 2000, 4000, 6000, 12000, 24000, 60000, 120000}) {
        const int bits = (int)((double)digits * 3.321928094887362) + 32;
        gmpb::Num a(bits), b(bits), c(bits), d(bits), e(bits);
        gmpb::set_d(a.v, 0.713724);
        gmpb::set_d(b.v, 0.291374);
        gmpb::set_d(c.v, 0.5);

        auto t0 = Clock::now();
        for (int i = 0; i < N; ++i) {
            gmpb::add(d.v, a.v, b.v);
            gmpb::sub(e.v, a.v, b.v);
            gmpb::mul(c.v, d.v, e.v);
            gmpb::mul(d.v, a.v, b.v);
            gmpb::mul_2exp(d.v, d.v, 1);
            gmpb::add(a.v, c.v, b.v);
        }
        auto t1 = Clock::now();

        int32_t ex = 0;
        auto t2 = Clock::now();
        double acc = 0;
        for (int i = 0; i < N; ++i) {
            acc += gmpb::get_d_2exp(&ex, a.v);
            Fexp f = Fexp(acc, 0);
            acc += f.e;
        }
        auto t3 = Clock::now();

        auto t4 = Clock::now();
        int cnt = 0;
        for (int i = 0; i < N; ++i) {
            cnt += gmpb::cmp_d(a.v, 1e40) > 0;
            cnt += gmpb::cmp_d(a.v, -1e40) < 0;
        }
        auto t5 = Clock::now();

        const double step_ms = ms(t0, t1) / N;
        std::printf("digits=%-5d bits=%-6d step=%8.3fus  get_d+norm=%7.3fus  cmp_d(x2)=%7.3fus\n",
                    digits, bits, step_ms * 1000.0, ms(t2, t3) / N * 1000.0,
                    ms(t4, t5) / N * 1000.0);
        (void)cnt;
    }
    return 0;
}
