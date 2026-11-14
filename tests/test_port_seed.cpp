// Port-layer seed tests: build-time and behavioural evidence that the port
// layer is real code, not documentation.  Each family adds its own file
// (tests/test_port_<family>.cpp) using the MSET_TEST registration macro.

#include "test_support.hpp"

#include "app/app_state.hpp"
#include "app/ui_model.hpp"
#include "port/crt/crt.hpp"
#include "port/gmp/gmp.hpp"
#include "port/gl/gl.hpp"
#include "port/platform/plt.hpp"
#include "port/video/video.hpp"

#include <cmath>
#include <cstring>
#include <string>

MSET_TEST(port_crt_memory) {
    char dst[16] = {};
    const char* src = "abc";
    CHECK(mset::port::crt::str_len(src) == 3);
    CHECK(mset::port::crt::str_copy(dst, src) == dst);
    CHECK(std::strcmp(dst, "abc") == 0);
    CHECK(mset::port::crt::str_cat(dst, "de") == dst);
    CHECK(std::strcmp(dst, "abcde") == 0);
    CHECK(mset::port::crt::str_cmp("abc", "abd") == -1);
    CHECK(mset::port::crt::str_cmp("abd", "abc") == 1);
    CHECK(mset::port::crt::str_cmp("abc", "abc") == 0);

    unsigned char block[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    CHECK(mset::port::crt::mem_set(block, 0xAB, sizeof(block)) == block);
    CHECK(block[0] == 0xAB && block[7] == 0xAB);
    CHECK(mset::port::crt::mem_copy(block, "xy", 2) == block);
    CHECK(block[0] == 'x' && block[1] == 'y');

    // memmove must handle a forward-overlapping shift in both directions.
    unsigned char seq[6] = {0, 1, 2, 3, 4, 5};
    mset::port::crt::mem_move(seq + 1, seq, 5);
    CHECK(seq[0] == 0 && seq[1] == 0 && seq[2] == 1 && seq[5] == 4);
    unsigned char seq2[6] = {0, 1, 2, 3, 4, 5};
    mset::port::crt::mem_move(seq2, seq2 + 1, 5);
    CHECK(seq2[0] == 1 && seq2[4] == 5 && seq2[5] == 5);
}

MSET_TEST(port_crt_strsafe) {
    char buf[8] = {};
    CHECK(mset::port::crt::str_copy_worker_a(buf, sizeof(buf), "abc") == 0);
    CHECK(std::strcmp(buf, "abc") == 0);
    // STRSAFE_E_INSUFFICIENT_BUFFER must be reported, and the buffer remains
    // NUL-terminated and truncated to cap-1 characters.
    char small[4] = {};
    long rc = mset::port::crt::str_copy_worker_a(small, sizeof(small), "abcdef");
    CHECK(rc == static_cast<long>(0x8007007AL));
    CHECK(std::strcmp(small, "abc") == 0);
    CHECK(mset::port::crt::str_copy_worker_a(nullptr, 0, "x") ==
          static_cast<long>(0x80070057L));
}

MSET_TEST(port_platform_rects) {
    mset::port::plt::Rect rc;
    CHECK(mset::port::plt::set_rect(&rc, 10, 20, 110, 220));
    CHECK(rc.right == 110 && rc.bottom == 220);
    CHECK(mset::port::plt::inflate_rect(&rc, 5, 5));
    CHECK(rc.left == 5 && rc.top == 15 && rc.right == 115 && rc.bottom == 225);
    CHECK(mset::port::plt::pt_in_rect(&rc, mset::port::plt::Point{100, 200}));
    // Right/bottom edges are exclusive, like user32's PtInRect.
    CHECK(!mset::port::plt::pt_in_rect(&rc, mset::port::plt::Point{115, 200}));
    CHECK(!mset::port::plt::pt_in_rect(&rc, mset::port::plt::Point{4, 20}));

    mset::port::plt::Rect du{0, 0, 40, 16};
    mset::port::plt::Rect px;
    CHECK(mset::port::plt::map_dialog_rect(&du, 8, 16, &px));
    CHECK(px.right == 80 && px.bottom == 32);
}

MSET_TEST(port_gmp_mpf) {
    BigFloat::precision_limbs = 24;
    mset::port::gmp::Mpf a, b, c;
    mset::port::gmp::gmpf_init2(&a, 240);
    mset::port::gmp::gmpf_init2(&b, 240);
    mset::port::gmp::gmpf_init2(&c, 240);
    mset::port::gmp::gmpf_set_ui(&a, 7);
    mset::port::gmp::gmpf_set_ui(&b, 5);
    mset::port::gmp::gmpf_add(&c, &a, &b);
    CHECK(std::fabs(c.v.to_double() - 12.0) < 1e-12);
    mset::port::gmp::gmpf_mul(&c, &a, &b);
    CHECK(std::fabs(c.v.to_double() - 35.0) < 1e-12);
    mset::port::gmp::gmpf_sub(&c, &a, &b);
    CHECK(std::fabs(c.v.to_double() - 2.0) < 1e-12);

    // hi + lo reproduces the value's leading two doubles.
    mset::port::gmp::Mpf x;
    mset::port::gmp::gmpf_init2(&x, 240);
    x.v = BigFloat::from_string("1.2345678901234567890123456789");
    double hi = mset::port::gmp::mpf_hi(&x);
    double lo = mset::port::gmp::mpf_lo(&x);
    CHECK(std::fabs((hi + lo) - x.v.to_double()) < 1e-15);
}

MSET_TEST(port_gl_window) {
    CHECK(mset::port::gl::init());
    mset::port::gl::Monitor* mon = mset::port::gl::get_primary_monitor();
    CHECK(mon != nullptr);
    mset::port::gl::Window* w = mset::port::gl::create_window(320, 200, "seed", mon, nullptr);
    CHECK(w != nullptr);
    if (w) {
        CHECK(w->width == 320 && w->height == 200);
        CHECK(!mset::port::gl::window_should_close(w));
        mset::port::gl::set_window_should_close(w, true);
        CHECK(mset::port::gl::window_should_close(w));
        mset::port::gl::destroy_window(w);
    }
    double t0 = mset::port::gl::get_time();
    double t1 = mset::port::gl::get_time();
    CHECK(t1 >= t0);
    CHECK(t1 > 0.0);
    // A null window is reported as closed, matching glfwWindowShouldClose on a
    // null handle being an error path in glfw; the headless model makes it a
    // defined "closed" result.
    CHECK(mset::port::gl::window_should_close(nullptr));
}

MSET_TEST(port_video_io) {
    mset::port::video::FormatContext* ctx =
        mset::port::video::alloc_output_context2(nullptr, "seed_avi.avi");
    CHECK(ctx != nullptr);
    if (ctx) {
        CHECK(std::strcmp(ctx->oformat->name, "avi") == 0);
        mset::port::video::IOContext* io = nullptr;
        CHECK(mset::port::video::io_open(&io, "port_seed_io.bin", 1) == 0);
        CHECK(io != nullptr && io->file != nullptr);
        const char payload[] = "seed";
        CHECK(std::fwrite(payload, 1, sizeof(payload) - 1, io->file) == sizeof(payload) - 1);
        CHECK(mset::port::video::io_close(io) == 0);
        mset::port::video::free_format_context(ctx);
    }
    CHECK(mset::port::video::guess_format(nullptr, "x.mjpg", nullptr) != nullptr);
    CHECK(mset::port::video::guess_format("nope", nullptr, nullptr) == nullptr);
}

MSET_TEST(port_app_actions) {
    mset::app::AppState app;
    app.set_status("start");
    CHECK(app.status == "start");
    CHECK(app.mode == mset::app::kModeNormal);
    mset::app::button0(app, 0);
    CHECK(app.mode == mset::app::kModeOverlay);
    mset::app::toggleinvert(app, 0);
    CHECK(app.invert);
    CHECK(app.req.invert);
    CHECK(!mset::app::AppState().invert);
    CHECK(app.needs_redraw);
    mset::app::togglehighres(app, 0);
    CHECK(app.highres);
    CHECK(app.nav_flags == (8 + 0x2000));
    CHECK(app.mode == mset::app::kModeNormal);
    mset::app::resetbutton(app, 0);
    CHECK(app.ntouches == 1);
    CHECK(app.view.zoom_exp10 == 0);
    CHECK(app.view.zoom_mantissa == 1.0);
}
