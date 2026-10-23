# v0.1.2 review, enhancement and QA pass — 2026-10-23

Independent pass over the v0.1.1 release (REVIEW_lmimg.md in the simulation
directory holds the earlier findings). Every v0.1.1 remediation claim was
re-verified by running its repro against the release binary installed by
`lmctl autorun` from an empty directory; all 13 public captions ran in 1–3 s
and their PNG/GIF outputs were byte-identical to `docs/images`.

## Pixel oracles (numpy, 16×8 pattern, quantization-aware)

Exact match (≤ 1/255) for: pattern, levels, curves, hsl, sepia, grayscale,
channelmix, all blend modes with partial alpha and offsets (W3C source-over
with blend), morphology erode/dilate/median with edge replication, unsharp,
vignette, crop, flip/flop, rotate 90°, Lanczos resize 2× and ½× against PIL,
the Gaussian impulse response of `blur` (kernel matches
exp(-x²/2σ²) normalised), PSNR, histogram and statistics. `rotate(30°)` gives
ImageMagick's shear extent (20×16 for 16×8) rather than the 18×15 bounding box;
documented, not changed. PNG/JPEG/TIFF/WebP/GIF/BMP/PPM saves are byte-stable
across runs. A 12000² canvas is refused at the 4096 MiB budget with one stderr
line and succeeds at `LMIMG_MAX_MEMORY_MB=8000`.

## Defects found and fixed

- `blur` never touched alpha (`rgb_filter` restored it), so a transparent text
  or shape layer could not become a soft shadow or glow. Now blurs all four
  channels on premultiplied colour (no dark fringes), skipping the extra
  passes when the image is opaque; sharpen/edges keep the RGB-only behaviour.
- `rect`/`circle` drew a 1-px black hairline around every shape because the
  default stroke colour was opaque black at width 0 (visible in every v0.1.1
  gallery image). A zero-width stroke is now transparent.
- `heal` filled the patch with the ring's mean, leaving a flat spot on any
  gradient (0.065 off on a shaded face). It now fits a least-squares plane to
  the ring (closed-form 3×3 normal equations) and reproduces linear gradients
  exactly; falls back to the mean when the ring is degenerate.
- `perspective` and `lens` used nearest-neighbour sampling (ragged text in the
  keystone example); both now sample bilinearly on premultiplied colour, and
  `lens` leaves out-of-frame samples transparent instead of smearing border
  pixels across the corners.
- License hygiene: ImageMagick's configure still auto-detected pango (LGPL),
  graphviz (EPL), zip, dmr, autotrace, dps, flif, fpx, rsvg and uhdr on hosts
  that have them. They are now `--without-*` and CMake fails if any
  `*_DELEGATE` other than PNG/JPEG/TIFF/WEBP/WEBPMUX/ZLIB is defined in
  `config.h`. The delegate audit of the built binary found no foreign symbols.

## API added for the user stories (schema regenerated, tests in
tests/scientific.cpp, tests/oracles.lms and test.sh error paths)

`Image.curves_rgb(Curve,Curve,Curve)`, `threshold(level)`, `invert()`,
`normalize()` (per-channel stretch), `equalize()` (luma histogram
equalisation, RGB scaled by the luma ratio so hue is kept), `colormap(name)`
(gray/heat/viridis/coolwarm anchors, linear), `clut(hald)` (trilinear Hald
CLUT), `Img.hald(level)` (identity CLUT, side level³, level 2–16),
`Img.text_width(text,size)` (advance width with the same rounding as `text`,
widest line), blend/layer mode `divide`, and expression functions
`hash(x,y,seed)` (splitmix of the IEEE bit patterns, platform-identical) and
`noise(x,y,seed)` (quintic-faded value noise). Error paths: unknown colormap,
non-Hald clut, hald level 17, threshold 1.5, `noise(x,y)` arity.

## Examples

Seventeen new stories (30 PNG + 2 GIF total): portrait_retouch, hdr_tone_map,
miniature_tilt_shift, graduated_nd_layers, color_grade_lut,
document_scan_cleanup, barcode_label (EAN-13 encoded and read back),
poster_text, watermark_tiled, image_diff_heatmap, histogram_equalize_plot
(lmimg → lmplot → lmimg), pipeline_diagram (lmimg → lmdraw → lmimg),
salt_pepper_denoise, perspective_whiteboard (inverse homography derived
in-script), lens_undistort (correction coefficient calibrated by sweep),
contact_sheet_shoot, sprite_sheet (+ GIF). Each asserts a measured result.
Grain in document_scan_cleanup and poster_text is quantised to 2–4 levels to
keep the PNGs under the 300 KiB gallery budget.

Lessons for authors: functions inside a `_dsl_` block cannot capture outer
locals (pass images and strings as parameters); `return` must be the last
statement; block built-ins are float/str/sin/cos/sqrt/abs/floor/pow/min/max/
clamp/lerp/int (no exp/log; use `img.eval`); outer-script variables are not
visible inside blocks, so hosts exchange data through files (gnuplot reads
the histogram CSV directly, lmdraw renders an overlay PNG that lmimg opens).
lmdraw's `image` accepts only data URIs, so composition happens in lmimg.

## Release note

The new examples need the 0.1.2 host; `install="github:lmctl-ai/lmimg@latest"`
resolves to it once the v0.1.2 tag is published by the release owner (tags
are never created in this pass).
