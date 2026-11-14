#include "test_support.hpp"

#include "bigfloat.hpp"
#include "floatexp.hpp"
#include "image.hpp"
#include "reference.hpp"
#include "render.hpp"
#include "viewport.hpp"
#include "algorithms.hpp"
#include "workspace.hpp"
#include "palette.hpp"
#include "hierarchy.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int g_checks = 0;
int g_failures = 0;

namespace test_support {
namespace {
std::vector<Item>& registry() {
    static std::vector<Item> items;
    return items;
}
}  // namespace
void add(const char* name, Fn fn) { registry().push_back(Item{name, fn}); }
const std::vector<Item>& items() { return registry(); }
}  // namespace test_support

namespace {

void test_bigfloat_basic() {
    BigFloat::precision_limbs = 32;
    BigFloat a = BigFloat::from_string("-123.45");
    CHECK(a.sign == -1);
    CHECK(a.to_fixed(2) == "-123.45");

    BigFloat b = BigFloat::from_string("0.000000000000000000000000000001");
    BigFloat c = BigFloat::from_string("1");
    BigFloat s = c + b;
    BigFloat d = s - c;
    CHECK(d.to_scientific(12).find("1.0000") == 0);

    BigFloat x = BigFloat::from_string("123456789012345678901234567890");
    BigFloat y = BigFloat::from_string("987654321098765432109876543210");
    BigFloat p = x * y;
    // Independent check with Python-style decimal arithmetic is done in the
    // artefact generation phase; here check structural invariants.
    CHECK(!p.is_zero());
    CHECK(p.sign == 1);

    BigFloat z = BigFloat::from_string("0");
    CHECK(z.is_zero());
    BigFloat nz = -z;
    CHECK(nz.is_zero());
}

void test_bigfloat_precision() {
    BigFloat::precision_limbs = 120;  // 1080 decimal digits
    BigFloat one = BigFloat::from_string("1");
    BigFloat tiny = BigFloat::from_string("1e-1000");
    BigFloat sum = one + tiny;
    BigFloat back = sum - one;
    CHECK(!back.is_zero());
    double m;
    int64_t e;
    back.to_floatexp(m, e);
    CHECK(e <= -999 && e >= -1001);

    // Product identity: (1 + 1e-500)^2 = 1 + 2e-500 + 1e-1000
    BigFloat t = BigFloat::from_string("1e-500");
    BigFloat u = one + t;
    BigFloat sq = u * u;
    BigFloat expected = one + BigFloat::from_string("2e-500") + BigFloat::from_string("1e-1000");
    BigFloat err = sq - expected;
    CHECK(err.abs().to_double() < 1e-100);
}

void test_floatexp() {
    Fexp a(1.0, -800000);
    Fexp b(1.0, -800000);
    Fexp c = a * b;
    CHECK(c.e == -1600000);
    CHECK(std::fabs(c.m - 1.0) < 1e-15);

    Fexp one(1.0, 0);
    Fexp sum = one + a;
    CHECK(sum.m == 1.0 && sum.e == 0);

    Fexp d(2.5, 3);
    CHECK(std::fabs(d.to_double() - 2500.0) < 1e-9);

    Fexp e(9.9, 10);
    CHECK(e.log10_abs() > 10.0 && e.log10_abs() < 11.0);
}

void test_precision_plan() {
    PrecisionPlan p = plan_precision(1.0, 800000, 0, 1200000);
    CHECK(p.digits >= 800000);
    CHECK(p.limbs * 9 >= p.digits);
    CHECK(p.limbs < 100000);

    PrecisionPlan q = plan_precision(1.0, 12, 0, 1200000);
    CHECK(q.digits >= 60);
}

void test_reference_orbit() {
    BigFloat::precision_limbs = 16;
    BigFloat cr = BigFloat::from_string("-1");
    BigFloat ci = BigFloat::from_string("0");
    Orbit o;
    o.digits = 120;
    std::string err;
    Fexp dm(1e-9, 0);
    CHECK(compute_orbit(o, cr, ci, 20, false, 0, dm, &err));
    CHECK(o.z.size() >= 4);
    // z0=0, z1=-1, z2=0, z3=-1 ...
    CHECK(o.z[0].is_zero());
    CHECK(std::fabs(o.z[1].re.to_double() + 1.0) < 1e-12);
    CHECK(o.z[2].is_zero());
}

void test_render_shallow() {
    RenderRequest req;
    req.center_re = "-0.6";
    req.center_im = "0.0";
    req.zoom_mantissa = 1.0;
    req.zoom_exp10 = 0;
    req.width = 64;
    req.height = 48;
    req.maxiter = 400;
    req.threads = 2;
    req.tile_size = 32;
    Image img;
    RenderStats st;
    std::string err;
    CHECK(render_image(req, img, st, &err));
    CHECK(img.rgb.size() == (size_t)64 * 48 * 3);
    // The set must contain both interior (black) and exterior (coloured) pixels.
    int black = 0, coloured = 0;
    for (size_t i = 0; i < img.rgb.size(); i += 3) {
        if (img.rgb[i] < 10 && img.rgb[i + 1] < 10 && img.rgb[i + 2] < 10) ++black;
        else ++coloured;
    }
    CHECK(black > 50);
    CHECK(coloured > 50);
}

void test_series_approximation() {
    BigFloat::precision_limbs = 40;
    RenderRequest req;
    req.center_re = "-0.7436438870371587";
    req.center_im = "0.1318259042053119";
    req.zoom_mantissa = 1.0;
    req.zoom_exp10 = 10;
    req.width = 48;
    req.height = 32;
    req.maxiter = 1500;
    req.threads = 2;
    req.tile_size = 16;
    req.sa_order = 8;
    req.use_sa = true;
    req.tile_rebase = false;
    Image a;
    RenderStats sa;
    std::string err;
    CHECK(render_image(req, a, sa, &err));
    req.use_sa = false;
    req.sa_order = 0;
    Image b;
    RenderStats no;
    CHECK(render_image(req, b, no, &err));

    long long diff = 0;
    for (size_t i = 0; i < a.rgb.size(); ++i) {
        diff += std::abs((int)a.rgb[i] - (int)b.rgb[i]);
    }
    double mean = (double)diff / (double)a.rgb.size();
    CHECK(mean < 3.0);
    CHECK(sa.sa_skipped_iters > 0);
}

void test_perturbation_vs_exact() {
    RenderRequest req;
    req.center_re = "-0.75";
    req.center_im = "0.1";
    req.zoom_mantissa = 1.0;
    req.zoom_exp10 = 4;
    req.width = 8;
    req.height = 8;
    req.maxiter = 200;
    req.threads = 2;
    req.tile_size = 8;
    req.sa_order = 0;
    req.use_sa = false;
    req.tile_rebase = false;
    Image pert;
    RenderStats s1;
    std::string err;
    CHECK(render_image(req, pert, s1, &err));
    req.exact = true;
    Image exact;
    RenderStats s2;
    CHECK(render_image(req, exact, s2, &err));

    long long diff = 0;
    for (size_t i = 0; i < pert.rgb.size(); ++i) diff += std::abs((int)pert.rgb[i] - (int)exact.rgb[i]);
    double mean = (double)diff / (double)pert.rgb.size();
    CHECK(mean < 6.0);
}

void test_png() {
    Image img;
    img.resize(8, 6);
    for (int i = 0; i < 8 * 6; ++i) {
        img.rgb[i * 3 + 0] = (uint8_t)(i * 3);
        img.rgb[i * 3 + 1] = (uint8_t)(i * 5);
        img.rgb[i * 3 + 2] = (uint8_t)(i * 7);
    }
    std::string err;
    CHECK(write_png("test_output.png", img, &err));
    FILE* f = std::fopen("test_output.png", "rb");
    CHECK(f != nullptr);
    if (f) {
        unsigned char sig[8];
        CHECK(std::fread(sig, 1, 8, f) == 8);
        CHECK(sig[0] == 137 && sig[1] == 'P' && sig[2] == 'N' && sig[3] == 'G');
        std::fclose(f);
    }
}

void test_viewport() {
    Viewport vp;
    vp.width = 100;
    vp.height = 50;
    vp.zoom_mantissa = 1.0;
    vp.zoom_exp10 = 10;
    vp.recalc();
    CHECK(vp.plan.digits >= 60);
    CHECK(std::fabs(vp.log10_zoom() - 10.0) < 1e-9);
    Fexp ps = vp.pixel_scale();
    CHECK(std::fabs(ps.log10_abs() - std::log10(3.0 / 1e10 / 100.0)) < 0.01);
    auto c = vp.point_at(50, 25);
    CHECK(std::fabs(c.first.to_double() - vp.center_re.to_double()) < 1e-9);
    double before = vp.center_re.to_double();
    vp.pan_pixels(10.0, 0.0);
    CHECK(std::fabs(vp.center_re.to_double() - before) > 0.0);
    Fexp delta = vp.zoom_delta();
    CHECK(delta.e < 0);
    vp.zoom_by(10.0);
    CHECK(vp.zoom_exp10 == 11);
}

void test_algorithms() {
    std::vector<CFexp> cycle;
    for (int i = 0; i < 10; ++i) cycle.push_back(CFexp(Fexp(i % 2 ? 1.0 : 2.0), Fexp(0.0)));
    CHECK(detect_period(cycle, 4, 1e-12) == 2);
    CHECK(is_misiurewicz(cycle, 0, 2, 1e-12));
    CHECK(find_preperiod(cycle, 2, 1e-12) == 0);

    Orbit o;
    o.digits = 80;
    std::string err;
    CHECK(compute_orbit(o, BigFloat::from_string("-0.6"), BigFloat(0.0), 200, false, 8,
                        Fexp(1.0, -12), &err));
    CHECK(radtest(o.sa, Fexp(1.0, -12), -5.0));
    CHECK(series_error_log10(o.sa, Fexp(1.0, -12)) < 0.0);
}

void test_workspace() {
    JsonValue root;
    std::string err;
    if (!parse_json(R"({"a": 1, "b": [true, "x"], "c": {"d": null}})", root, &err)) std::cerr << "json error: " << err << "\n";
    CHECK(root.type == JsonValue::Type::Object);
    CHECK(root.type == JsonValue::Type::Object);
    CHECK(root.find("a") && root.find("a")->as_int() == 1);
    CHECK(root.find("b") && root.find("b")->array.size() == 2);
    CHECK(root.find("b")->array[0].as_bool());
    CHECK(root.find("b")->array[1].as_string() == "x");
    CHECK(dump_json(root, 0).find("\"a\"") != std::string::npos);

    RenderRequest req;
    req.center_re = "-0.743643887037151";
    req.zoom_mantissa = 1.0;
    req.zoom_exp10 = 6;
    req.width = 123;
    req.julia = true;
    req.julia_re = "-0.8";
    req.color_mode = ColorMode::Smooth;
    CHECK(save_workspace("test_workspace.json", req, &err));
    RenderRequest back;
    CHECK(load_workspace("test_workspace.json", back, &err));
    CHECK(back.center_re == req.center_re);
    CHECK(back.zoom_exp10 == 6);
    CHECK(back.width == 123);
    CHECK(back.julia);
    CHECK(back.julia_re == "-0.8");
}

void test_color() {
    RGB c{0.2, 0.6, 0.9};
    HSV h = rgb_to_hsv(c);
    CHECK(h.s > 0.7 && h.v > 0.8);
    RGB back = hsv_to_rgb(h);
    CHECK(std::fabs(back.r - c.r) < 1e-9);
    CHECK(std::fabs(back.g - c.g) < 1e-9);
    CHECK(std::fabs(back.b - c.b) < 1e-9);
    CHECK(std::fabs(hsv_to_rgb(HSV{0.0, 1.0, 1.0}).r - 1.0) < 1e-9);
}

void test_hierarchy() {
    Viewport vp;
    vp.width = 100;
    vp.height = 100;
    vp.center_re = BigFloat::from_string("-0.6");
    vp.center_im = BigFloat(0.0);
    vp.recalc();
    ReferenceHierarchy h;
    std::string err;
    Fexp delta = vp.zoom_delta();
    int primary = h.add_primary(vp.center_re, vp.center_im, 300, 6, delta, &err);
    CHECK(primary == 0);
    int local = h.add_local(primary, vp, 25, 25, 50, 50, false, BigFloat(0.0),
                            BigFloat(0.0), 300, 6, &err);
    CHECK(local >= 1);
    CHECK(h.find_covering(40, 40) != nullptr);
    CHECK(h.find_covering(40, 40)->parent == primary);
}

}  // namespace

int main() {
    std::cout << "mset_tests\n";
    test_bigfloat_basic();
    test_bigfloat_precision();
    test_floatexp();
    test_precision_plan();
    test_reference_orbit();
    test_render_shallow();
    test_series_approximation();
    test_perturbation_vs_exact();
    test_png();
    test_viewport();
    test_algorithms();
    test_workspace();
    test_color();
    test_hierarchy();

    // Registered family tests (tests/test_port_*.cpp and further slices).
    for (const auto& item : test_support::items()) {
        std::cout << "[test] " << item.name << "\n";
        item.fn();
    }

    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
