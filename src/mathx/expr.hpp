// ---------------------------------------------------------------------------
// expr.hpp — a small, total, arbitrary-precision expression language for
// fractal iteration formulas.
//
//   z, c, n, z1        current value, constant term, iteration index, previous z
//   i, pi, e           constants
//   any other letter   parameter (slider); single letters only
//
//   + - * / ^  unary -  implicit multiplication (2z, 3(z+1))
//   sin cos tan asin acos atan sinh cosh tanh exp log ln log10 sqrt
//   abs abs2 arg conj re im pow min max floor ceil round sign if
//
// The source is tokenised, parsed by recursive descent into a postfix program
// and evaluated on mx::BC.  Compilation reports position-aware errors, which
// is what the formula panel shows the user.  Evaluation is total: division by
// zero is zero, log(0) is zero, and exp clamps — one divergent pixel must not
// abort a frame.
// ---------------------------------------------------------------------------
#pragma once

#include "complex.hpp"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mx {

struct ExprError : std::runtime_error {
    size_t pos = 0;
    ExprError(const std::string& msg, size_t p)
        : std::runtime_error(msg), pos(p) {}
};

struct Program {
    enum Op : uint8_t {
        CONST = 0, Z, C, Z1, N, PARAM,
        ADD, SUB, MUL, DIV, POW, NEG,
        CALL1, CALL2, CALL3
    };
    struct Ins {
        uint8_t op = 0;
        int16_t arg = 0;
    };
    std::vector<Ins> code;
    std::vector<BC> consts;
    std::vector<std::string> params;
    std::string source;
    bool uses_c = false;
    bool uses_z1 = false;
    bool uses_n = false;
    bool transcendental = false;   // abs/conj/trig/exp/log/div-by-z anywhere
    int degree = 0;                // polynomial degree in z, -1 when not one

    int stack_depth() const;
};

// `digits` is the decimal precision the literals and constants are built at;
// it must be at least the precision the orbit will be computed with.
Program compile(const std::string& source, int digits);

// True when `p` can be evaluated (used by the UI to enable the render button).
bool valid(const Program& p);

BC run(const Program& p, const BC& z, const BC& c, const BC& z1,
       const BigFloat& n, const BigFloat* params, int nparams);

}  // namespace mx
