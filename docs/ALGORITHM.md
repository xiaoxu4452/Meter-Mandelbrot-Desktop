# Algorithm notes

[中文版](ALGORITHM.zh.md)

## 1. Perturbation

For `z_{n+1} = z_n² + c`, pick a reference point `c0` with an orbit `Z_n`
computed at full precision, and write a pixel as `c = c0 + d0`, `z_n = Z_n + d_n`:

```
Mandelbrot : d_{n+1} = 2 Z_n d_n + d_n² + d0
Julia      : d_{n+1} = 2 Z_n d_n + d_n²          (no constant term)
```

The Julia variant is a frequent source of wrong pictures: `c` is fixed there,
so `d0` is the *initial* perturbation, not a recurrence constant. `RefCache::julia`
switches all three kernels (scalar, AVX2 quad, double-double) and the distance
derivative; the series coefficients are integrated with the same semantics.

Per-pixel values use a float-exp pair (double mantissa + int64 exponent), so a
pixel offset of 1e-60000 does not underflow. Above 1e-250 the exponent cannot
overflow and a plain double path is used. The AVX2 kernel
`iterate_quad_d` processes four pixels per step and is byte-identical to the
scalar one; it is guarded by `#if defined(__AVX2__)`.

## 2. Reference orbit rules (measured, B2–B10)

1. **The reference must outlive every pixel.** A 24×16 probe grid keeps
   promoting long-lived candidates during the frame.
2. **`ref_cap = maxiter` in one shot.** Example 14 genuinely escapes at
   28026…39815 iterations; an 8192 ceiling classified the whole frame as
   interior. The orbit stops as soon as it escapes; increasing the cap in
   ×4 steps only recomputes a prefix.
3. **Eight parallel candidates**, spread apart, keep the longest survivor
   (`build_best()`).
4. **Glitch = `|Z+d| << max(|Z|,|d|)`** — 20 lost bits (`2^-20`). Cancellation
   destroys the perturbation recursion and the pixel is repaired against a
   local reference. Float-exp addition must renormalise the exponent field in
   full (`f2_norm`), not just the mantissa.
5. **Series approximation.** Writing `d_n = A_n d0 + B_n d0² + C_n d0³ + …`,
   the truncation term must be negligible *relative to the first term*:
   ```
   log10|A_N| - log10|A_1| + (N-1)·log10(dmax) < -12
   ```
   Integration closes at `|Z| > 2` (`SaTracker::step`), otherwise a skip can
   land past the escape point. Coefficients depend only on the already stored
   `z_n`, so `sa_replay()` recomputes them afterwards: every orbit is built
   with `sa = 0` and one replay at the end of the staircase. Preview frames
   use order 8, full frames order 16. A pairing guard
   (`ref.rdx == rdx && ref.rdy == rdy`) prevents an unadopted candidate from
   overwriting the elected reference.
6. **Exact eventual period.** The first 64 GMP steps are compared with
   `__gmpf_cmp`; a bit-exact repeat means the orbit is analytically periodic,
   and `orbit.z` is tiled to `maxiter` with zero further full-precision
   squarings. The anchor must be the *exact view centre* (`*p->cr`, `*p->ci`):
   the centre pixel is `center + 0.5px·scale` and is not the Misiurewicz point.
   `RefCache::anchor_x/y` (double, `w/2-0.5`) drives every `d0`; the pairing
   guard still uses integer `rdx/rdy`. For `c=i` the period is 2 from `z_2`.
   `MB_QUASI=1` relaxes the test to CFexp (53-bit) precision but is **off by
   default**: `c=i+1e-6050` only looks periodic, freezing `Z_n` there is wrong.
7. **Julia recurrence** — see §1.
8. **Parse coordinates at the current frame's width.** `BigFloat::from_string`
   uses a `thread_local` limb count; a shallow frame leaves 90 bits, which
   truncates a 180-digit centre at 1e100 and shifts the structure. The host sets
   `precision_limbs` from `log10_zoom + 80` *before* parsing.
9. **Observational iteration budget.** `canvas->maxiter` is the largest
   iteration actually observed; the vertex budget is `known*3+3000`. A
   "starved" frame — `cap>0 && hi===0 && refLen>=cap-1` — jumps to 80000 and
   **repaints once** (`observedIter.repainted`, `refine<=3`), otherwise the
   user is left looking at a truncated black frame.

## 3. Periodic centres and deep coordinates

- `mset misiurewicz --m M --p P`: Newton on `z_{m+p}(c) - z_m(c) = 0`.
  `(2,2) → c=i`, `(3,1) → c=-2`. Components below `10^-(digits-6)` are zeroed
  (otherwise `c=i` prints 92 digits of noise). Any nucleus also solves `g=0`,
  so Newton can converge back to a nucleus.
- `mset nucleus`: atom-domain period detection + Newton on `z_p(c)=0`.
  Example 14 → period 11196, 200 digits 0.2 s / 6100 digits 21 s. Zooming
  into a nucleus is always all-interior — that is the mathematics, not a bug.
- Long coordinates go through `--config view.json` because the Windows command
  line caps at 8191 characters.

## 4. Precision

- `digits = depth10 + 50`, rounded to 64-bit limbs, capped at 1.2 Mbit.
- Reference orbit: GMP `mpf` loaded at runtime (`gmp_bridge.cpp`), with
  `__mpf_struct` treated as an opaque 32-byte block — its fields are never
  read. The built-in base-10^9 BigFloat is the fallback.
- `BigFloat::to_floatexp` must fill 17 significant digits (it used to read only
  two limbs → 10 digits); GMP/BigFloat orbits then agree to ≤4.4e-16.
- **No `std::exp2` / `std::pow` / `std::exp` in the core**: the CRT/libm paired
  with this executable returns garbage for them. Use `pow10_int`,
  `fexp_pow2_frac`, `fexp_exp`. This single fault twice produced a solid-colour
  frame.
- The pixel scale has exactly one definition, `mbc::pixelScaleLog2`, inline in
  `mb_core.h`.

## 5. Verification and cost

Oracle: exact BigFloat kernel at 88 bits vs 268 bits (identical), then `cmp`
compares pixel by pixel. 1e6: 98.71%, 1e20 / 1e38: 100%, 0 unresolved.
Julia 1e6/1e12/1e20/1e38 vs `--exact`: 0 differing pixels.

| View | Cost |
| --- | --- |
| 1e38, 426×240 | 44 ms |
| 1e73, 640×400 | 475 ms |
| 1e6000 `c=i`, 320×200 | 84 ms |
| 1e60000 `c=i`, 160×100 / 200000 iter | 0.65 s |
| 200×120 / 2000 iter: 1e600 / 1e2000 / 1e6000 / 1e20000 | 52 / 98 / 272 / 1050 ms |

The AVX2 quad kernel cuts the main loop from 33 ms to 8 ms; deep frames are
dominated by the reference orbit, not the pixels. GMP costs 22/43/106/463/980
µs per step at 6000/12000/24000/60000/120000 bits — **no FFT threshold is
crossed**, so switching to an FFT multiply is not an optimisation. The 10^60000
wall was the *number* of full-precision steps, not the multiply algorithm.

## 6. Ruled out by measurement (do not re-investigate)

- Zoom jitter is not double cancellation; it is preview/final-pass switching
  and the quantisation grid. Anchor drift was a CDP integer-coordinate artefact.
- Full-frame caching is slower and races on writes; 64px local references need
  243 orbits at 1e38.
- "The 1e78 limit" = `ref_cap` truncation. "1e6000 is flat" = not enough digits
  in the centre string. "GMP needs FFT multiplication" = wrong.
- "A small bright ring / a black export is normal" — that was the broken
  `exp2`.
