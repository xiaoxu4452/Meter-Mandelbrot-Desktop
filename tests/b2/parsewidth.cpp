// Reproduce the desktop shell's centre-parsing trap at 1e100.
//
// BigFloat::from_string() truncates to the thread-local working width, and the
// shell used to parse the page's decimal centre BEFORE recomputing that width
// for the new frame.  A preceding shallow view leaves ~10 limbs (90 digits),
// so a 180-digit 1e100 centre silently lost its low 90 digits.  The frame
// still rendered something, which is why the symptom was a *small* structural
// drift rather than an obvious blank.
#include "bigfloat.hpp"

#include <cstdio>
#include <string>

static int last_nonzero_decimal(const std::string& fixed) {
    int last = -1;
    for (int i = (int)fixed.size() - 1; i >= 0; --i) {
        if (fixed[(size_t)i] >= '1' && fixed[(size_t)i] <= '9') { last = i; break; }
    }
    return last;
}

int main() {
    // 180 fractional digits, all significant, like the GMP view bridge emits.
    std::string s = "-0.";
    for (int i = 0; i < 180; ++i) s.push_back(char('0' + 1 + (i * 7 + i / 9) % 9));
    const std::string want = s;

    BigFloat::precision_limbs = 10;      // what a shallow frame leaves behind
    const BigFloat narrow = BigFloat::from_string(want);
    const std::string got_narrow = narrow.to_fixed(180);

    BigFloat::precision_limbs = 128;     // parse at the frame's own width
    const BigFloat wide = BigFloat::from_string(want);
    const std::string got_wide = wide.to_fixed(180);

    /* "-0." occupies indices 0..2, so fractional digit k sits at index 2+k. */
    const int kept_narrow = last_nonzero_decimal(got_narrow) - 2;
    const int kept_wide = last_nonzero_decimal(got_wide) - 2;
    std::printf("input             : 180 significant fractional digits\n");
    std::printf("legacy limbs=10   : %d digits kept, %d low digits LOST\n",
                kept_narrow, 180 - kept_narrow);
    std::printf("fixed  limbs=128  : %d digits kept, %d low digits lost\n",
                kept_wide, 180 - kept_wide);
    std::printf("byte-identical to input: narrow=%s wide=%s\n",
                got_narrow == want ? "yes" : "no", got_wide == want ? "yes" : "no");
    return (got_wide == want && got_narrow != want) ? 0 : 1;
}
