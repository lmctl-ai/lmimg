#include "image.hpp"
#include <MagickWand/MagickWand.h>

#include <cmath>
#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using lmimg::Color;
using Img = lmimg::Image;

namespace {

std::size_t checks = 0;

void check(bool ok, const std::string &what) {
  ++checks;
  if (!ok)
    throw std::runtime_error("scientific check failed: " + what);
}

void close(double got, double want, double tol, const std::string &what) {
  check(std::isfinite(got) && std::abs(got - want) <= tol,
        what + " got=" + std::to_string(got) + " want=" + std::to_string(want));
}

bool near(Color a, Color b, double tol = 1e-6) {
  return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol &&
         std::abs(a.b - b.b) <= tol && std::abs(a.a - b.a) <= tol;
}

void codec_roundtrip(const Img &source, const fs::path &dir,
                     const std::string &format, const std::string &suffix) {
  const fs::path path = dir / ("roundtrip." + suffix);
  source.save(path.string(), format);
  check(fs::file_size(path) > 0, format + " writer produced an empty file");
  Img decoded = Img::open(path.string());
  check(decoded.width() == source.width() &&
            decoded.height() == source.height(),
        format + " roundtrip changed dimensions");
  // Lossless formats preserve every channel. JPEG is intentionally checked with
  // a codec-appropriate error bound because its quantization is lossy.
  const double similarity = source.ssim(decoded);
  close(similarity, 1.0, format == "JPEG" ? 0.08 : 1e-8,
        format + " roundtrip SSIM");
}

void check_animation_gif(const fs::path &path) {
  MagickWand *wand = NewMagickWand();
  check(wand != nullptr, "GIF decoder allocation");
  if (MagickReadImage(wand, path.string().c_str()) == MagickFalse) {
    DestroyMagickWand(wand);
    throw std::runtime_error("GIF decoder could not read animation");
  }
  check(MagickGetNumberImages(wand) == 2, "GIF keeps both animation frames");
  auto sample = [&](size_t index, const char *label) {
    check(MagickSetIteratorIndex(wand, index) == MagickTrue, label);
    PixelWand *pixel = NewPixelWand();
    check(pixel != nullptr, "GIF pixel allocation");
    check(MagickGetImagePixelColor(wand, 0, 0, pixel) == MagickTrue,
          "GIF pixel read");
    const double red = PixelGetRed(pixel);
    DestroyPixelWand(pixel);
    return red;
  };
  const double first = sample(0, "GIF first frame");
  const size_t first_delay = MagickGetImageDelay(wand);
  const double second = sample(1, "GIF second frame");
  const size_t second_delay = MagickGetImageDelay(wand);
  DestroyMagickWand(wand);
  check(std::abs(first - second) > 0.5, "GIF frames retain different pixels");
  check(first_delay == 5 && second_delay == 7, "GIF frame delays");
}

} // namespace

int main(int argc, char **argv) {
  try {
    const fs::path dir = argc > 1
                             ? fs::path(argv[1])
                             : fs::temp_directory_path() / "lmimg-scientific";
    fs::create_directories(dir);

    const double q64 = 64.0 / 255.0, q128 = 128.0 / 255.0, q192 = 192.0 / 255.0;
    Img solid = Img::create(32, 24, {q64, q128, q192, 1.0});
    check(solid.width() == 32 && solid.height() == 24, "create dimensions");
    const Color solid_pixel = solid.pixel(0, 0);
    check(near(solid_pixel, {q64, q128, q192, 1.0}, 0.002),
          "create fill color r=" + std::to_string(solid_pixel.r) +
              " g=" + std::to_string(solid_pixel.g) +
              " b=" + std::to_string(solid_pixel.b) +
              " a=" + std::to_string(solid_pixel.a));
    auto st = solid.stats();
    check(st.pixels == 768, "pixel count");
    close(st.mean.r, q64, 0.002, "constant mean red");
    close(st.mean.g, q128, 0.002, "constant mean green");
    close(st.mean.b, q192, 0.002, "constant mean blue");
    close(st.standard_deviation.r, 0.0, 1e-12, "constant standard deviation");
    close(st.minimum.b, q192, 0.002, "constant minimum");
    close(st.maximum.a, 1.0, 1e-12, "constant maximum alpha");
    check(solid.ssim(solid) > 1.0 - 1e-12, "SSIM identity");
    check(std::isinf(solid.psnr(solid)) || solid.psnr(solid) > 100.0,
          "PSNR identity");
    Img black = Img::create(4, 4, {0, 0, 0, 1});
    Img white = Img::create(4, 4, {1, 1, 1, 1});
    close(black.psnr(white), 10.0 * std::log10(4.0 / 3.0), 0.01,
          "PSNR black-white known pair");
    check(black.ssim(white) < 0.01, "SSIM black-white known pair");

    Img colors = Img::create(3, 1, {0, 0, 0, 1});
    colors.set_pixel(0, 0, {1, 0, 0, 1})
        .set_pixel(1, 0, {0, 1, 0, 1})
        .set_pixel(2, 0, {0, 0, 1, 1});
    colors.grayscale();
    close(colors.pixel(0, 0).r, 0.2126, 0.002, "sRGB luminance red");
    close(colors.pixel(1, 0).r, 0.7152, 0.002, "sRGB luminance green");
    close(colors.pixel(2, 0).r, 0.0722, 0.002, "sRGB luminance blue");
    close(colors.pixel(1, 0).g, colors.pixel(1, 0).r, 1e-12,
          "grayscale equal channels");

    Img feature = Img::create(5, 5, {1, 0, 0, 1});
    feature.curves({0, 0, 0.5, 0.5, 1, 1});
    close(feature.pixel(2, 2).r, 1.0, 0.002, "identity curve");
    Img hue = Img::create(1, 1, {1, 0, 0, 1});
    hue.hsl(120, 1, 1);
    check(hue.pixel(0, 0).g > 0.95 && hue.pixel(0, 0).r < 0.05,
          "HSL hue rotation");
    Img mixed = Img::create(1, 1, {1, 0, 0, 1});
    mixed.channel_mix({0, 0, 1, 0, 1, 0, 1, 0, 0});
    close(mixed.pixel(0, 0).b, 1.0, 0.002, "channel matrix");
    Img dot = Img::create(5, 5, {0, 0, 0, 1});
    dot.set_pixel(2, 2, {1, 1, 1, 1}).morphology("dilate", 1);
    check(dot.pixel(1, 2).r > 0.95 && dot.pixel(0, 0).r < 0.05,
          "morphology dilation");
    Img degenerate = Img::create(1, 1, {0.5, 0.25, 0.75, 1});
    check(std::isfinite(degenerate.clone().tilt_shift(2, 0, 1).pixel(0, 0).r),
          "tilt shift 1x1 remains finite");
    check(std::isfinite(degenerate.clone().vignette().pixel(0, 0).r),
          "vignette 1x1 remains finite");
    check(std::isfinite(degenerate.clone().unsharp(1, 1, 1, 0).pixel(0, 0).r),
          "unsharp 1x1 remains finite");
    check(std::isfinite(degenerate.clone().morphology("median", 1).pixel(0, 0).r),
          "morphology 1x1 remains finite");
    Img mask = Img::create(5, 5, {0, 0, 0, 0});
    mask.set_pixel(2, 2, {1, 1, 1, 1});
    Img masked = Img::create(5, 5, {1, 0, 0, 1});
    masked.apply_mask(mask);
    close(masked.pixel(2, 2).a, 1.0, 0.002, "mask center");
    close(masked.pixel(0, 0).a, 0.0, 0.002, "mask outside");
    Img selected = Img::create(5, 5, {1, 0, 0, 1});
    selected.selection(1, 1, 2, 2);
    close(selected.pixel(1, 1).a, 1.0, 0.002, "selection inside");
    close(selected.pixel(0, 0).a, 0.0, 0.002, "selection outside");
    Img healed = Img::create(5, 5, {0.2, 0.4, 0.6, 1});
    healed.set_pixel(2, 2, {1, 1, 1, 1}).heal(2, 2, 1, 1, 1);
    close(healed.pixel(2, 2).r, 0.2, 0.02,
          "healing fills region from boundary");
    Img geometry = Img::pattern(9, 9, "x", "y", "0.3", "1");
    check(geometry.clone().perspective(0, 0, 1, 0, 1, 1, 0, 1).ssim(geometry) >
              0.99,
          "identity perspective");
    check(geometry.clone().lens(0).ssim(geometry) > 0.99, "identity lens");

    Img quarter = Img::create(3, 2, {1, 0, 0, 1}).rotate(90);
    check(quarter.width() == 2 && quarter.height() == 3,
          "quarter-turn dimensions");
    Img large_angle = Img::create(3, 2, {1, 0, 0, 1}).rotate(1e308);
    check(large_angle.width() > 0 && large_angle.width() <= 8 &&
              large_angle.height() > 0 && large_angle.height() <= 8,
          "finite huge angle normalizes before trigonometry");
    Img ramp = Img::pattern(17, 9, "x", "y", "0.25", "1");
    close(ramp.pixel(0, 0).r, 0.0, 1e-12, "pattern origin");
    close(ramp.pixel(16, 8).r, 1.0, 1e-12, "pattern x endpoint");
    close(ramp.pixel(16, 8).g, 1.0, 1e-12, "pattern y endpoint");
    close(ramp.pixel(8, 4).b, 64.0 / 255.0, 0.002, "pattern expression");

    Img levels = Img::create(1, 1, {0.5, 0.5, 0.5, 1});
    levels.levels(0.0, 1.0, 1.0);
    close(levels.pixel(0, 0).r, 128.0 / 255.0, 0.002, "identity levels");
    Img sepia = Img::create(1, 1, {0.0, 0.0, 1.0, 1});
    sepia.sepia(0.0);
    close(sepia.pixel(0, 0).b, 1.0, 1e-12,
          "zero sepia strength preserves input");
    sepia.sepia(100);
    close(sepia.pixel(0, 0).r, .189, .003, "sepia matrix red known answer");
    close(sepia.pixel(0, 0).b, .131, .003, "sepia matrix blue known answer");

    Img impulse = Img::create(9, 9, {0, 0, 0, 1});
    impulse.set_pixel(4, 4, {1, 1, 1, 1});
    Img blurred = impulse.clone().blur(2.0, 1.0);
    check(blurred.pixel(4, 4).r < 1.0 && blurred.pixel(4, 4).r > 0.0,
          "blur lowers impulse peak");
    check(blurred.pixel(3, 4).r > 0.0, "blur spreads impulse");
    Img constant = Img::create(9, 9, {0.4, 0.2, 0.8, 1});
    Img constant_blur = constant.clone().blur(3.0, 2.0);
    check(constant_blur.ssim(constant) > 1.0 - 1e-8,
          "blur preserves constant image");
    Img edges = constant.clone().edges();
    close(edges.stats().mean.r, 0.0, 1e-12, "edges of constant image");

    Img alpha_probe = Img::create(9, 9, {0.4, 0.2, 0.8, 128.0 / 255.0});
    alpha_probe.levels(0.1, 1.2, 0.9)
        .blur(2, 1)
        .sharpen(1, 1)
        .edges(1)
        .sepia(80);
    close(alpha_probe.pixel(4, 4).a, 128.0 / 255.0, 1e-12,
          "color filters preserve alpha");
    Img backdrop = Img::create(2, 2, {0.2, 0.4, 0.6, 1});
    Img foreground = Img::create(2, 2, {0.5, 0.25, 0.75, 1});
    Img multiplied = backdrop.clone().blend(foreground, "multiply", 1);
    close(multiplied.pixel(0, 0).r, 0.1, 0.006, "multiply red known pair");
    close(multiplied.pixel(0, 0).b, 0.45, 0.006, "multiply blue known pair");
    Img screened = backdrop.clone().blend(foreground, "screen", 1);
    close(screened.pixel(0, 0).r, 0.6, 0.006, "screen red known pair");
    close(screened.pixel(0, 0).b, 0.9, 0.006, "screen blue known pair");
    const std::array<std::pair<std::string, double>, 7> modes = {{{
        "overlay", 0.24}, {"difference", 0.4}, {"darken", 0.2},
        {"lighten", 0.6}, {"addition", 0.8}, {"subtract", 0.0},
        {"over", 0.6}}};
    for (const auto &[mode, expected] : modes) {
      Img mode_result = Img::create(1, 1, {0.2, 0.4, 0.6, 1});
      mode_result.blend(Img::create(1, 1, {0.6, 0.3, 0.2, 1}), mode, 1);
      close(mode_result.pixel(0, 0).r, expected, 0.01,
            mode + " blend red known pair");
    }
    Img overlay = Img::create(2, 2, {1, 0, 0, 0.5});
    Img base = Img::create(4, 4, {0, 0, 1, 1});
    base.blend(overlay, "over", 1.0, 1, 1);
    close(base.pixel(1, 1).r, 0.5, 0.02, "alpha over blend red");
    close(base.pixel(1, 1).b, 0.5, 0.02, "alpha over blend blue");
    check(near(base.pixel(0, 0), {0, 0, 1, 1}), "blend leaves outside pixels");
    base.rect(0, 0, 4, 1, {0, 1, 0, 1})
        .circle(2, 2, 1, {1, 1, 0, 1})
        .line(0, 3, 3, 3, {1, 0, 1, 1}, 1.0);
    check(base.stats().maximum.g > 0.9, "drawing primitive");
    const auto before_text = base.ssim(base.clone());
    base.text(1, 2, "lmimg", 10.0, {1, 1, 1, 1});
    close(before_text, 1.0, 1e-12, "pre-text identity");
    check(base.stats().pixels == 16, "text stays inside image dimensions");
    Img lettering = Img::create(96, 40, {0, 0, 0, 1});
    Img blank = lettering.clone();
    lettering.text(4, 28, "GREEN", 20, {0, 1, 0, 1})
        .text(58, 28, "BLUE", 20, {0, 0, 1, 1});
    check(lettering.ssim(blank) < 0.99, "embedded text changes pixels");
    check(lettering.stats().mean.g > lettering.stats().mean.r &&
              lettering.stats().mean.b > lettering.stats().mean.r,
          "embedded text preserves green and blue channels");
    Img clipped_text = Img::create(48, 32, {0.2, 0.3, 0.4, 1});
    Img reference_text = clipped_text.clone();
    clipped_text.text(2, 24, "A", 18, {2, -1, 0.5, 2});
    reference_text.text(2, 24, "A", 18, {1, 0, 0.5, 1});
    check(clipped_text.ssim(reference_text) > 1.0 - 1e-12,
          "text clips out-of-range color and alpha");
    Img transparent_text = reference_text.clone();
    Img transparent_background = reference_text.clone();
    transparent_text.text(2, 24, "A", 18, {1, 0, 0.5, -1});
    check(transparent_text.ssim(transparent_background) > 1.0 - 1e-12,
          "text clips negative alpha to transparent");

    codec_roundtrip(ramp, dir, "PNG", "png");
    codec_roundtrip(ramp, dir, "JPEG", "jpg");
    codec_roundtrip(ramp, dir, "TIFF", "tiff");
    codec_roundtrip(ramp, dir, "WEBP", "webp");
    codec_roundtrip(ramp, dir, "BMP", "bmp");
    codec_roundtrip(ramp, dir, "PNM", "pnm");
    Img gray = Img::create(2, 1, {0.25, 0.25, 0.25, 1});
    gray.save((dir / "gray.pgm").string());
    gray.save((dir / "gray.pbm").string());
    auto magic = [](const fs::path &path) {
      std::ifstream in(path, std::ios::binary);
      std::string header;
      std::getline(in, header);
      return header;
    };
    check(magic(dir / "gray.pgm") == "P5", "PGM writer emits grayscale P5");
    check(magic(dir / "gray.pbm") == "P4", "PBM writer emits bitmap P4");
    codec_roundtrip(ramp, dir, "GIF", "gif");

    Img frame_a = Img::create(12, 8, {1, 0, 0, 1});
    Img frame_b = Img::create(12, 8, {0, 0, 1, 1});
    lmimg::Animation animation;
    animation.add(frame_a, 5).add(frame_b, 7);
    check(animation.size() == 2, "animation frame count");
    const fs::path gif = dir / "animation.gif";
    animation.save(gif.string());
    check(fs::file_size(gif) > 0, "animation GIF writer");
    check(frame_a.pixel(0, 0).r > 0.99 && frame_b.pixel(0, 0).r < 0.01,
          "animation source frames differ");
    close(frame_a.ssim(frame_b),
          (2 * 0.2126 * 0.0722 + 0.0001) /
              (0.2126 * 0.2126 + 0.0722 * 0.0722 + 0.0001),
          1e-10, "SSIM constant-color known pair");
    check_animation_gif(gif);
    Img montage = lmimg::montage({frame_a, frame_b}, 2, 2, {1, 1, 1, 1});
    check(montage.width() == 30 && montage.height() == 12, "montage geometry");

    // v0.1.2 operations against closed-form answers.
    {
      Img ramp3 = Img::create(3, 1, {0, 0, 0, 1});
      ramp3.set_pixel(0, 0, {0.2, 0.2, 0.2, 1})
          .set_pixel(1, 0, {0.5, 0.5, 0.5, 1})
          .set_pixel(2, 0, {0.8, 0.8, 0.8, 1});
      Img thr = ramp3.clone();
      thr.threshold(0.5);
      check(thr.pixel(0, 0).r == 0 && thr.pixel(1, 0).r == 1 &&
                thr.pixel(2, 0).g == 1,
            "threshold is a luma step at the level");
      Img inv = ramp3.clone();
      inv.invert();
      close(inv.pixel(0, 0).r, 0.8, 0.002, "invert 0.2 -> 0.8");
      Img norm = ramp3.clone();
      norm.normalize();
      close(norm.pixel(0, 0).r, 0.0, 1e-9, "normalize lowest -> 0");
      close(norm.pixel(1, 0).r, 0.5, 0.002, "normalize middle stays centered");
      close(norm.pixel(2, 0).r, 1.0, 1e-9, "normalize highest -> 1");
      Img eq = Img::create(4, 1, {0, 0, 0, 1});
      eq.set_pixel(0, 0, {0.1, 0.1, 0.1, 1})
          .set_pixel(1, 0, {0.1, 0.1, 0.1, 1})
          .set_pixel(2, 0, {0.3, 0.3, 0.3, 1})
          .set_pixel(3, 0, {0.9, 0.9, 0.9, 1});
      eq.equalize();
      // cdf = 2,3,4 over 4 pixels; first occupied bin maps to 0: (cdf-2)/2.
      close(eq.pixel(0, 0).r, 0.0, 1e-9, "equalize first bin -> 0");
      close(eq.pixel(2, 0).r, 0.5, 0.002, "equalize middle -> 0.5");
      close(eq.pixel(3, 0).r, 1.0, 1e-9, "equalize last bin -> 1");
      Img cm = ramp3.clone();
      cm.colormap("heat");
      close(cm.pixel(2, 0).r, 1.0, 0.002, "heat colormap bright end is warm");
      check(cm.pixel(0, 0).r > cm.pixel(0, 0).b, "heat colormap dark end is red");
      Img gr = ramp3.clone();
      gr.colormap("gray");
      close(gr.pixel(1, 0).g, 0.5, 0.002, "gray colormap is identity on luma");
      bool bad_map = false;
      try {
        ramp3.clone().colormap("rainbow");
      } catch (const std::invalid_argument &) {
        bad_map = true;
      }
      check(bad_map, "unknown colormap name is rejected");
      Img crgb = Img::create(1, 1, {0.5, 0.5, 0.5, 1});
      crgb.curves_rgb({0, 0, 1, 1}, {0, 0, 0.5, 0.9, 1, 1}, {0, 1, 1, 0});
      close(crgb.pixel(0, 0).r, 0.5, 0.002, "curves_rgb identity red");
      close(crgb.pixel(0, 0).g, 0.9, 0.002, "curves_rgb green control point");
      close(crgb.pixel(0, 0).b, 0.5, 0.002, "curves_rgb inverted blue");
      Img hald = Img::hald(4);
      check(hald.width() == 64 && hald.height() == 64, "hald level 4 is 64x64");
      close(hald.pixel(15, 0).r, 1.0, 1e-9, "hald red axis ends at 1");
      close(hald.pixel(16, 0).g, 1.0 / 15.0, 0.002, "hald green axis step");
      close(hald.pixel(0, 1).g, 4.0 / 15.0, 0.002, "hald rows wrap the green axis");
      close(hald.pixel(0, 4).b, 1.0 / 15.0, 0.002, "hald blue axis step");
      Img probe = Img::create(2, 1, {0.3, 0.6, 0.9, 1});
      probe.set_pixel(1, 0, {0.05, 0.5, 0.95, 0.5});
      Img ident = probe.clone();
      ident.clut(hald);
      check(near(ident.pixel(0, 0), probe.pixel(0, 0), 0.004) &&
                near(ident.pixel(1, 0), probe.pixel(1, 0), 0.004),
            "identity hald clut is a no-op within quantization");
      Img graded_hald = hald.clone();
      graded_hald.invert();
      Img via = probe.clone();
      via.clut(graded_hald);
      Img direct = probe.clone();
      direct.invert();
      check(near(via.pixel(0, 0), direct.pixel(0, 0), 0.01),
            "graded hald reproduces the direct grade");
      bool bad_hald = false;
      try {
        probe.clone().clut(probe);
      } catch (const std::invalid_argument &) {
        bad_hald = true;
      }
      check(bad_hald, "non-hald clut image is rejected");
      Img div = Img::create(1, 1, {0.25, 0.5, 0.0, 1});
      Img den = Img::create(1, 1, {0.5, 0.5, 0.0, 1});
      div.blend(den, "divide");
      close(div.pixel(0, 0).r, 0.5, 0.002, "divide 0.25/0.5");
      close(div.pixel(0, 0).g, 1.0, 0.002, "divide 0.5/0.5");
      close(div.pixel(0, 0).b, 1.0, 0.002, "divide by zero saturates");
      check(Img::blend_mode("divide") && !Img::blend_mode("dodge"),
            "blend mode vocabulary");
      const double tw = Img::text_width("Hello", 32);
      check(tw > 40 && tw < 120, "text_width of Hello at 32px is plausible");
      close(Img::text_width("Hello\nHi", 32), tw, 1e-9,
            "text_width reports the widest line");
      close(Img::text_width("HelloHello", 32), 2 * tw, 1e-9,
            "text_width is additive in advances");
      // Blur must feather alpha without bleeding transparent color.
      Img glow = Img::create(21, 21, {0, 0, 0, 0});
      glow.rect(8, 8, 4, 4, {1, 0, 0, 1});
      Img before_blur = glow.clone();
      close(before_blur.pixel(8, 10).r, 1.0, 1e-9,
            "zero-width stroke leaves no hairline on a rectangle edge");
      glow.blur(0, 1.5);
      const Color g_center = glow.pixel(10, 10), g_edge = glow.pixel(7, 10);
      check(g_center.a < 0.95 && g_center.a > 0.5, "blur feathers center alpha");
      check(g_edge.a > 0.1 && g_edge.a < 0.6, "blur spreads alpha outward");
      close(g_center.r, 1.0, 0.004, "blurred center keeps pure red");
      close(g_edge.r, 1.0, 0.004, "blurred fringe keeps pure red");
      close(glow.pixel(2, 10).a, 0.0, 1e-9, "far pixels stay transparent");
      Img disc = Img::create(21, 21, {1, 1, 1, 1});
      disc.circle(10, 10, 6, {0, 0, 1, 1});
      check(near(disc.pixel(4, 10), {0, 0, 1, 1}, 0.004),
            "zero-width stroke leaves no hairline on a circle edge");
      // Healing follows local shading: a blemish on a linear gradient is
      // restored to the gradient itself (planar fit), not to a flat mean.
      Img slope = Img::pattern(40, 30, "0.2+0.6*x", "0.8-0.5*y", "0.5");
      Img damaged = slope.clone();
      damaged.rect(15, 10, 8, 6, {0, 0, 0, 1});
      damaged.heal(14, 9, 11, 9, 4);
      check(damaged.psnr(slope) > 45, "planar heal restores a gradient");
      check(near(damaged.pixel(19, 12), slope.pixel(19, 12), 0.01),
            "healed center matches the gradient");
      Img n1 = Img::pattern(8, 8, "hash(x,y,1)", "noise(4*x,4*y,2)", "0");
      Img n2 = Img::pattern(8, 8, "hash(x,y,1)", "noise(4*x,4*y,2)", "0");
      check(std::isinf(n1.psnr(n2)), "hash and noise are deterministic");
      check(n1.pixel(0, 0).r != n1.pixel(1, 0).r, "hash varies per pixel");
      auto n_stats = n1.stats();
      check(n_stats.minimum.g >= 0 && n_stats.maximum.g <= 1,
            "noise stays inside [0,1]");
    }
    std::ofstream histogram(dir / "histogram.csv");
    histogram << solid.histogram_csv();
    check(histogram.good(), "histogram CSV");
    std::cout << "scientific image checks passed (" << checks << ")\n";
    return 0;
  } catch (const std::exception &e) {
    std::cerr << e.what() << '\n';
    return 1;
  }
}
