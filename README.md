# lmimg

Image editing, compositing, and procedural graphics from scripts.

| A gradient becomes a poster | Light, pigment, and transparency |
| --- | --- |
| ![Procedural gradient with an annotated color study](docs/images/gradient_studio.png)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/gradient_studio.lms` | ![Original and screen blend](docs/images/blend_modes.png)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blend_modes.lms` |
| ![Original and graded tones](docs/images/levels_lab.png)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/levels_lab.lms` | ![Original and sepia print treatment](docs/images/sepia_poster.png)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/sepia_poster.lms` |
| ![Gaussian blur softens geometric boundaries](docs/images/blur_reveal.png)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blur_reveal.lms` | ![Edges extracted from a synthetic geometric scene](docs/images/edge_map.png)<br>`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/edge_map.lms` |

Install [lmctl](https://github.com/lmctl-ai/lmctl), then copy any caption above into a terminal. Each script installs its bound host as needed and writes `images/<example>.png` in the current directory. Inputs are generated in the script; no photographs or additional files are required.

```sh
lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/gradient_studio.lms
```

The [v0.1.0 release](https://github.com/lmctl-ai/lmimg/releases/tag/v0.1.0) provides static binaries for Linux x64/arm64, macOS arm64/x64, and Windows x64. macOS uses system libraries. No ImageMagick installation, font directory, or codec DLL is needed.

```lms
p = _dsl_ cli="lmimg" install="github:lmctl-ai/lmimg@latest";
let original = img.pattern(440, 260, "x", "y", "0.2", "1");
let edited = original.copy().levels(0.1, 0.8, 0.9);
let panel = img.panel(original, edited, 4);
panel.save("images/comparison.png");
img.assert_close(original.ssim(original), 1.0, 0.000001);
end _dsl_
savedsl(p, "comparison.dslb");
r = system ${dsl_cli(p)} --dsl comparison.dslb;
assert(r.code == 0);
```

All image edits mutate their receiver and return it for chaining. Use `copy()` before editing when the original must remain available. Dimensions and coordinates use pixels; RGBA channels are normalized to `[0,1]`. Storage is 8 bits per channel, so fractional colors are rounded to the nearest representable byte. Expressions in `pattern` use normalized `x` and `y` coordinates and the arithmetic/functions documented in [the expression note](durable-memory/03-schema-and-numerics.md).

| Task | API |
| --- | --- |
| Create / load | `img.canvas(w,h,r,g,b,a)`, `img.pattern(w,h,R,G,B,A)`, `img.open(path)` |
| Copy / save | `image.copy()`, `image.save(path)` |
| Geometry | `resize`, `crop`, `rotate`, `flip`, `perspective`, `lens` |
| Color | `levels`, `curves`, `hsl`, `channelmix`, `grayscale`, `sepia` |
| Filters | `blur`, `sharpen`, `edges`, `morphology`, `unsharp`, `vignette`, `tilt_shift` |
| Compositing | `blend`, masks, selections, layers with per-layer opacity |
| Drawing | `rect`, `circle`, `line`, `text` with an embedded font |
| Measurements | `pixel`, `mean`, `stddev`, `min`, `max`, `histogram`, `psnr`, `ssim` |
| Contact sheets / GIF | `img.panel(a,b,gap)`, `img.animation().add(frame,delay).save(path)` |
| Metadata | `metadata(key,value)`, `property(key)` |

`lmimg --dsl-signatures` lists exact parameter types and overloads; `--dsl-schema --json` prints the compiler schema. PNG, JPEG, TIFF, WebP, GIF, BMP and PNM are supported local raster formats. Animated output is GIF; delays are centiseconds. `open` reads the first frame of an animated input. Text supports ASCII through the embedded Roboto font. Comparison requires equal dimensions. PSNR compares normalized RGBA and is positive infinity for identical images. SSIM uses one global luma window with sample covariance and constants 0.01² and 0.03²; it is a compact image statistic rather than a multiscale perceptual model.

### Examples

| Story | Run from any empty directory |
| --- | --- |
| Procedural color studio | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/gradient_studio.lms` |
| Screen compositing | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blend_modes.lms` |
| Recovering tonal contrast | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/levels_lab.lms` |
| A warm print treatment | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/sepia_poster.lms` |
| Softening geometric boundaries | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blur_reveal.lms` |
| Finding boundaries | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/edge_map.lms` |
| A contact sheet | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/thumbnail_grid.lms` |
| A moving color wave | `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/animation_wave.lms` |

![A procedural color wave](docs/images/animation_wave.gif)

`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/animation_wave.lms`

Regenerate the committed gallery with `examples/render-all.sh`. Every example compiles and executes itself, checks its results, and prints its output path.

### Relationship to GIMP

lmimg is a scripting host with a small, original MIT implementation of editing ideas such as layers, masks, curves, selections, healing-lite, perspective/lens transforms, and selective-focus filters. It does not link GIMP or GEGL and does not use their source. Healing-lite performs a local patch correction; it is not a content-aware reconstruction engine. This project has no GUI and does not implement GIMP's project format or plug-in ecosystem.

ImageMagick supplies the static raster and codec engine. Its license and the permissive licenses for the pinned codecs and embedded font are included in [THIRD_PARTY_NOTICES](THIRD_PARTY_NOTICES), also available verbatim through `lmimg --notices`.

### Resource limits

The default total memory budget is **4096 MiB**. Set `LMIMG_MAX_MEMORY_MB` to a positive whole number of MiB to change it. Invalid values exit 2. The host counts C++ allocations and conservatively charges native image storage, checking known temporary peaks before allocation. Exceeding the budget exits 3 with a JSON error on stdout and one stderr line naming `LMIMG_MAX_MEMORY_MB`. Image caches cannot spill to disk. Large images and retained copies share the same budget.

The scripting-only command surface is:

```text
lmimg --dsl FILE [FILE ...] [--dry-run]
lmimg --version
lmimg --notices
lmimg --dsl-schema [--json]
lmimg --dsl-signatures
```

Usage errors exit 2; bytecode-load and execution failures exit 3 with JSON on stdout and a diagnostic on stderr. A dry run verifies bytecode without running image operations.

### Build and verification

```sh
scripts/sync-deps.sh
scripts/sync-sdk.sh --verify
./test.sh
```

CMake builds pinned source dependencies into one executable. `test.sh` checks native pixel/codec/math oracles, DSL/compiler signatures, examples, byte-identical reruns, errors, notices, static linkage, and memory-limit failures. CI runs the same suite on all five release platforms. See [durable-memory](durable-memory/index.md) for pins, implementation decisions, measured results, and release evidence.
