# v0.1.1 independent-review response


The original independent review remains in the simulation workspace. Review took place during the
v0.1.0 build; disposition below is against the completed release and v0.1.1
changes. Final verification completed: v0.1.1 is released with exactly five assets and green main/tagged five-platform CI.

| Finding | Disposition and reason | Regression in test.sh |
| --- | --- | --- |
| C1 | Resolved before this review follow-up: v0.1.0 is public with all five platforms green. Retained as a live fresh-cache installation regression. | `reviews C1/m8`: raw gradient URL, empty cwd, no host on PATH, PNG magic/dimensions. |
| M1 | Fixed: one-row focus coordinate uses a nonzero denominator. Degenerate filters also have native checks. | `review M1` reproduces the exact 9×1 impulse and asserts center=1. |
| M2 | Fixed by expanding image and layer modes to over, multiply, screen, overlay, difference, darken, lighten, addition, subtract. Straight-alpha compositing remains shared. | `review M2`, review_image.lms and native numerical blend oracles. |
| M3 | Accepted scoped limitation: selections are rectangles; arbitrary image masks remain available. README and note 08 explicitly state this. | `review M3` verifies inside/outside alpha; `review m9` checks selection documentation. |
| m1 | Fixed: DSL and native morphology both accept radius 0–16; 17 fails at the DSL boundary. | `review m1`, including the accepted maximum and rejected next value. |
| m2 | Fixed: PBM extension/format is accepted. | `review m2` loads a hand-written 1×1 PBM, saves it, and verifies P4 magic. |
| m3 | Fixed: PGM/PPM retain distinct encoders; PGM/PBM receive actual grayscale conversion rather than a colorspace tag. | `review m3` checks P5/P6 headers and red→Rec.709 grayscale. |
| m4 | Fixed: text ink RGBA is clipped before alpha compositing. | `review m4` plus native comparisons with explicitly clipped and negative-alpha ink. |
| m5 | Fixed: missing image files report `cannot open image file` without raw filesystem exception wording. | `review m5` checks exit 3, JSON, one stderr line and concise message. |
| m6 | Removed the two unreachable multi-frame guards. The benign concurrent-mutation observation is retained as an explicit input-stability limitation: atomic snapshots are not promised; dimensions are still validated before/after decode. No claim is made that a race was eliminated. | `review m6` verifies the supported first-frame behavior of a two-frame GIF. |
| m7 | Fixed completeness: full pinned libwebp PATENTS grant is included in embedded notices, plus a disclosure that optional GPL-bearing tooling/docs in upstream trees are not compiled or linked. | `review m7` compares the complete patent text and disclosure; existing notices parity and delegate/static audits remain. |
| m8 | Fixed: LMIMG_TEST_NETWORK now gates an actual public autorun integration check; unset emits an explicit skip. CI enables it. | `reviews C1/m8` runs the gated network path. |
| m9 | Fixed: README adds healing, dimensions/pixels, builder methods, animation size and utility surface. | `review m9` verifies the specifically omitted names remain documented. |
| m10 | Valid semantic hardening, fixed: malformed loop operand kinds previously succeeded. A marked generated SDK build copy now rejects non-bool while conditions and non-int range operands; the original SDK bytes/hash stay unchanged. | `review m10` runs hand-crafted v5 bytecode and requires exit 3/JSON/one stderr line. |
| m11 | Not a newly introduced code defect, as the review states. Retained runtime-license policy: full terms and public source/build/relink recipe pointers remain embedded. Distribution without those materials would require a separate review. | `review m11` checks inventory/pointers; existing notices parity and static-link checks remain. |

Gallery expanded from eight to thirteen captioned PNG examples plus GIF, including
a shaded procedural photo-style grade, aspect-preserving batch outputs, watermark,
contact-sheet variants and before/after delivery panels.

### Final verification

- Release: https://github.com/lmctl-ai/lmimg/releases/tag/v0.1.1 (exactly five executable assets).
- Main CI: https://github.com/lmctl-ai/lmimg/actions/runs/40017099712 — all five green.
- Tagged CI/release: https://github.com/lmctl-ai/lmimg/actions/runs/40019405293 — all five builds and publication green.
- Local complete network-enabled test.sh: 31 groups, 107 native checks, 16 DSL programs; final measured run 24.66 seconds, peak RSS 2,120,044 KiB. macOS/Windows run 30 groups because the extra default-budget virtual-limit check is Linux-only.
- Every one of the 13 raw public URLs passed in a separate empty directory with no lmimg on PATH. A new private cache installed v0.1.1; cached/downloaded Linux SHA-256 both equal `234427c9e488ab1fac5892f82f52abd9c27de09ece2303adf1e2490f58ac19ec`.
- Thirteen PNGs plus the GIF match committed images byte for byte; batch output files preserve 180×240, 210×280 and 240×320 sizes.
- Downloaded release also passes review_image.lms (including center=1 for the one-row focus repro) and all four malformed VM loop fixtures. Embedded notices match the source exactly.
- CI retains compiler 0.2.369 and uses an isolated lmctl 0.2.401 for autorun, because the older CLI predates that command.

No review implementation/release work remains. M3/m6 scope boundaries and m11's
existing distribution policy remain explicitly documented rather than claimed as
new functionality or a new legal determination.
