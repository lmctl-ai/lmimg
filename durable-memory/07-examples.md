# Examples and gallery

The lmimg examples are self-contained procedural image stories. Each binds the
host with `_dsl_ cli="lmimg" install="github:lmctl-ai/lmimg@latest"`, saves its
bytecode, executes it, and writes `images/<name>.png` in the current directory.
This makes each raw GitHub example runnable from an empty directory with:

`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/<name>.lms`

The checked-in `docs/images/` gallery is generated from those same programs.
Images are kept near 900 pixels wide and below 300 KiB; all content is
procedural and contains no copyrighted photographs. Gallery validation checks
PNG/GIF magic bytes, file-size limits, example links, self-executing output,
and byte-identical reruns for deterministic images. Native scientific tests
cover pixel geometry, color transforms, blur behavior, blend compositing,
statistics, histogram output, PSNR/SSIM, codec round trips, text bounds,
montage geometry, and GIF animation.

## v0.1.1 user workflows

Five additional self-contained examples bring the gallery to 13 PNGs plus GIF:

- `procedural_photo`: smoothly shaded procedural product/fruit study with
  deterministic texture, exposure/contrast, white balance, vignette and unsharp.
- `watermark_story`: translucent embedded-font watermark on original artwork.
- `batch_resize`: three named deliverables preserve the original 3:4 aspect ratio
  at 180×240, 210×280 and 240×320, with a labeled comparison sheet.
- `before_after_panels`: source and delivery grade side by side.
- `contact_sheet_story`: labeled warm/cool/gold/violet catalogue variants.

Every README image has a raw-URL `lmctl autorun` caption. `test.sh` additionally
uses `LMIMG_TEST_NETWORK=1` to verify a public caption from an empty directory
with the build host removed from PATH; CI enables this integration check.

## v0.1.2 user stories

Seventeen further examples (30 PNG + 2 GIF): see
[10-review-0.1.2.md](10-review-0.1.2.md) for the list and the authoring
lessons (no closures over outer locals in block functions, data exchange
between hosts through files, lmdraw overlays composed by lmimg). The README
gallery is grouped by theme (retouching and looks; documents, labels and
design; measurement, QA and calibration; production; basics). The gallery
checker now validates every `docs/images/*.gif` against an example and a
README caption, not only `animation_wave.gif`.
