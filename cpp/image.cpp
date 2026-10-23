#include "image.hpp"
#include "expr.hpp"
#include "memory_budget.hpp"

#include <MagickWand/MagickWand.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#if __has_include("embedded_font.hpp") && __has_include("stb_truetype.h")
#include "embedded_font.hpp"
#define STBTT_malloc(n, u) (::operator new(n))
#define STBTT_free(p, u) (::operator delete(p))
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define LMIMG_EMBEDDED_FONT 1
#else
#define LMIMG_EMBEDDED_FONT 0
#endif

namespace lmimg {
namespace {
std::once_flag genesis;
void init() {
  std::call_once(genesis, [] {
    MagickWandGenesis();
    MagickWand *policy_wand = NewMagickWand();
    if (!policy_wand)
      throw std::runtime_error("ImageMagick policy initialization failed");
    const char *policy =
        "<policymap><policy domain=\"delegate\" rights=\"none\" "
        "pattern=\"*\"/><policy domain=\"coder\" rights=\"none\" "
        "pattern=\"*\"/><policy domain=\"coder\" rights=\"read|write\" "
        "pattern=\"{PNG,JPEG,TIFF,WEBP,GIF,BMP,PNM,PPM,PGM,PBM}\"/><policy "
        "domain=\"path\" "
        "rights=\"none\" pattern=\"@*\"/><policy domain=\"system\" "
        "name=\"max-profile-size\" value=\"1MiB\"/></policymap>";
    if (MagickSetSecurityPolicy(policy_wand, policy) == MagickFalse) {
      DestroyMagickWand(policy_wand);
      throw std::runtime_error(
          "ImageMagick security policy initialization failed");
    }
    DestroyMagickWand(policy_wand);
    MagickSetResourceLimit(DiskResource, 0);
    MagickSetResourceLimit(MapResource, 0);
    MagickSetResourceLimit(ListLengthResource, MagickResourceInfinity);
  });
}

[[noreturn]] void fail(MagickWand *w, const char *what) {
  ExceptionType severity;
  char *detail = w ? MagickGetException(w, &severity) : nullptr;
  std::string message = what;
  if (detail && *detail) {
    message += ": ";
    message += detail;
  }
  if (detail)
    MagickRelinquishMemory(detail);
  throw std::runtime_error(message);
}
void ok(MagickBooleanType v, MagickWand *w, const char *what) {
  if (v == MagickFalse)
    fail(w, what);
}
void valid_size(std::size_t w, std::size_t h) {
  if (!w || !h || w > std::numeric_limits<std::size_t>::max() / h ||
      w * h > (std::size_t(1) << 31) ||
      w * h > (std::numeric_limits<std::size_t>::max() - 32768) / 32)
    throw std::invalid_argument("image dimensions are invalid or too large");
  // Q8 pixels occupy four bytes; MagickWand's cache and operation scratch can
  // retain several copies, so preflight at 32 bytes per pixel before native
  // calls.
  membudget::require(w * h * 32 + 32768);
}
double clip(double x) {
  if (!std::isfinite(x))
    throw std::invalid_argument("pixel expression produced a non-finite value");
  return std::max(0.0, std::min(1.0, x));
}
void valid_color(Color c) {
  if (!std::isfinite(c.r) || !std::isfinite(c.g) || !std::isfinite(c.b) ||
      !std::isfinite(c.a))
    throw std::invalid_argument("color contains non-finite value");
}
bool supported_format(std::string e) {
  for (char &c : e)
    c = char(std::tolower((unsigned char)c));
  return e == "png" || e == "jpg" || e == "jpeg" || e == "tif" || e == "tiff" ||
         e == "webp" || e == "gif" || e == "bmp" || e == "pnm" || e == "ppm" ||
         e == "pgm" || e == "pbm";
}
void check_path(const std::string &path) {
  if (path.empty() || path.find('\0') != std::string::npos)
    throw std::invalid_argument("image path must be a local file");
  for (size_t i = 0; i < path.size(); i++)
    if (path[i] == ':') {
      bool drive = i == 1 && std::isalpha((unsigned char)path[0]) &&
                   i + 1 < path.size() && (path[2] == '/' || path[2] == '\\');
      if (!drive)
        throw std::invalid_argument("image path must be a local file");
    }
  auto dot = path.find_last_of('.');
  if (dot == std::string::npos)
    throw std::invalid_argument("image path needs a supported extension");
  std::string e = path.substr(dot + 1);
  for (char &c : e)
    c = char(std::tolower((unsigned char)c));
  if (!supported_format(e))
    throw std::invalid_argument("unsupported image format");
}
std::string path_format(const std::string &path) {
  auto d = path.find_last_of('.');
  std::string e = path.substr(d + 1);
  for (char &c : e)
    c = char(std::toupper((unsigned char)c));
  if (e == "JPG")
    e = "JPEG";
  if (e == "TIF")
    e = "TIFF";
  return e;
}
PixelWand *color_wand(Color c) {
  valid_color(c);
  c.r = clip(c.r);
  c.g = clip(c.g);
  c.b = clip(c.b);
  c.a = clip(c.a);
  PixelWand *p = NewPixelWand();
  if (!p)
    throw std::runtime_error("pixel allocation failed");
  PixelSetRed(p, clip(c.r));
  PixelSetGreen(p, clip(c.g));
  PixelSetBlue(p, clip(c.b));
  PixelSetAlpha(p, clip(c.a));
  return p;
}
void color_draw(DrawingWand *d, Color c) {
  PixelWand *p = color_wand(c);
  DrawSetFillColor(d, p);
  DestroyPixelWand(p);
}
std::size_t bytes_for(MagickWand *w) {
  return MagickGetImageWidth(w) * MagickGetImageHeight(w) * 8 + 16384;
}
constexpr std::size_t metadata_cap = 1024 * 1024;
struct NativeNames {
  char **names;
  size_t count;
  ~NativeNames() {
    if (names) {
      for (size_t i = 0; i < count; ++i)
        MagickRelinquishMemory(names[i]);
      MagickRelinquishMemory(names);
    }
  }
};
std::size_t native_metadata_bytes(MagickWand *w) {
  size_t count = 0, total = 0;
  auto add = [&](size_t n) {
    if (n > metadata_cap - total)
      throw std::invalid_argument("image metadata exceeds 1 MiB");
    total += n;
  };
  NativeNames props{MagickGetImageProperties(w, "*", &count), count};
  if (count > 4096)
    throw std::invalid_argument("too many image metadata fields");
  for (size_t i = 0; i < props.count; ++i) {
    std::unique_ptr<char, decltype(&MagickRelinquishMemory)> value(
        MagickGetImageProperty(w, props.names[i]), MagickRelinquishMemory);
    add(std::strlen(props.names[i]) + 256);
    if (value)
      add(std::strlen(value.get()));
  }
  count = 0;
  NativeNames profiles{MagickGetImageProfiles(w, "*", &count), count};
  if (count > 64)
    throw std::invalid_argument("too many image profiles");
  for (size_t i = 0; i < profiles.count; ++i) {
    size_t n = 0;
    membudget::require(metadata_cap);
    std::unique_ptr<unsigned char, decltype(&MagickRelinquishMemory)> data(
        MagickGetImageProfile(w, profiles.names[i], &n),
        MagickRelinquishMemory);
    add(n);
    add(std::strlen(profiles.names[i]) + 256);
  }
  return total;
}
void export_rgba(MagickWand *w, std::vector<double> &a) {
  size_t n = MagickGetImageWidth(w) * MagickGetImageHeight(w);
  membudget::require(n * 4 * sizeof(double));
  a.resize(n * 4);
  ok(MagickExportImagePixels(w, 0, 0, MagickGetImageWidth(w),
                             MagickGetImageHeight(w), "RGBA", DoublePixel,
                             a.data()),
     w, "export pixels");
}
void import_rgba(MagickWand *w, std::vector<double> &a) {
  ok(MagickSetImageAlphaChannel(w, ActivateAlphaChannel), w, "activate alpha");
  ok(MagickImportImagePixels(w, 0, 0, MagickGetImageWidth(w),
                             MagickGetImageHeight(w), "RGBA", DoublePixel,
                             a.data()),
     w, "import pixels");
}
template <class F> void rgb_filter(MagickWand *p, F operation) {
  const size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p);
  std::vector<unsigned char> alpha(w * h);
  ok(MagickExportImagePixels(p, 0, 0, w, h, "A", CharPixel, alpha.data()), p,
     "read filter alpha");
  const auto old = MagickSetImageChannelMask(
      p, static_cast<ChannelType>(RedChannel | GreenChannel | BlueChannel));
  operation();
  MagickSetImageChannelMask(p, old);
  ok(MagickSetImageAlphaChannel(p, ActivateAlphaChannel), p,
     "activate filter alpha");
  ok(MagickImportImagePixels(p, 0, 0, w, h, "A", CharPixel, alpha.data()), p,
     "preserve filter alpha");
}
void kernel_peak(double radius, double sigma) {
  if (!std::isfinite(radius) || !std::isfinite(sigma) || radius < 0 ||
      sigma < 0 || radius > 256 || sigma > 256)
    throw std::invalid_argument("filter radius and sigma must be in [0,256]");
  auto width =
      static_cast<size_t>(2 * std::ceil(std::max(radius, 4 * sigma)) + 1);
  membudget::require(width * width * sizeof(double) + 32768);
}
double lum(const double *p) {
  return .2126 * p[0] + .7152 * p[1] + .0722 * p[2];
}
// Bilinear RGBA sample at continuous pixel coordinates (fx, fy), computed on
// premultiplied color so transparent neighbours do not darken edges. The
// coordinates must already lie inside [0,w-1] x [0,h-1].
void sample_bilinear(const std::vector<double> &a, size_t w, size_t h,
                     double fx, double fy, double *out) {
  size_t x0 = size_t(std::floor(fx)), y0 = size_t(std::floor(fy));
  size_t x1 = std::min(w - 1, x0 + 1), y1 = std::min(h - 1, y0 + 1);
  x0 = std::min(w - 1, x0);
  y0 = std::min(h - 1, y0);
  double tx = fx - double(x0), ty = fy - double(y0);
  const double *p00 = &a[(y0 * w + x0) * 4], *p10 = &a[(y0 * w + x1) * 4],
               *p01 = &a[(y1 * w + x0) * 4], *p11 = &a[(y1 * w + x1) * 4];
  double w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty), w01 = (1 - tx) * ty,
         w11 = tx * ty;
  double alpha = w00 * p00[3] + w10 * p10[3] + w01 * p01[3] + w11 * p11[3];
  for (int k = 0; k < 3; k++) {
    double prem = w00 * p00[k] * p00[3] + w10 * p10[k] * p10[3] +
                  w01 * p01[k] * p01[3] + w11 * p11[k] * p11[3];
    out[k] = alpha > 1e-12 ? prem / alpha : 0;
  }
  out[3] = alpha;
}
void to_hsl(double r, double g, double b, double &h, double &s, double &l) {
  double hi = std::max({r, g, b}), lo = std::min({r, g, b}), d = hi - lo;
  l = (hi + lo) / 2;
  s = d == 0 ? 0 : d / (1 - std::abs(2 * l - 1));
  if (!d)
    h = 0;
  else if (hi == r)
    h = 60 * std::fmod((g - b) / d + 6, 6.);
  else if (hi == g)
    h = 60 * ((b - r) / d + 2);
  else
    h = 60 * ((r - g) / d + 4);
}
double hpart(double p, double q, double t) {
  if (t < 0)
    t += 1;
  if (t > 1)
    t -= 1;
  if (t < 1. / 6)
    return p + (q - p) * 6 * t;
  if (t < .5)
    return q;
  if (t < 2. / 3)
    return p + (q - p) * (2. / 3 - t) * 6;
  return p;
}
void from_hsl(double h, double s, double l, double &r, double &g, double &b) {
  if (!s) {
    r = g = b = l;
    return;
  }
  double q = l < .5 ? l * (1 + s) : l + s - l * s, p = 2 * l - q;
  h /= 360;
  r = hpart(p, q, h + 1. / 3);
  g = hpart(p, q, h);
  b = hpart(p, q, h - 1. / 3);
}
} // namespace

struct Image::Impl {
  MagickWand *wand = nullptr;
  std::size_t charged = 0;
  std::size_t metadata_bytes = 0;
  explicit Impl(MagickWand *p, std::size_t metadata = 0)
      : wand(p), charged(bytes_for(p) + metadata), metadata_bytes(metadata) {
    membudget::charge(charged);
  }
  ~Impl() {
    if (wand)
      DestroyMagickWand(wand);
    membudget::release(charged);
  }
  Impl(const Impl &) = delete;
};

Image::Image() : impl_() {}
Image::Image(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Image::~Image() = default;
Image::Image(Image &&) noexcept = default;
Image &Image::operator=(Image &&) noexcept = default;
Image::Image(const Image &x) {
  if (x.impl_) {
    init();
    valid_size(x.width(), x.height());
    membudget::require(x.impl_->metadata_bytes);
    std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> p(
        CloneMagickWand(x.impl_->wand), DestroyMagickWand);
    if (!p)
      fail(x.impl_->wand, "clone");
    impl_ = std::make_unique<Impl>(p.get(), x.impl_->metadata_bytes);
    p.release();
  }
}
Image &Image::operator=(const Image &x) {
  if (this != &x) {
    Image t(x);
    *this = std::move(t);
  }
  return *this;
}
void Image::require_image() const {
  if (!impl_)
    throw std::runtime_error("image is empty");
}

Image Image::create(std::size_t w, std::size_t h, Color fill) {
  valid_color(fill);
  init();
  valid_size(w, h);
  std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> native(
      NewMagickWand(), DestroyMagickWand);
  if (!native)
    throw std::runtime_error("image allocation failed");
  MagickWand *p = native.get();
  std::unique_ptr<PixelWand, decltype(&DestroyPixelWand)> pixel(
      color_wand(fill), DestroyPixelWand);
  PixelWand *c = pixel.get();
  try {
    ok(MagickNewImage(p, w, h, c), p, "create image");
    ok(MagickSetImageAlphaChannel(p, SetAlphaChannel), p, "activate alpha");
    ok(MagickSetImageAlpha(p, clip(fill.a)), p, "set fill alpha");
  } catch (...) {
    throw;
  }
  Image out(std::make_unique<Impl>(native.get()));
  native.release();
  return out;
}
Image Image::open(const std::string &path) {
  init();
  check_path(path);
  const std::string read_path = path + "[0]";
  std::uintmax_t file_bytes;
  try {
    file_bytes = std::filesystem::file_size(path);
  } catch (const std::filesystem::filesystem_error &) {
    throw std::runtime_error("cannot open image file: " + path);
  }
  if (file_bytes > (std::numeric_limits<size_t>::max() - 2 * 1024 * 1024) / 2)
    throw std::invalid_argument("image file too large");
  membudget::require(static_cast<size_t>(file_bytes) * 2 + 2 * 1024 * 1024);
  MagickWand *probe = NewMagickWand();
  if (!probe)
    throw std::runtime_error("image allocation failed");
  try {
    ok(MagickPingImage(probe, read_path.c_str()), probe, "inspect image");
    valid_size(MagickGetImageWidth(probe), MagickGetImageHeight(probe));
  } catch (...) {
    DestroyMagickWand(probe);
    throw;
  }
  DestroyMagickWand(probe);
  MagickWand *p = NewMagickWand();
  if (!p)
    throw std::runtime_error("image allocation failed");
  try {
    ok(MagickReadImage(p, read_path.c_str()), p, "read image");
  } catch (...) {
    DestroyMagickWand(p);
    throw;
  }
  try {
    valid_size(MagickGetImageWidth(p), MagickGetImageHeight(p));
  } catch (...) {
    DestroyMagickWand(p);
    throw;
  }
  std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> holder(
      p, DestroyMagickWand);
  Image out(std::make_unique<Impl>(holder.get(),
                                   native_metadata_bytes(holder.get())));
  holder.release();
  return out;
}
Image Image::clone() const { return Image(*this); }
void Image::save(const std::string &path) const {
  require_image();
  check_path(path);
  save(path, path_format(path));
}
void Image::save(const std::string &path, const std::string &format) const {
  require_image();
  check_path(path);
  if (!supported_format(format))
    throw std::invalid_argument("unsupported image format");
  membudget::require(width() * height() * 32 + 4096);
  std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> p(
      CloneMagickWand(impl_->wand), DestroyMagickWand);
  if (!p)
    fail(impl_->wand, "clone");
  ok(MagickSetFormat(p.get(), format.c_str()), p.get(), "set format");
  if (format == "PGM" || format == "PBM")
    ok(MagickTransformImageColorspace(p.get(), GRAYColorspace), p.get(),
       "set grayscale format");
  ok(MagickSetOption(p.get(), "png:exclude-chunks", "date,time"), p.get(),
     "stable PNG metadata");
  ok(MagickSetOption(p.get(), "webp:lossless", "true"), p.get(),
     "lossless WebP");
  ok(MagickSetOption(p.get(), "webp:exact", "true"), p.get(),
     "exact WebP alpha");
  ok(MagickSetImageCompressionQuality(p.get(), 95), p.get(), "codec quality");
  ok(MagickWriteImage(p.get(), path.c_str()), p.get(), "write image");
}
std::size_t Image::width() const noexcept {
  return impl_ ? MagickGetImageWidth(impl_->wand) : 0;
}
std::size_t Image::height() const noexcept {
  return impl_ ? MagickGetImageHeight(impl_->wand) : 0;
}

Color Image::pixel(std::size_t x, std::size_t y) const {
  require_image();
  if (x >= width() || y >= height())
    throw std::out_of_range("pixel coordinates");
  std::unique_ptr<PixelWand, decltype(&DestroyPixelWand)> p(NewPixelWand(),
                                                            DestroyPixelWand);
  if (!p)
    throw std::runtime_error("pixel allocation failed");
  ok(MagickGetImagePixelColor(impl_->wand, x, y, p.get()), impl_->wand,
     "read pixel");
  Color c{PixelGetRed(p.get()), PixelGetGreen(p.get()), PixelGetBlue(p.get()),
          PixelGetAlpha(p.get())};
  return c;
}
Image &Image::set_pixel(std::size_t x, std::size_t y, Color c) {
  if (x >= width() || y >= height())
    throw std::out_of_range("pixel coordinates");
  return mutate([&](MagickWand *p) {
    std::unique_ptr<PixelWand, decltype(&DestroyPixelWand)> q(color_wand(c),
                                                              DestroyPixelWand);
    ok(MagickSetImagePixelColor(p, x, y, q.get()), p, "write pixel");
  });
}

template <class F> Image &Image::mutate(F &&fn, std::size_t metadata_add) {
  require_image();
  valid_size(width(), height());
  membudget::require(impl_->metadata_bytes + metadata_add);
  MagickWand *p = CloneMagickWand(impl_->wand);
  if (!p)
    fail(impl_->wand, "clone image");
  std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> holder(
      p, DestroyMagickWand);
  {
    fn(holder.get());
    valid_size(MagickGetImageWidth(holder.get()),
               MagickGetImageHeight(holder.get()));
    auto n = std::make_unique<Impl>(holder.get(),
                                    impl_->metadata_bytes + metadata_add);
    holder.release();
    impl_.swap(n);
  }
  return *this;
}
Image &Image::resize(std::size_t w, std::size_t h) {
  valid_size(w, h);
  return mutate([&](MagickWand *p) {
    ok(MagickResizeImage(p, w, h, LanczosFilter), p, "resize");
  });
}
Image &Image::crop(std::size_t x, std::size_t y, std::size_t w, std::size_t h) {
  valid_size(w, h);
  if (x > width() || y > height() || w > width() - x || h > height() - y)
    throw std::out_of_range("crop rectangle");
  return mutate([&](MagickWand *p) {
    ok(MagickCropImage(p, w, h, x, y), p, "crop");
    ok(MagickResetImagePage(p, "+0+0"), p, "reset crop page");
  });
}
Image &Image::rotate(double d, Color bg) {
  if (!std::isfinite(d))
    throw std::invalid_argument("rotation is not finite");
  d = std::remainder(d, 360.0);
  double rad = d * 3.14159265358979323846 / 180.0;
  double nw = std::ceil(std::abs(width() * std::cos(rad)) +
                        std::abs(height() * std::sin(rad)));
  double nh = std::ceil(std::abs(width() * std::sin(rad)) +
                        std::abs(height() * std::cos(rad)));
  if (!std::isfinite(nw) || !std::isfinite(nh) || nw < 1 || nh < 1 ||
      nw > std::numeric_limits<std::size_t>::max() ||
      nh > std::numeric_limits<std::size_t>::max())
    throw std::invalid_argument("rotation extent is invalid");
  valid_size(std::size_t(nw), std::size_t(nh));
  return mutate([&](MagickWand *p) {
    std::unique_ptr<PixelWand, decltype(&DestroyPixelWand)> q(color_wand(bg),
                                                              DestroyPixelWand);
    ok(MagickRotateImage(p, q.get(), d), p, "rotate");
  });
}
Image &Image::flip(bool horizontal) {
  return mutate([&](MagickWand *p) {
    ok(horizontal ? MagickFlopImage(p) : MagickFlipImage(p), p, "flip");
  });
}
Image &Image::levels(double black, double gamma, double white) {
  if (!std::isfinite(black) || !std::isfinite(gamma) || !std::isfinite(white) ||
      !(gamma > 0) || black < 0 || white > 1 || black >= white)
    throw std::invalid_argument(
        "levels require 0 <= black < white <= 1 and positive finite gamma");
  return mutate([&](MagickWand *p) {
    rgb_filter(p, [&] {
      ok(MagickLevelImage(p, black * QuantumRange, gamma, white * QuantumRange),
         p, "levels");
    });
  });
}
namespace {
void valid_curve(const std::vector<double> &cp) {
  if (cp.size() < 4 || cp.size() % 2 || cp.front() != 0 ||
      cp[cp.size() - 2] != 1)
    throw std::invalid_argument("curves needs endpoints at 0 and 1");
  for (size_t i = 0; i < cp.size(); i++)
    if (!std::isfinite(cp[i]) || cp[i] < 0 || cp[i] > 1 ||
        (i % 2 == 0 && i && cp[i] <= cp[i - 2]))
      throw std::invalid_argument(
          "curve points must be finite and ordered in [0,1]");
}
double curve_at(const std::vector<double> &cp, double x) {
  x = clip(x);
  if (x >= cp[cp.size() - 2])
    return clip(cp.back());
  for (size_t j = 0; j + 2 < cp.size(); j += 2)
    if (x <= cp[j + 2]) {
      double t = (x - cp[j]) / (cp[j + 2] - cp[j]);
      return clip(cp[j + 1] * (1 - t) + cp[j + 3] * t);
    }
  return clip(cp[1]);
}
} // namespace
Image &Image::curves(const std::vector<double> &cp) {
  return curves_rgb(cp, cp, cp);
}
Image &Image::curves_rgb(const std::vector<double> &r,
                         const std::vector<double> &g,
                         const std::vector<double> &b) {
  valid_curve(r);
  valid_curve(g);
  valid_curve(b);
  const std::vector<double> *cp[3] = {&r, &g, &b};
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t i = 0; i < a.size(); i += 4)
      for (int k = 0; k < 3; k++)
        a[i + k] = curve_at(*cp[k], a[i + k]);
    import_rgba(p, a);
  });
}
Image &Image::threshold(double level) {
  if (!std::isfinite(level) || level < 0 || level > 1)
    throw std::invalid_argument("threshold level must be in [0,1]");
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t i = 0; i < a.size(); i += 4) {
      double v = lum(&a[i]) >= level ? 1 : 0;
      a[i] = a[i + 1] = a[i + 2] = v;
    }
    import_rgba(p, a);
  });
}
Image &Image::invert() {
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t i = 0; i < a.size(); i += 4)
      for (int k = 0; k < 3; k++)
        a[i + k] = 1 - clip(a[i + k]);
    import_rgba(p, a);
  });
}
Image &Image::normalize() {
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    double lo[3] = {1, 1, 1}, hi[3] = {0, 0, 0};
    for (size_t i = 0; i < a.size(); i += 4)
      for (int k = 0; k < 3; k++) {
        lo[k] = std::min(lo[k], a[i + k]);
        hi[k] = std::max(hi[k], a[i + k]);
      }
    for (size_t i = 0; i < a.size(); i += 4)
      for (int k = 0; k < 3; k++)
        if (hi[k] > lo[k])
          a[i + k] = clip((a[i + k] - lo[k]) / (hi[k] - lo[k]));
    import_rgba(p, a);
  });
}
Image &Image::equalize() {
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    // Equalize the luma histogram and scale RGB by the luma ratio, so hues
    // are preserved (per-channel equalization posterizes color).
    const size_t n = a.size() / 4;
    std::vector<size_t> bins(256, 0);
    std::vector<double> luma(n);
    for (size_t i = 0; i < n; i++) {
      luma[i] = clip(lum(&a[i * 4]));
      bins[size_t(std::lround(255 * luma[i]))]++;
    }
    std::vector<double> cdf(256, 0);
    size_t run = 0, first = 0;
    bool seen = false;
    for (int v = 0; v < 256; v++) {
      run += bins[v];
      if (!seen && bins[v]) {
        seen = true;
        first = run;
      }
      cdf[v] = double(run);
    }
    // Classic equalization: map through the normalized cumulative histogram
    // so the first occupied bin lands on 0 and the last on 1.
    double span = double(n) - double(first);
    for (size_t i = 0; i < n; i++) {
      int v = int(std::lround(255 * luma[i]));
      double target = span > 0 ? clip((cdf[v] - double(first)) / span) : 0;
      double ratio = luma[i] > 1e-9 ? target / luma[i] : 0;
      for (int k = 0; k < 3; k++)
        a[i * 4 + k] = luma[i] > 1e-9 ? clip(a[i * 4 + k] * ratio) : target;
    }
    import_rgba(p, a);
  });
}
Image &Image::colormap(const std::string &name) {
  // Anchor colors at equally spaced positions in [0,1], linearly interpolated.
  static const std::vector<std::array<double, 3>> gray = {{0, 0, 0}, {1, 1, 1}},
      heat = {{0, 0, 0}, {0.75, 0, 0}, {1, 0.6, 0}, {1, 1, 0.25}, {1, 1, 1}},
      viridis = {{0.267, 0.005, 0.329},
                 {0.229, 0.322, 0.546},
                 {0.127, 0.567, 0.551},
                 {0.369, 0.789, 0.383},
                 {0.993, 0.906, 0.144}},
      coolwarm = {{0.230, 0.299, 0.754},
                  {0.865, 0.865, 0.865},
                  {0.706, 0.016, 0.150}};
  const std::vector<std::array<double, 3>> *lut =
      name == "gray"       ? &gray
      : name == "heat"     ? &heat
      : name == "viridis"  ? &viridis
      : name == "coolwarm" ? &coolwarm
                           : nullptr;
  if (!lut)
    throw std::invalid_argument(
        "colormap must be gray, heat, viridis or coolwarm");
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    const double segments = double(lut->size() - 1);
    for (size_t i = 0; i < a.size(); i += 4) {
      double t = clip(lum(&a[i])) * segments;
      size_t j = std::min(lut->size() - 2, size_t(std::floor(t)));
      double f = t - double(j);
      for (int k = 0; k < 3; k++)
        a[i + k] = clip((*lut)[j][k] * (1 - f) + (*lut)[j + 1][k] * f);
    }
    import_rgba(p, a);
  });
}
Image Image::hald(std::size_t level) {
  if (level < 2 || level > 16)
    throw std::invalid_argument("hald level must be in [2,16]");
  const size_t cube = level * level, side = cube * level;
  valid_size(side, side);
  membudget::require(side * side * 4 * sizeof(double));
  std::vector<double> px(side * side * 4);
  const double den = double(cube - 1);
  for (size_t i = 0; i < side * side; i++) {
    px[i * 4] = double(i % cube) / den;
    px[i * 4 + 1] = double((i / cube) % cube) / den;
    px[i * 4 + 2] = double(i / (cube * cube)) / den;
    px[i * 4 + 3] = 1;
  }
  Image out = create(side, side);
  ok(MagickSetImageAlphaChannel(out.impl_->wand, ActivateAlphaChannel),
     out.impl_->wand, "activate alpha");
  ok(MagickImportImagePixels(out.impl_->wand, 0, 0, side, side, "RGBA",
                             DoublePixel, px.data()),
     out.impl_->wand, "import hald pixels");
  return out;
}
Image &Image::clut(const Image &h) {
  h.require_image();
  size_t level = 0;
  for (size_t l = 2; l <= 16; l++)
    if (l * l * l == h.width())
      level = l;
  if (!level || h.width() != h.height())
    throw std::invalid_argument(
        "clut needs a square Hald image whose side is level^3 (level 2-16)");
  const size_t cube = level * level;
  return mutate([&](MagickWand *p) {
    std::vector<double> a, t;
    export_rgba(p, a);
    export_rgba(h.impl_->wand, t);
    auto sample = [&](size_t r, size_t g, size_t b, int k) {
      return t[(r + g * cube + b * cube * cube) * 4 + size_t(k)];
    };
    const double den = double(cube - 1);
    for (size_t i = 0; i < a.size(); i += 4) {
      double c[3], f[3];
      size_t i0[3], i1[3];
      for (int k = 0; k < 3; k++) {
        c[k] = clip(a[i + k]) * den;
        i0[k] = std::min(cube - 1, size_t(std::floor(c[k])));
        i1[k] = std::min(cube - 1, i0[k] + 1);
        f[k] = c[k] - double(i0[k]);
      }
      for (int k = 0; k < 3; k++) {
        double c00 = sample(i0[0], i0[1], i0[2], k) * (1 - f[0]) +
                     sample(i1[0], i0[1], i0[2], k) * f[0];
        double c10 = sample(i0[0], i1[1], i0[2], k) * (1 - f[0]) +
                     sample(i1[0], i1[1], i0[2], k) * f[0];
        double c01 = sample(i0[0], i0[1], i1[2], k) * (1 - f[0]) +
                     sample(i1[0], i0[1], i1[2], k) * f[0];
        double c11 = sample(i0[0], i1[1], i1[2], k) * (1 - f[0]) +
                     sample(i1[0], i1[1], i1[2], k) * f[0];
        double c0 = c00 * (1 - f[1]) + c10 * f[1],
               c1 = c01 * (1 - f[1]) + c11 * f[1];
        a[i + k] = clip(c0 * (1 - f[2]) + c1 * f[2]);
      }
    }
    import_rgba(p, a);
  });
}
Image &Image::hsl(double hd, double ss, double ls) {
  if (!std::isfinite(hd) || !std::isfinite(ss) || !std::isfinite(ls) ||
      ss < 0 || ls < 0)
    throw std::invalid_argument("invalid HSL parameters");
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t i = 0; i < a.size(); i += 4) {
      double h, s, l, r, g, b;
      to_hsl(a[i], a[i + 1], a[i + 2], h, s, l);
      from_hsl(std::fmod(std::fmod(hd, 360.) + h + 360., 360.), clip(s * ss),
               clip(l * ls), r, g, b);
      a[i] = r;
      a[i + 1] = g;
      a[i + 2] = b;
    }
    import_rgba(p, a);
  });
}
Image &Image::channel_mix(const std::array<double, 9> &m) {
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t i = 0; i < a.size(); i += 4) {
      double r = a[i], g = a[i + 1], b = a[i + 2];
      a[i] = clip(m[0] * r + m[1] * g + m[2] * b);
      a[i + 1] = clip(m[3] * r + m[4] * g + m[5] * b);
      a[i + 2] = clip(m[6] * r + m[7] * g + m[8] * b);
    }
    import_rgba(p, a);
  });
}
Image &Image::grayscale() {
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t i = 0; i < a.size(); i += 4) {
      double v = lum(&a[i]);
      a[i] = a[i + 1] = a[i + 2] = v;
    }
    import_rgba(p, a);
  });
}
Image &Image::sepia(double strength) {
  if (!std::isfinite(strength) || strength < 0 || strength > 100)
    throw std::invalid_argument("sepia strength must be in [0,100]");
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    double t = strength / 100;
    for (size_t i = 0; i < a.size(); i += 4) {
      double r = a[i], g = a[i + 1], b = a[i + 2];
      a[i] = clip((1 - t) * r + t * (.393 * r + .769 * g + .189 * b));
      a[i + 1] = clip((1 - t) * g + t * (.349 * r + .686 * g + .168 * b));
      a[i + 2] = clip((1 - t) * b + t * (.272 * r + .534 * g + .131 * b));
    }
    import_rgba(p, a);
  });
}
Image &Image::blur(double r, double sigma) {
  kernel_peak(r, sigma);
  // Unlike sharpen/edges, blur acts on all four channels so that a layer with
  // transparency (a shadow or glow) feathers its alpha. The convolution runs
  // on premultiplied color so transparent pixels never bleed their color.
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    bool opaque = true;
    for (size_t i = 3; i < a.size(); i += 4)
      if (a[i] < 1) {
        opaque = false;
        break;
      }
    if (!opaque) {
      for (size_t i = 0; i < a.size(); i += 4)
        for (int k = 0; k < 3; k++)
          a[i + k] *= a[i + 3];
      import_rgba(p, a);
    }
    ok(MagickBlurImage(p, r, sigma), p, "blur");
    if (!opaque) {
      export_rgba(p, a);
      for (size_t i = 0; i < a.size(); i += 4)
        for (int k = 0; k < 3; k++)
          a[i + k] = a[i + 3] > 1e-9 ? clip(a[i + k] / a[i + 3]) : 0;
      import_rgba(p, a);
    }
  });
}
Image &Image::sharpen(double r, double sigma) {
  kernel_peak(r, sigma);
  return mutate([&](MagickWand *p) {
    rgb_filter(p, [&] { ok(MagickSharpenImage(p, r, sigma), p, "sharpen"); });
  });
}
Image &Image::edges(double r) {
  kernel_peak(r, 0);
  return mutate([&](MagickWand *p) {
    rgb_filter(p, [&] { ok(MagickEdgeImage(p, r), p, "edges"); });
  });
}
Image &Image::unsharp(double r, double s, double amount, double threshold) {
  if (!std::isfinite(r) || !std::isfinite(s) || r < 0 || s < 0 ||
      !std::isfinite(amount) || !std::isfinite(threshold) || threshold < 0 ||
      r > 256 || s > 256)
    throw std::invalid_argument("invalid unsharp parameters");
  return mutate([&](MagickWand *p) {
    size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p);
    std::vector<double> a, o;
    export_rgba(p, a);
    o = a;
    int rad = std::max(1, int(std::ceil(r)));
    double sigma = s > 0 ? s : std::max(0.5, r / 3.0),
           inv = 1.0 / (2 * sigma * sigma);
    for (size_t y = 0; y < h; y++)
      for (size_t x = 0; x < w; x++) {
        double avg[3] = {}, weight = 0;
        for (int yy = -rad; yy <= rad; yy++)
          for (int xx = -rad; xx <= rad; xx++) {
            size_t sx = std::min(w - 1, size_t(std::max(0, int(x) + xx))),
                   sy = std::min(h - 1, size_t(std::max(0, int(y) + yy)));
            double wt = std::exp(-(xx * xx + yy * yy) * inv);
            for (int k = 0; k < 3; k++)
              avg[k] += wt * a[(sy * w + sx) * 4 + k];
            weight += wt;
          }
        for (int k = 0; k < 3; k++) {
          double v = a[(y * w + x) * 4 + k], blur = avg[k] / weight;
          if (std::abs(v - blur) >= threshold)
            o[(y * w + x) * 4 + k] = clip(v + amount * (v - blur));
        }
      }
    import_rgba(p, o);
  });
}
Image &Image::vignette(double radius, double sigma) {
  if (!std::isfinite(radius) || !std::isfinite(sigma) || radius < 0 ||
      sigma < 0)
    throw std::invalid_argument("invalid vignette parameters");
  return mutate([&](MagickWand *p) {
    size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p);
    std::vector<double> a;
    export_rgba(p, a);
    double cx = (w - 1) / 2., cy = (h - 1) / 2.,
           r = radius > 0 ? radius : std::max(1., std::min(w, h) / 2.);
    for (size_t y = 0; y < h; y++)
      for (size_t x = 0; x < w; x++) {
        double d = std::sqrt((x - cx) * (x - cx) + (y - cy) * (y - cy)) / r;
        double f =
            d <= 1 ? 1
                   : std::max(0., 1 - (d - 1) * (sigma > 0 ? 1 / sigma : 1));
        for (int k = 0; k < 3; k++)
          a[(y * w + x) * 4 + k] *= f;
      }
    import_rgba(p, a);
  });
}
Image &Image::tilt_shift(double radius, double a, double b) {
  kernel_peak(radius, radius);
  if (!std::isfinite(radius) || !std::isfinite(a) || !std::isfinite(b) ||
      radius < 0 || a < 0 || b < a || b > 1)
    throw std::invalid_argument("tilt shift focus range");
  return mutate([&](MagickWand *p) {
    std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> q(
        CloneMagickWand(p), DestroyMagickWand);
    if (!q)
      throw std::runtime_error("tilt shift clone failed");
    ok(MagickBlurImage(q.get(), radius, radius), q.get(), "tilt shift blur");
    size_t h = MagickGetImageHeight(p), w = MagickGetImageWidth(p);
    membudget::require(w * h * 8);
    std::vector<unsigned char> src(w * h * 4), dst(w * h * 4);
    ok(MagickExportImagePixels(p, 0, 0, w, h, "RGBA", CharPixel, src.data()), p,
       "tilt shift pixels");
    ok(MagickExportImagePixels(q.get(), 0, 0, w, h, "RGBA", CharPixel,
                               dst.data()),
       q.get(), "tilt shift blurred pixels");
    for (size_t y = 0; y < h; y++) {
      double z = double(y) / (h > 1 ? h - 1 : 1);
      if (z >= a && z <= b)
        for (size_t x = 0; x < w; x++)
          for (int k = 0; k < 4; k++)
            dst[(y * w + x) * 4 + k] = src[(y * w + x) * 4 + k];
    }
    ok(MagickImportImagePixels(p, 0, 0, w, h, "RGBA", CharPixel, dst.data()), p,
       "tilt shift pixels");
  });
}
Image &Image::morphology(const std::string &op, std::size_t radius) {
  if (radius > 16 || (op != "erode" && op != "dilate" && op != "median"))
    throw std::invalid_argument("morphology operation or radius");
  return mutate([&](MagickWand *p) {
    size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p);
    std::vector<double> a, o;
    export_rgba(p, a);
    o = a;
    int r = int(radius);
    for (size_t y = 0; y < h; y++)
      for (size_t x = 0; x < w; x++)
        for (int k = 0; k < 3; k++) {
          std::vector<double> v;
          for (int yy = -r; yy <= r; yy++)
            for (int xx = -r; xx <= r; xx++) {
              size_t sx = std::min(w - 1, size_t(std::max(0, int(x) + xx))),
                     sy = std::min(h - 1, size_t(std::max(0, int(y) + yy)));
              v.push_back(a[(sy * w + sx) * 4 + k]);
            }
          if (op == "erode")
            o[(y * w + x) * 4 + k] = *std::min_element(v.begin(), v.end());
          else if (op == "dilate")
            o[(y * w + x) * 4 + k] = *std::max_element(v.begin(), v.end());
          else {
            std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
            o[(y * w + x) * 4 + k] = v[v.size() / 2];
          }
        }
    import_rgba(p, o);
  });
}
Image &Image::apply_mask(const Image &m) {
  m.require_image();
  if (width() != m.width() || height() != m.height())
    throw std::invalid_argument("mask dimensions differ");
  return mutate([&](MagickWand *p) {
    std::vector<double> a, b;
    export_rgba(p, a);
    export_rgba(m.impl_->wand, b);
    for (size_t i = 0; i < a.size(); i += 4)
      a[i + 3] *= clip(lum(&b[i]) * b[i + 3]);
    import_rgba(p, a);
  });
}
Image &Image::selection(std::size_t x, std::size_t y, std::size_t w,
                        std::size_t h) {
  if (x > width() || y > height() || w > width() - x || h > height() - y)
    throw std::out_of_range("selection rectangle");
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t yy = 0; yy < height(); yy++)
      for (size_t xx = 0; xx < width(); xx++)
        if (xx < x || xx >= x + w || yy < y || yy >= y + h)
          a[(yy * width() + xx) * 4 + 3] = 0;
    import_rgba(p, a);
  });
}
Image &Image::heal(std::size_t x, std::size_t y, std::size_t w, std::size_t h,
                   std::size_t radius) {
  if (!w || !h || radius == 0 || x > width() || y > height() ||
      w > width() - x || h > height() - y)
    throw std::invalid_argument("healing region");
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    // Fit a plane c + gx*(xx-cx) + gy*(yy-cy) per channel to the ring around
    // the region (least squares), so healed patches follow local shading
    // instead of leaving a flat spot. Falls back to the mean when the ring is
    // too thin to determine a slope.
    const double cx = double(x) + double(w) / 2, cy = double(y) + double(h) / 2;
    double sxx = 0, syy = 0, sxy = 0, sx = 0, sy = 0, n = 0;
    double sv[3] = {}, svx[3] = {}, svy[3] = {};
    size_t x0 = x > radius ? x - radius : 0, y0 = y > radius ? y - radius : 0,
           x1 = std::min(width(), x + w + radius),
           y1 = std::min(height(), y + h + radius);
    for (size_t yy = y0; yy < y1; yy++)
      for (size_t xx = x0; xx < x1; xx++)
        if (xx < x || xx >= x + w || yy < y || yy >= y + h) {
          double dx = double(xx) - cx, dy = double(yy) - cy;
          sxx += dx * dx;
          syy += dy * dy;
          sxy += dx * dy;
          sx += dx;
          sy += dy;
          n++;
          for (int k = 0; k < 3; k++) {
            double v = a[(yy * width() + xx) * 4 + k];
            sv[k] += v;
            svx[k] += v * dx;
            svy[k] += v * dy;
          }
        }
    if (!n)
      throw std::runtime_error("healing region has no boundary");
    // Normal equations for [c, gx, gy]: symmetric 3x3 solved by Cramer's rule.
    double m[3][3] = {{n, sx, sy}, {sx, sxx, sxy}, {sy, sxy, syy}};
    auto det3 = [](double q[3][3]) {
      return q[0][0] * (q[1][1] * q[2][2] - q[1][2] * q[2][1]) -
             q[0][1] * (q[1][0] * q[2][2] - q[1][2] * q[2][0]) +
             q[0][2] * (q[1][0] * q[2][1] - q[1][1] * q[2][0]);
    };
    double d = det3(m);
    for (int k = 0; k < 3; k++) {
      double c = sv[k] / n, gx = 0, gy = 0;
      if (std::abs(d) > 1e-9 * std::max(1.0, n * n * n)) {
        double rhs[3] = {sv[k], svx[k], svy[k]}, sol[3];
        for (int j = 0; j < 3; j++) {
          double t[3][3];
          for (int r = 0; r < 3; r++)
            for (int cidx = 0; cidx < 3; cidx++)
              t[r][cidx] = cidx == j ? rhs[r] : m[r][cidx];
          sol[j] = det3(t) / d;
        }
        c = sol[0];
        gx = sol[1];
        gy = sol[2];
      }
      for (size_t yy = y; yy < y + h; yy++)
        for (size_t xx = x; xx < x + w; xx++)
          a[(yy * width() + xx) * 4 + k] =
              clip(c + gx * (double(xx) - cx) + gy * (double(yy) - cy));
    }
    import_rgba(p, a);
  });
}
Image &Image::perspective(double tlx, double tly, double trx, double try_,
                          double brx, double bry, double blx, double bly) {
  double q[] = {tlx, tly, trx, try_, brx, bry, blx, bly};
  for (double v : q)
    if (!std::isfinite(v))
      throw std::invalid_argument("perspective coordinate");
  return mutate([&](MagickWand *p) {
    double A[8][9] = {{0}};
    double sx[] = {0, 1, 1, 0}, sy[] = {0, 0, 1, 1};
    for (int i = 0; i < 4; i++) {
      double u = q[i * 2], v = q[i * 2 + 1];
      A[i * 2][0] = sx[i];
      A[i * 2][1] = sy[i];
      A[i * 2][2] = 1;
      A[i * 2][6] = -u * sx[i];
      A[i * 2][7] = -u * sy[i];
      A[i * 2][8] = u;
      A[i * 2 + 1][3] = sx[i];
      A[i * 2 + 1][4] = sy[i];
      A[i * 2 + 1][5] = 1;
      A[i * 2 + 1][6] = -v * sx[i];
      A[i * 2 + 1][7] = -v * sy[i];
      A[i * 2 + 1][8] = v;
    }
    for (int c = 0; c < 8; c++) {
      int pivot = c;
      for (int r = c + 1; r < 8; r++)
        if (std::abs(A[r][c]) > std::abs(A[pivot][c]))
          pivot = r;
      if (std::abs(A[pivot][c]) < 1e-12)
        throw std::invalid_argument("degenerate perspective quadrilateral");
      for (int j = c; j < 9; j++)
        std::swap(A[c][j], A[pivot][j]);
      double z = A[c][c];
      for (int j = c; j < 9; j++)
        A[c][j] /= z;
      for (int r = 0; r < 8; r++)
        if (r != c) {
          z = A[r][c];
          for (int j = c; j < 9; j++)
            A[r][j] -= z * A[c][j];
        }
    }
    double H[9] = {A[0][8], A[1][8], A[2][8], A[3][8], A[4][8],
                   A[5][8], A[6][8], A[7][8], 1};
    double d = H[0] * (H[4] * H[8] - H[5] * H[7]) -
               H[1] * (H[3] * H[8] - H[5] * H[6]) +
               H[2] * (H[3] * H[7] - H[4] * H[6]);
    if (std::abs(d) < 1e-12)
      throw std::invalid_argument("singular perspective transform");
    double I[9] = {
        (H[4] * H[8] - H[5] * H[7]) / d, (H[2] * H[7] - H[1] * H[8]) / d,
        (H[1] * H[5] - H[2] * H[4]) / d, (H[5] * H[6] - H[3] * H[8]) / d,
        (H[0] * H[8] - H[2] * H[6]) / d, (H[2] * H[3] - H[0] * H[5]) / d,
        (H[3] * H[7] - H[4] * H[6]) / d, (H[1] * H[6] - H[0] * H[7]) / d,
        (H[0] * H[4] - H[1] * H[3]) / d};
    size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p);
    std::vector<double> a, o;
    export_rgba(p, a);
    o.assign(a.size(), 0);
    for (size_t y = 0; y < h; y++)
      for (size_t x = 0; x < w; x++) {
        double u = w > 1 ? double(x) / (w - 1) : 0,
               v = h > 1 ? double(y) / (h - 1) : 0,
               z = I[6] * u + I[7] * v + I[8];
        if (!std::isfinite(z) || std::abs(z) < 1e-12)
          continue;
        double xx = (I[0] * u + I[1] * v + I[2]) / z,
               yy = (I[3] * u + I[4] * v + I[5]) / z;
        if (!std::isfinite(xx) || !std::isfinite(yy) || xx < -1e-10 ||
            yy < -1e-10 || xx > 1 + 1e-10 || yy > 1 + 1e-10)
          continue;
        sample_bilinear(a, w, h, std::clamp(xx, 0., 1.) * double(w - 1),
                        std::clamp(yy, 0., 1.) * double(h - 1),
                        &o[(y * w + x) * 4]);
      }
    import_rgba(p, o);
  });
}
Image &Image::lens(double d) {
  if (!std::isfinite(d) || std::abs(d) > 2)
    throw std::invalid_argument("lens distortion");
  return mutate([&](MagickWand *p) {
    size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p);
    std::vector<double> a, o;
    export_rgba(p, a);
    o.assign(a.size(), 0);
    double cx = (w - 1) / 2., cy = (h - 1) / 2., s = std::min(w, h) / 2.;
    for (size_t y = 0; y < h; y++)
      for (size_t x = 0; x < w; x++) {
        double nx = (x - cx) / s, ny = (y - cy) / s,
               f = 1 + d * (nx * nx + ny * ny);
        double fx = cx + nx * f * s, fy = cy + ny * f * s;
        // Samples that fall outside the frame stay transparent instead of
        // smearing the border pixels across the corners.
        if (fx < 0 || fy < 0 || fx > double(w - 1) || fy > double(h - 1))
          continue;
        sample_bilinear(a, w, h, fx, fy, &o[(y * w + x) * 4]);
      }
    import_rgba(p, o);
  });
}

bool Image::blend_mode(const std::string &mode) noexcept {
  return mode == "over" || mode == "multiply" || mode == "screen" ||
         mode == "overlay" || mode == "difference" || mode == "darken" ||
         mode == "lighten" || mode == "addition" || mode == "subtract" ||
         mode == "divide";
}
Image &Image::blend(const Image &o, const std::string &mode, double opacity,
                    std::size_t x, std::size_t y) {
  o.require_image();
  if (!std::isfinite(opacity) || opacity < 0 || opacity > 1)
    throw std::invalid_argument("opacity must be in [0,1]");
  if (!blend_mode(mode))
    throw std::invalid_argument("unsupported blend mode");
  return mutate([&](MagickWand *p) {
    size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p),
           ow = o.width(), oh = o.height();
    std::vector<double> a, b;
    export_rgba(p, a);
    export_rgba(o.impl_->wand, b);
    for (size_t yy = 0; yy < oh && y + yy < h; yy++)
      for (size_t xx = 0; xx < ow && x + xx < w; xx++) {
        size_t di = ((y + yy) * w + x + xx) * 4, si = (yy * ow + xx) * 4;
        double as = clip(b[si + 3] * opacity), ab = clip(a[di + 3]),
               ao = as + ab * (1 - as);
        for (int k = 0; k < 3; k++) {
          double cs = b[si + k], cb = a[di + k],
                 mix = mode == "multiply"
                           ? cb * cs
                           : mode == "screen"
                                 ? 1 - (1 - cb) * (1 - cs)
                                 : mode == "overlay"
                                       ? (cb < 0.5 ? 2 * cb * cs
                                                   : 1 - 2 * (1 - cb) * (1 - cs))
                                       : mode == "difference" ? std::abs(cb - cs)
                                       : mode == "darken" ? std::min(cb, cs)
                                       : mode == "lighten" ? std::max(cb, cs)
                                       : mode == "addition" ? std::min(1., cb + cs)
                                       : mode == "subtract" ? std::max(0., cb - cs)
                                       : mode == "divide"
                                           ? (cs > 0 ? std::min(1., cb / cs) : 1.)
                                           : cs;
          double prem = as * (1 - ab) * cs + as * ab * mix + (1 - as) * ab * cb;
          a[di + k] = ao > 1e-15 ? clip(prem / ao) : 0;
        }
        a[di + 3] = ao;
      }
    import_rgba(p, a);
  });
}
Image &Image::rect(double x, double y, double w, double h, Color fill,
                   Color stroke, double sw) {
  return mutate([&](MagickWand *p) {
    std::unique_ptr<DrawingWand, decltype(&DestroyDrawingWand)> drawing(
        NewDrawingWand(), DestroyDrawingWand);
    DrawingWand *d = drawing.get();
    if (!d)
      throw std::runtime_error("drawing allocation failed");
    color_draw(d, fill);
    if (!(sw > 0))
      stroke.a = 0; // a zero-width stroke must not leave a hairline outline
    std::unique_ptr<PixelWand, decltype(&DestroyPixelWand)> q(
        color_wand(stroke), DestroyPixelWand);
    DrawSetStrokeColor(d, q.get());
    DrawSetStrokeWidth(d, sw);
    DrawRectangle(d, x, y, x + w, y + h);
    ok(MagickDrawImage(p, d), p, "draw rectangle");
  });
}
Image &Image::circle(double x, double y, double r, Color fill, Color stroke,
                     double sw) {
  return mutate([&](MagickWand *p) {
    std::unique_ptr<DrawingWand, decltype(&DestroyDrawingWand)> drawing(
        NewDrawingWand(), DestroyDrawingWand);
    DrawingWand *d = drawing.get();
    if (!d)
      throw std::runtime_error("drawing allocation failed");
    color_draw(d, fill);
    if (!(sw > 0))
      stroke.a = 0;
    std::unique_ptr<PixelWand, decltype(&DestroyPixelWand)> q(
        color_wand(stroke), DestroyPixelWand);
    DrawSetStrokeColor(d, q.get());
    DrawSetStrokeWidth(d, sw);
    DrawCircle(d, x, y, x + r, y);
    ok(MagickDrawImage(p, d), p, "draw circle");
  });
}
Image &Image::line(double x1, double y1, double x2, double y2, Color c,
                   double sw) {
  return mutate([&](MagickWand *p) {
    std::unique_ptr<DrawingWand, decltype(&DestroyDrawingWand)> drawing(
        NewDrawingWand(), DestroyDrawingWand);
    DrawingWand *d = drawing.get();
    if (!d)
      throw std::runtime_error("drawing allocation failed");
    std::unique_ptr<PixelWand, decltype(&DestroyPixelWand)> ink(
        color_wand(c), DestroyPixelWand);
    DrawSetStrokeColor(d, ink.get());
    DrawSetStrokeWidth(d, sw);
    DrawLine(d, x1, y1, x2, y2);
    ok(MagickDrawImage(p, d), p, "draw line");
  });
}
Image &Image::text(double x, double y, const std::string &value, double size,
                   Color c, const std::string &font) {
  if (!std::isfinite(x) || !std::isfinite(y) || size <= 0 || size > 4096 ||
      !std::isfinite(size) || font != "embedded" || value.size() > 65536 ||
      std::abs(x) > 100000000 || std::abs(y) > 100000000 ||
      std::any_of(value.begin(), value.end(),
                  [](unsigned char ch) { return ch >= 128; }))
    throw std::invalid_argument("text requires bounded finite coordinates, "
                                "ASCII text, and embedded font");
  valid_color(c);
  c.r = clip(c.r);
  c.g = clip(c.g);
  c.b = clip(c.b);
  c.a = clip(c.a);
  membudget::require(
      std::size_t(std::ceil(size)) * std::size_t(std::ceil(size)) * 4 + 4096);
  return mutate([&](MagickWand *p) {
#if LMIMG_EMBEDDED_FONT
    stbtt_fontinfo fi;
    if (!stbtt_InitFont(&fi, embedded_font,
                        stbtt_GetFontOffsetForIndex(embedded_font, 0)))
      throw std::runtime_error("embedded font initialization failed");
    size_t w = MagickGetImageWidth(p), h = MagickGetImageHeight(p);
    std::vector<double> a;
    export_rgba(p, a);
    float scale = stbtt_ScaleForPixelHeight(&fi, float(size));
    int asc, des, lg;
    stbtt_GetFontVMetrics(&fi, &asc, &des, &lg);
    int pen = int(std::lround(x));
    int baseline = int(std::lround(y));
    for (unsigned char ch : value) {
      if (ch == '\n') {
        pen = int(std::lround(x));
        baseline += int(std::lround(size));
        continue;
      }
      int adv, lsb;
      stbtt_GetCodepointHMetrics(&fi, ch, &adv, &lsb);
      int bw, bh, ox, oy;
      unsigned char *bm =
          stbtt_GetCodepointBitmap(&fi, 0, scale, ch, &bw, &bh, &ox, &oy);
      if (!bm && bw > 0 && bh > 0)
        throw std::runtime_error("embedded glyph allocation failed");
      for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++) {
          int px = pen + ox + bx, py = baseline + oy + by;
          if (px < 0 || py < 0 || size_t(px) >= w || size_t(py) >= h)
            continue;
          double as = c.a * (bm[by * bw + bx] / 255.0),
                 ab = a[(size_t(py) * w + size_t(px)) * 4 + 3],
                 ao = as + ab * (1 - as);
          double ink[3] = {c.r, c.g, c.b};
          for (int k = 0; k < 3; k++)
            a[(size_t(py) * w + size_t(px)) * 4 + k] =
                ao > 1e-15 ? (as * ink[k] +
                              (1 - as) * ab *
                                  a[(size_t(py) * w + size_t(px)) * 4 + k]) /
                                 ao
                           : a[(size_t(py) * w + size_t(px)) * 4 + k];
          a[(size_t(py) * w + size_t(px)) * 4 + 3] = ao;
        }
      stbtt_FreeBitmap(bm, nullptr);
      pen += int(std::lround(adv * scale));
    }
    import_rgba(p, a);
#else
    (void)p;
    (void)value;
    (void)x;
    (void)y;
    (void)size;
    (void)c;
    throw std::runtime_error("embedded font is unavailable in this build");
#endif
  });
}
double Image::text_width(const std::string &value, double size) {
  if (size <= 0 || size > 4096 || !std::isfinite(size) ||
      value.size() > 65536 ||
      std::any_of(value.begin(), value.end(),
                  [](unsigned char ch) { return ch >= 128; }))
    throw std::invalid_argument(
        "text_width requires a finite size in (0,4096] and ASCII text");
#if LMIMG_EMBEDDED_FONT
  stbtt_fontinfo fi;
  if (!stbtt_InitFont(&fi, embedded_font,
                      stbtt_GetFontOffsetForIndex(embedded_font, 0)))
    throw std::runtime_error("embedded font initialization failed");
  float scale = stbtt_ScaleForPixelHeight(&fi, float(size));
  long widest = 0, pen = 0;
  for (unsigned char ch : value) {
    if (ch == '\n') {
      widest = std::max(widest, pen);
      pen = 0;
      continue;
    }
    int adv, lsb;
    stbtt_GetCodepointHMetrics(&fi, ch, &adv, &lsb);
    pen += std::lround(adv * scale); // identical advance rounding to text()
  }
  return double(std::max(widest, pen));
#else
  throw std::runtime_error("embedded font is unavailable in this build");
#endif
}
Image &Image::metadata(const std::string &k, const std::string &v) {
  if (k.empty() || k.size() > 256 || v.size() > 65536 ||
      k.find('\0') != std::string::npos || v.find('\0') != std::string::npos)
    throw std::invalid_argument("metadata key or value exceeds limit");
  constexpr std::size_t cap = 1024 * 1024;
  require_image();
  std::size_t add = k.size() + v.size() + 256;
  if (impl_->metadata_bytes > cap || add > cap - impl_->metadata_bytes)
    throw std::invalid_argument("image metadata exceeds 1 MiB");
  membudget::require(add);
  mutate(
      [&](MagickWand *p) {
        ok(MagickSetImageProperty(p, k.c_str(), v.c_str()), p, "set metadata");
      },
      add);
  return *this;
}
std::string Image::property(const std::string &k) const {
  require_image();
  if (k.empty() || k.size() > 256 || k.find('\0') != std::string::npos)
    throw std::invalid_argument("invalid metadata key");
  std::unique_ptr<char, decltype(&MagickRelinquishMemory)> v(
      MagickGetImageProperty(impl_->wand, k.c_str()), MagickRelinquishMemory);
  std::string out = v ? v.get() : "";
  if (out.size() > 65536)
    throw std::runtime_error("metadata value exceeds limit");
  return out;
}

Stats Image::stats() const {
  require_image();
  Stats s;
  s.width = width();
  s.height = height();
  s.pixels = s.width * s.height;
  std::vector<double> px(s.pixels * 4);
  ok(MagickExportImagePixels(impl_->wand, 0, 0, s.width, s.height, "RGBA",
                             DoublePixel, px.data()),
     impl_->wand, "export pixels");
  double mean[4] = {}, m2[4] = {}, lo[4] = {1, 1, 1, 1}, hi[4] = {};
  for (size_t i = 0; i < s.pixels; i++)
    for (int k = 0; k < 4; k++) {
      double v = px[i * 4 + k];
      double delta = v - mean[k];
      mean[k] += delta / double(i + 1);
      m2[k] += delta * (v - mean[k]);
      lo[k] = std::min(lo[k], v);
      hi[k] = std::max(hi[k], v);
    }
  double *n[] = {&s.mean.r, &s.mean.g, &s.mean.b, &s.mean.a};
  double *d[] = {&s.standard_deviation.r, &s.standard_deviation.g,
                 &s.standard_deviation.b, &s.standard_deviation.a};
  double *mn[] = {&s.minimum.r, &s.minimum.g, &s.minimum.b, &s.minimum.a};
  double *mx[] = {&s.maximum.r, &s.maximum.g, &s.maximum.b, &s.maximum.a};
  for (int k = 0; k < 4; k++) {
    *n[k] = mean[k];
    *d[k] = std::sqrt(std::max(0., m2[k] / s.pixels));
    *mn[k] = lo[k];
    *mx[k] = hi[k];
  }
  return s;
}
std::string Image::histogram_csv() const {
  require_image();
  std::vector<double> px(width() * height() * 4);
  ok(MagickExportImagePixels(impl_->wand, 0, 0, width(), height(), "RGBA",
                             DoublePixel, px.data()),
     impl_->wand, "export histogram pixels");
  std::size_t bins[256] = {};
  for (size_t i = 0; i < width() * height(); i++) {
    int v = int(std::lround(
        255 * clip((px[i * 4] + px[i * 4 + 1] + px[i * 4 + 2]) / 3)));
    bins[v]++;
  }
  std::ostringstream out;
  out << "bin,count\n";
  for (int i = 0; i < 256; i++)
    out << i << "," << bins[i] << "\n";
  return out.str();
}
double Image::psnr(const Image &o) const {
  require_image();
  o.require_image();
  if (width() != o.width() || height() != o.height())
    throw std::invalid_argument("PSNR dimensions differ");
  std::vector<double> a(width() * height() * 4), b(a.size());
  ok(MagickExportImagePixels(impl_->wand, 0, 0, width(), height(), "RGBA",
                             DoublePixel, a.data()),
     impl_->wand, "export PSNR");
  ok(MagickExportImagePixels(o.impl_->wand, 0, 0, width(), height(), "RGBA",
                             DoublePixel, b.data()),
     o.impl_->wand, "export PSNR");
  double e = 0;
  for (size_t i = 0; i < a.size(); i++) {
    double d = a[i] - b[i];
    e += d * d;
  }
  e /= a.size();
  return e == 0 ? std::numeric_limits<double>::infinity()
                : 10 * std::log10(1 / e);
}
double Image::ssim(const Image &o) const {
  require_image();
  o.require_image();
  if (width() != o.width() || height() != o.height())
    throw std::invalid_argument("SSIM dimensions differ");
  size_t n = width() * height();
  std::vector<double> a(n * 4), b(n * 4);
  ok(MagickExportImagePixels(impl_->wand, 0, 0, width(), height(), "RGBA",
                             DoublePixel, a.data()),
     impl_->wand, "export SSIM");
  ok(MagickExportImagePixels(o.impl_->wand, 0, 0, width(), height(), "RGBA",
                             DoublePixel, b.data()),
     o.impl_->wand, "export SSIM");
  double m1 = 0, m2 = 0;
  std::vector<double> x(n), y(n);
  for (size_t i = 0; i < n; i++) {
    x[i] = .2126 * a[i * 4] + .7152 * a[i * 4 + 1] + .0722 * a[i * 4 + 2];
    y[i] = .2126 * b[i * 4] + .7152 * b[i * 4 + 1] + .0722 * b[i * 4 + 2];
    m1 += x[i];
    m2 += y[i];
  }
  m1 /= n;
  m2 /= n;
  double v1 = 0, v2 = 0, cov = 0;
  for (size_t i = 0; i < n; i++) {
    double dx = x[i] - m1, dy = y[i] - m2;
    v1 += dx * dx;
    v2 += dy * dy;
    cov += dx * dy;
  }
  double den = n > 1 ? double(n - 1) : 1;
  v1 /= den;
  v2 /= den;
  cov /= den;
  double c1 = .0001, c2 = .0009;
  return ((2 * m1 * m2 + c1) * (2 * cov + c2)) /
         ((m1 * m1 + m2 * m2 + c1) * (v1 + v2 + c2));
}

Image Image::pattern(std::size_t w, std::size_t h, const std::string &r,
                     const std::string &g, const std::string &b,
                     const std::string &a) {
  valid_size(w, h);
  Expr er(r, {"x", "y"}), eg(g, {"x", "y"}), eb(b, {"x", "y"}),
      ea(a, {"x", "y"});
  membudget::require(w * h * 4 * sizeof(double));
  std::vector<double> px(w * h * 4);
  for (size_t y = 0; y < h; y++)
    for (size_t x = 0; x < w; x++) {
      double v[2] = {w > 1 ? double(x) / (w - 1) : 0,
                     h > 1 ? double(y) / (h - 1) : 0};
      size_t i = (y * w + x) * 4;
      px[i] = clip(er.eval(v));
      px[i + 1] = clip(eg.eval(v));
      px[i + 2] = clip(eb.eval(v));
      px[i + 3] = clip(ea.eval(v));
    }
  Image out = create(w, h);
  ok(MagickSetImageAlphaChannel(out.impl_->wand, ActivateAlphaChannel),
     out.impl_->wand, "activate alpha");
  ok(MagickImportImagePixels(out.impl_->wand, 0, 0, w, h, "RGBA", DoublePixel,
                             px.data()),
     out.impl_->wand, "import pattern pixels");
  return out;
}

Image montage(const std::vector<Image> &xs, std::size_t cols, std::size_t gap,
              Color bg) {
  if (xs.empty() || !cols)
    throw std::invalid_argument("montage requires images and columns");
  if (cols > 2147483647 || gap > 2147483647)
    throw std::invalid_argument("montage dimensions exceed range");
  size_t rows = xs.size() / cols + (xs.size() % cols != 0), mw = 0, mh = 0;
  for (auto &x : xs) {
    mw = std::max(mw, x.width());
    mh = std::max(mh, x.height());
  }
  auto extent = [&](size_t n, size_t unit) {
    if (n > std::numeric_limits<size_t>::max() / (unit + gap) ||
        n * (unit + gap) > std::numeric_limits<size_t>::max() - gap)
      throw std::invalid_argument("montage dimensions overflow");
    return n * (unit + gap) + gap;
  };
  Image out = Image::create(extent(cols, mw), extent(rows, mh), bg);
  for (size_t i = 0; i < xs.size(); i++)
    out.blend(xs[i], "over", 1, gap + (i % cols) * (mw + gap),
              gap + (i / cols) * (mh + gap));
  return out;
}
Animation &Animation::add(const Image &f, std::size_t d) {
  if (d > 65535)
    throw std::invalid_argument("GIF delay too large");
  frames_.push_back({f.clone(), d});
  return *this;
}
void Animation::save(const std::string &path) const {
  if (frames_.empty())
    throw std::runtime_error("empty animation");
  check_path(path);
  if (path_format(path) != "GIF")
    throw std::invalid_argument("animation output must use GIF format");
  std::size_t total = 0;
  std::size_t w = 0, h = 0;
  for (auto &f : frames_) {
    f.image.require_image();
    if (!w) {
      w = f.image.width();
      h = f.image.height();
    }
    if (f.image.width() != w || f.image.height() != h)
      throw std::invalid_argument(
          "animation frames must have equal dimensions");
    if (total > std::numeric_limits<std::size_t>::max() - w * h)
      throw std::invalid_argument("animation is too large");
    total += w * h;
  }
  if (total > (std::numeric_limits<std::size_t>::max() - 4096) / 32)
    throw std::invalid_argument("animation is too large");
  membudget::require(total * 32 + 4096);
  init();
  std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> ww(
      NewMagickWand(), DestroyMagickWand);
  if (!ww)
    throw std::runtime_error("animation allocation failed");
  for (auto &f : frames_) {
    std::unique_ptr<MagickWand, decltype(&DestroyMagickWand)> p(
        CloneMagickWand(f.image.impl_.get()->wand), DestroyMagickWand);
    if (!p)
      fail(ww.get(), "clone animation frame");
    ok(MagickSetImageDelay(p.get(), f.delay), p.get(), "set animation delay");
    if (MagickAddImage(ww.get(), p.get()) == MagickFalse)
      fail(ww.get(), "add animation frame");
  }
  ok(MagickWriteImages(ww.get(), path.c_str(), MagickTrue), ww.get(),
     "write animation");
}
} // namespace lmimg
