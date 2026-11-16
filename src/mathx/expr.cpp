// expr.cpp — see expr.hpp.
#include "expr.hpp"

#include <cctype>
#include <cmath>
#include <cstring>

namespace mx {
namespace {

enum class T { End, Num, Ident, Op };

struct Tok {
    T t = T::End;
    std::string text;
    size_t pos = 0;
};

bool is_ident_start(char c) {
    return std::isalpha((unsigned char)c) != 0 || c == '_';
}
bool is_ident_char(char c) {
    return std::isalnum((unsigned char)c) != 0 || c == '_';
}

std::vector<Tok> lex(const std::string& s) {
    std::vector<Tok> out;
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const char c = s[i];
        if (std::isspace((unsigned char)c)) { ++i; continue; }
        if (c == '#') {
            while (i < n && s[i] != '\n') ++i;
            continue;
        }
        if (std::isdigit((unsigned char)c) ||
            (c == '.' && i + 1 < n && std::isdigit((unsigned char)s[i + 1]))) {
            size_t j = i;
            while (j < n && (std::isdigit((unsigned char)s[j]) || s[j] == '.')) ++j;
            if (j < n && (s[j] == 'e' || s[j] == 'E')) {
                size_t k = j + 1;
                if (k < n && (s[k] == '+' || s[k] == '-')) ++k;
                if (k < n && std::isdigit((unsigned char)s[k])) {
                    while (k < n && std::isdigit((unsigned char)s[k])) ++k;
                    j = k;
                }
            }
            out.push_back({T::Num, s.substr(i, j - i), i});
            i = j;
            continue;
        }
        if (is_ident_start(c)) {
            size_t j = i;
            while (j < n && is_ident_char(s[j])) ++j;
            out.push_back({T::Ident, s.substr(i, j - i), i});
            i = j;
            continue;
        }
        /* Two-character operators first. */
        if (i + 1 < n) {
            const std::string two = s.substr(i, 2);
            if (two == "<=" || two == ">=" || two == "==" || two == "!=") {
                out.push_back({T::Op, two, i});
                i += 2;
                continue;
            }
        }
        if (std::strchr("+-*/^(),<>", c)) {
            out.push_back({T::Op, std::string(1, c), i});
            ++i;
            continue;
        }
        throw ExprError(std::string("unexpected character '") + c + "'", i);
    }
    out.push_back({T::End, "", n});
    return out;
}

enum Fn1 { F_SIN, F_COS, F_TAN, F_ASIN, F_ACOS, F_ATAN, F_SINH, F_COSH, F_TANH,
           F_EXP, F_LOG, F_LOG10, F_SQRT, F_ABS, F_ABS2, F_ARG, F_CONJ,
           F_RE, F_IM, F_FLOOR, F_CEIL, F_ROUND, F_SIGN };
enum Fn2 { F_POW, F_MIN, F_MAX, F_ATAN2 };
enum Fn3 { F_IF };

int fn1_id(const std::string& name) {
    if (name == "sin") return F_SIN;
    if (name == "cos") return F_COS;
    if (name == "tan") return F_TAN;
    if (name == "asin") return F_ASIN;
    if (name == "acos") return F_ACOS;
    if (name == "atan") return F_ATAN;
    if (name == "sinh") return F_SINH;
    if (name == "cosh") return F_COSH;
    if (name == "tanh") return F_TANH;
    if (name == "exp") return F_EXP;
    if (name == "log" || name == "ln") return F_LOG;
    if (name == "log10") return F_LOG10;
    if (name == "sqrt") return F_SQRT;
    if (name == "abs") return F_ABS;
    if (name == "abs2" || name == "norm") return F_ABS2;
    if (name == "arg") return F_ARG;
    if (name == "conj") return F_CONJ;
    if (name == "re" || name == "real") return F_RE;
    if (name == "im" || name == "imag") return F_IM;
    if (name == "floor") return F_FLOOR;
    if (name == "ceil") return F_CEIL;
    if (name == "round") return F_ROUND;
    if (name == "sign" || name == "sgn") return F_SIGN;
    return -1;
}

class Parser {
 public:
    Parser(std::vector<Tok> toks, Program& p, int digits)
        : t_(std::move(toks)), p_(p), digits_(digits) {}

    void parse() {
        if (t_[0].t == T::End) throw ExprError("empty expression", 0);
        parseCompare();
        if (peek().t != T::End)
            throw ExprError("unexpected '" + peek().text + "'", peek().pos);
    }

 private:
    const Tok& peek(size_t ahead = 0) const {
        const size_t i = std::min(i_ + ahead, t_.size() - 1);
        return t_[i];
    }
    Tok take() { return t_[i_ < t_.size() - 1 ? i_++ : i_]; }
    bool is_op(const char* op) const {
        return peek().t == T::Op && peek().text == op;
    }
    void emit(Program::Op op, int arg = 0) {
        p_.code.push_back({(uint8_t)op, (int16_t)arg});
    }

    void parseCompare() {
        parseAdd();
        while (peek().t == T::Op &&
               (peek().text == "<" || peek().text == ">" || peek().text == "<=" ||
                peek().text == ">=" || peek().text == "==" || peek().text == "!=")) {
            const std::string op = take().text;
            parseAdd();
            int code = 0;
            if (op == "<") code = 0;
            else if (op == "<=") code = 1;
            else if (op == ">") code = 2;
            else if (op == ">=") code = 3;
            else if (op == "==") code = 4;
            else code = 5;
            emit(Program::CALL2, (int16_t)(100 + code));   // 100..105 = compare
        }
    }

    void parseAdd() {
        parseMul();
        while (is_op("+") || is_op("-")) {
            const bool plus = take().text == "+";
            parseMul();
            emit(plus ? Program::ADD : Program::SUB);
        }
    }

    bool starts_primary() const {
        const Tok& k = peek();
        return k.t == T::Num || k.t == T::Ident || (k.t == T::Op && k.text == "(");
    }

    void parseMul() {
        parseUnary();
        for (;;) {
            if (is_op("*") || is_op("/")) {
                const bool mul = take().text == "*";
                parseUnary();
                emit(mul ? Program::MUL : Program::DIV);
                if (!mul) p_.transcendental = true;
            } else if (starts_primary()) {
                parseUnary();
                emit(Program::MUL);
            } else {
                break;
            }
        }
    }

    void parseUnary() {
        if (is_op("-")) {
            take();
            parseUnary();
            emit(Program::NEG);
            return;
        }
        if (is_op("+")) {
            take();
            parseUnary();
            return;
        }
        parsePow();
    }

    void parsePow() {
        parsePrimary();
        if (is_op("^")) {
            take();
            parseUnary();          // right associative, allows z^-1
            emit(Program::POW);
            p_.transcendental = true;
        }
    }

    BigFloat number(const Tok& k) {
        try {
            return BigFloat::from_string(k.text);
        } catch (const std::exception&) {
            throw ExprError("bad number '" + k.text + "'", k.pos);
        }
    }

    void emit_const(const BC& v) {
        p_.consts.push_back(v);
        emit(Program::CONST, (int16_t)(p_.consts.size() - 1));
    }

    int param_slot(const std::string& name) {
        for (size_t i = 0; i < p_.params.size(); ++i)
            if (p_.params[i] == name) return (int)i;
        if (p_.params.size() >= 12)
            throw ExprError("too many parameters", 0);
        p_.params.push_back(name);
        return (int)(p_.params.size() - 1);
    }

    void parsePrimary() {
        const Tok k = take();
        if (k.t == T::Num) {
            emit_const(BC{number(k), BigFloat()});
            return;
        }
        if (k.t == T::Op && k.text == "(") {
            parseCompare();
            if (!is_op(")")) throw ExprError("expected ')'", peek().pos);
            take();
            return;
        }
        if (k.t == T::Ident) {
            const std::string& name = k.text;
            if (is_op("(")) {
                take();
                int args = 0;
                if (!is_op(")")) {
                    for (;;) {
                        parseCompare();
                        ++args;
                        if (is_op(",")) { take(); continue; }
                        break;
                    }
                }
                if (!is_op(")")) throw ExprError("expected ')' after arguments", peek().pos);
                take();
                const int f1 = fn1_id(name);
                if (args == 1 && f1 >= 0) {
                    emit(Program::CALL1, (int16_t)f1);
                    if (f1 != F_ABS && f1 != F_ABS2 && f1 != F_ARG && f1 != F_CONJ &&
                        f1 != F_RE && f1 != F_IM)
                        p_.transcendental = true;
                    return;
                }
                if (args == 2 && name == "pow") { emit(Program::CALL2, (int16_t)F_POW); p_.transcendental = true; return; }
                if (args == 2 && name == "min") { emit(Program::CALL2, (int16_t)F_MIN); return; }
                if (args == 2 && name == "max") { emit(Program::CALL2, (int16_t)F_MAX); return; }
                if (args == 2 && name == "atan2") { emit(Program::CALL2, (int16_t)F_ATAN2); p_.transcendental = true; return; }
                if (args == 3 && name == "if") { emit(Program::CALL3, (int16_t)F_IF); return; }
                if (f1 >= 0)
                    throw ExprError("'" + name + "' takes 1 argument, got " +
                                        std::to_string(args), k.pos);
                throw ExprError("unknown function '" + name + "'", k.pos);
            }
            if (name == "z") { emit(Program::Z); return; }
            if (name == "c") { p_.uses_c = true; emit(Program::C); return; }
            if (name == "n") { p_.uses_n = true; emit(Program::N); return; }
            if (name == "z1" || name == "z_1" || name == "zp") {
                p_.uses_z1 = true;
                emit(Program::Z1);
                return;
            }
            if (name == "i") { emit_const(BC{BigFloat(), BigFloat((int64_t)1)}); return; }
            if (name == "pi") {
                emit_const(BC{bm::pi(), BigFloat()});
                return;
            }
            if (name == "e") {
                emit_const(BC{bm::exp(BigFloat((int64_t)1)), BigFloat()});
                return;
            }
            if (name.size() == 1 && std::isalpha((unsigned char)name[0])) {
                emit(Program::PARAM, (int16_t)param_slot(name));
                return;
            }
            throw ExprError("unknown symbol '" + name + "'", k.pos);
        }
        if (k.t == T::End) throw ExprError("unexpected end of expression", k.pos);
        throw ExprError("unexpected '" + k.text + "'", k.pos);
    }

    std::vector<Tok> t_;
    Program& p_;
    int digits_;
    size_t i_ = 0;
};

void apply1(int f, BC& a) {
    switch (f) {
        case F_SIN: a = bc_sin(a); break;
        case F_COS: a = bc_cos(a); break;
        case F_TAN: a = bc_tan(a); break;
        case F_ASIN: a = bc_asin(a); break;
        case F_ACOS: a = bc_acos(a); break;
        case F_ATAN: a = bc_atan(a); break;
        case F_SINH: a = bc_sinh(a); break;
        case F_COSH: a = bc_cosh(a); break;
        case F_TANH: a = bc_tanh(a); break;
        case F_EXP: a = bc_exp(a); break;
        case F_LOG: a = bc_log(a); break;
        case F_LOG10: {
            BC l = bc_log(a);
            const BigFloat ln10 = bm::ln10();
            a = BC{bm::div(l.re, ln10), bm::div(l.im, ln10)};
            break;
        }
        case F_SQRT: a = bc_sqrt(a); break;
        case F_ABS: a = BC{bc_abs(a), BigFloat()}; break;
        case F_ABS2: a = BC{bc_abs2(a), BigFloat()}; break;
        case F_ARG: a = BC{bc_arg(a), BigFloat()}; break;
        case F_CONJ: a = bc_conj(a); break;
        case F_RE: a = BC{a.re, BigFloat()}; break;
        case F_IM: a = BC{a.im, BigFloat()}; break;
        case F_FLOOR: a = BC{bm::floor(a.re), bm::floor(a.im)}; break;
        case F_CEIL: {
            const BigFloat one((int64_t)1);
            a = BC{bm::floor(a.re.add(one)).sub(one), bm::floor(a.im.add(one)).sub(one)};
            break;
        }
        case F_ROUND: {
            const BigFloat half = BigFloat::from_double(0.5);
            const BigFloat one((int64_t)1);
            BC t{bm::floor(a.re.add(half)), bm::floor(a.im.add(half))};
            a = t;
            (void)one;
            break;
        }
        case F_SIGN: {
            const double s = a.re.sign;
            a = BC{BigFloat::from_double(s), BigFloat()};
            break;
        }
        default: break;
    }
}

}  // namespace

int Program::stack_depth() const {
    int d = 0, mx = 0;
    for (const Ins& i : code) {
        switch (i.op) {
            case CONST: case Z: case C: case Z1: case N: case PARAM: ++d; break;
            case ADD: case SUB: case MUL: case DIV: case POW: --d; break;
            case NEG: break;
            case CALL1: break;
            case CALL2: --d; break;
            case CALL3: d -= 2; break;
            default: break;
        }
        if (d > mx) mx = d;
    }
    return mx + 4;
}

Program compile(const std::string& source, int digits) {
    Program p;
    p.source = source;
    if (digits < 20) digits = 20;
    bm::ScopedPrecision guard(digits / 9 + 3);
    Parser parser(lex(source), p, digits);
    parser.parse();
    return p;
}

bool valid(const Program& p) { return !p.code.empty(); }

BC run(const Program& p, const BC& z, const BC& c, const BC& z1,
       const BigFloat& n, const BigFloat* params, int nparams) {
    static thread_local std::vector<BC> st;
    st.clear();
    st.reserve(p.code.size() + 4);
    for (const Program::Ins& in : p.code) {
        switch (in.op) {
            case Program::CONST: st.push_back(p.consts[(size_t)in.arg]); break;
            case Program::Z: st.push_back(z); break;
            case Program::C: st.push_back(c); break;
            case Program::Z1: st.push_back(z1); break;
            case Program::N: st.push_back(BC{n, BigFloat()}); break;
            case Program::PARAM: {
                const int idx = in.arg;
                const BigFloat v = (params && idx >= 0 && idx < nparams)
                                       ? params[idx] : BigFloat();
                st.push_back(BC{v, BigFloat()});
                break;
            }
            case Program::ADD: {
                BC b = std::move(st.back()); st.pop_back();
                st.back() = bc_add(st.back(), b);
                break;
            }
            case Program::SUB: {
                BC b = std::move(st.back()); st.pop_back();
                st.back() = bc_sub(st.back(), b);
                break;
            }
            case Program::MUL: {
                BC b = std::move(st.back()); st.pop_back();
                st.back() = bc_mul(st.back(), b);
                break;
            }
            case Program::DIV: {
                BC b = std::move(st.back()); st.pop_back();
                st.back() = bc_div(st.back(), b);
                break;
            }
            case Program::POW: {
                BC b = std::move(st.back()); st.pop_back();
                st.back() = bc_pow(st.back(), b);
                break;
            }
            case Program::NEG: st.back() = bc_neg(st.back()); break;
            case Program::CALL1: {
                BC a = std::move(st.back()); st.pop_back();
                apply1(in.arg, a);
                st.push_back(std::move(a));
                break;
            }
            case Program::CALL2: {
                BC b = std::move(st.back()); st.pop_back();
                BC a = std::move(st.back()); st.pop_back();
                const int f = in.arg;
                if (f >= 100) {                    // comparison
                    BC r{BigFloat(), BigFloat()};
                    const int c2 = a.re.compare(b.re);
                    bool val = false;
                    switch (f - 100) {
                        case 0: val = c2 < 0; break;
                        case 1: val = c2 <= 0; break;
                        case 2: val = c2 > 0; break;
                        case 3: val = c2 >= 0; break;
                        case 4: val = c2 == 0; break;
                        default: val = c2 != 0; break;
                    }
                    r.re = BigFloat((int64_t)(val ? 1 : 0));
                    st.push_back(r);
                } else if (f == F_POW) {
                    st.push_back(bc_pow(a, b));
                } else if (f == F_MIN) {
                    st.push_back(a.re.compare(b.re) <= 0 ? a : b);
                } else if (f == F_MAX) {
                    st.push_back(a.re.compare(b.re) >= 0 ? a : b);
                } else if (f == F_ATAN2) {
                    st.push_back(BC{bm::atan2(a.im, a.re), BigFloat()});
                }
                break;
            }
            case Program::CALL3: {
                BC c3 = std::move(st.back()); st.pop_back();
                BC b = std::move(st.back()); st.pop_back();
                BC a = std::move(st.back()); st.pop_back();
                (void)c3;
                st.push_back(a.re.is_zero() ? c3 : b);
                break;
            }
            default: break;
        }
    }
    return st.empty() ? BC{} : st.back();
}

}  // namespace mx
