// test_bigmath.cpp — checks the elementary-function layer against known
// decimal expansions, at a precision an order of magnitude below the 1e120
// target (so a missing guard limb shows up as a wrong last digit here rather
// than as a bent frame at depth).
#include "../src/mathx/bigmath.hpp"

#include <cstdio>
#include <string>

using bm::BigFloat;

static BigFloat N(int64_t v) { return BigFloat(v); }

static int g_fail = 0;

static std::string fixed(const BigFloat& v, int decimals) {
    return v.to_fixed(decimals);
}

static void expect(const char* what, const std::string& got, const std::string& want,
                   int prefix) {
    const bool ok = got.size() >= (size_t)prefix && got.compare(0, (size_t)prefix, want, 0, (size_t)prefix) == 0;
    if (!ok) {
        ++g_fail;
        std::printf("FAIL %-10s got  %s\n              want %s\n", what, got.c_str(), want.c_str());
    } else {
        std::printf("ok   %-10s %s\n", what, got.substr(0, (size_t)prefix).c_str());
    }
}

int main() {
    BigFloat::precision_limbs = 8;   // 72 decimal digits

    expect("sqrt2", fixed(bm::sqrt(N(2)), 60),
           "1.414213562373095048801688724209698078569671875376948073176", 58);
    expect("e", fixed(bm::exp(N(1)), 60),
           "2.718281828459045235360287471352662497757247093699959574966", 58);
    expect("ln2", fixed(bm::log(N(2)), 60),
           "0.693147180559945309417232121458176568075500134360255254120", 58);
    expect("pi", fixed(bm::pi(), 60),
           "3.141592653589793238462643383279502884197169399375105820974", 58);
    expect("sin1", fixed(bm::sin(N(1)), 60),
           "0.841470984807896506652502321630298999622563060798371065672", 58);
    expect("cos1", fixed(bm::cos(N(1)), 60),
           "0.540302305868139717400936607442976603732310420617922227670", 58);
    expect("atan1", fixed(bm::atan(N(1)), 60),
           "0.785398163397448309615660845819875721049292349843776455243", 58);
    expect("ln10", fixed(bm::ln10(), 60),
           "2.302585092994045684017991454684364207601101488628772976033", 58);

    /* Division and roots at depth: 1e120 / 3e60 = 3.333...e59. */
    BigFloat big = BigFloat::from_string("1e120");
    BigFloat three = BigFloat::from_string("3e60");
    expect("div-e120", bm::div(big, three).to_scientific(40), "3.333333333333333333333333333333333333333e59", 40);
    {
        const BigFloat want1 = BigFloat::from_string("1e60");
        const bool ok = bm::sqrt(big).compare(want1) == 0;
        std::printf(ok ? "ok   sqrt-e120  1e60 exactly\n" : "FAIL sqrt-e120\n");
        if (!ok) ++g_fail;
    }
    expect("pow-e120", bm::powi(BigFloat::from_string("1.00000000000000000001e30"), 4).to_scientific(30),
           "1.000000000000000000040000000000e120", 30);

    /* Cancellation: (1+1e-40)^2 - 1 must keep the small term. */
    BigFloat one = N(1);
    BigFloat eps = BigFloat::from_string("1e-40");
    BigFloat sq = bm::powi(one.add(eps), 2);
    expect("cancel", sq.sub(one).to_scientific(12), "2.00000e-40", 11);

    std::printf(g_fail ? "\n%d FAILURES\n" : "\nall bigmath checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
