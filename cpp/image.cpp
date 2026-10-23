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
         e == "pgm";
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
  if (e == "PPM" || e == "PGM")
    e = "PNM";
  return e;
}
PixelWand *color_wand(Color c) {
  valid_color(c);
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
  auto file_bytes = std::filesystem::file_size(path);
  if (file_bytes > (std::numeric_limits<size_t>::max() - 2 * 1024 * 1024) / 2)
    throw std::invalid_argument("image file too large");
  membudget::require(static_cast<size_t>(file_bytes) * 2 + 2 * 1024 * 1024);
  MagickWand *probe = NewMagickWand();
  if (!probe)
    throw std::runtime_error("image allocation failed");
  try {
    ok(MagickPingImage(probe, read_path.c_str()), probe, "inspect image");
    if (MagickGetNumberImages(probe) > 1)
      throw std::invalid_argument("multi-frame input is not supported");
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
    if (MagickGetNumberImages(p) > 1)
      throw std::invalid_argument("multi-frame input is not supported");
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
Image &Image::curves(const std::vector<double> &cp) {
  if (cp.size() < 4 || cp.size() % 2 || cp.front() != 0 ||
      cp[cp.size() - 2] != 1)
    throw std::invalid_argument("curves needs endpoints at 0 and 1");
  for (size_t i = 0; i < cp.size(); i++)
    if (!std::isfinite(cp[i]) || cp[i] < 0 || cp[i] > 1 ||
        (i % 2 == 0 && i && cp[i] <= cp[i - 2]))
      throw std::invalid_argument(
          "curve points must be finite and ordered in [0,1]");
  return mutate([&](MagickWand *p) {
    std::vector<double> a;
    export_rgba(p, a);
    for (size_t i = 0; i < a.size(); i += 4)
      for (int k = 0; k < 3; k++) {
        double x = clip(a[i + k]), y = cp[1];
        if (x >= cp[cp.size() - 2])
          y = cp.back();
        else
          for (size_t j = 0; j + 2 < cp.size(); j += 2)
            if (x <= cp[j + 2]) {
              double t = (x - cp[j]) / (cp[j + 2] - cp[j]);
              y = cp[j + 1] * (1 - t) + cp[j + 3] * t;
              break;
            }
        a[i + k] = clip(y);
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
  return mutate([&](MagickWand *p) {
    rgb_filter(p, [&] { ok(MagickBlurImage(p, r, sigma), p, "blur"); });
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
           r = radius > 0 ? radius : std::min(w, h) / 2.;
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
      double z = double(y) / (h ? h - 1 : 1);
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
    double sum[3] = {}, n = 0;
    size_t x0 = x > radius ? x - radius : 0, y0 = y > radius ? y - radius : 0,
           x1 = std::min(width(), x + w + radius),
           y1 = std::min(height(), y + h + radius);
    for (size_t yy = y0; yy < y1; yy++)
      for (size_t xx = x0; xx < x1; xx++)
        if (xx < x || xx >= x + w || yy < y || yy >= y + h) {
          for (int k = 0; k < 3; k++)
            sum[k] += a[(yy * width() + xx) * 4 + k];
          n++;
        }
    if (!n)
      throw std::runtime_error("healing region has no boundary");
    for (size_t yy = y; yy < y + h; yy++)
      for (size_t xx = x; xx < x + w; xx++)
        for (int k = 0; k < 3; k++)
          a[(yy * width() + xx) * 4 + k] = sum[k] / n;
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
        size_t ix = std::min(w - 1, size_t(std::lround(std::clamp(xx, 0., 1.) *
                                                       (w - 1)))),
               iy = std::min(h - 1, size_t(std::lround(std::clamp(yy, 0., 1.) *
                                                       (h - 1))));
        for (int k = 0; k < 4; k++)
          o[(y * w + x) * 4 + k] = a[(iy * w + ix) * 4 + k];
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
        double fx = std::clamp(cx + nx * f * s, 0., double(w - 1)),
               fy = std::clamp(cy + ny * f * s, 0., double(h - 1));
        size_t sx = size_t(std::lround(fx)), sy = size_t(std::lround(fy));
        for (int k = 0; k < 4; k++)
          o[(y * w + x) * 4 + k] = a[(sy * w + sx) * 4 + k];
      }
    import_rgba(p, o);
  });
}

Image &Image::blend(const Image &o, const std::string &mode, double opacity,
                    std::size_t x, std::size_t y) {
  o.require_image();
  if (!std::isfinite(opacity) || opacity < 0 || opacity > 1)
    throw std::invalid_argument("opacity must be in [0,1]");
  if (mode != "over" && mode != "multiply" && mode != "screen")
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
                           : (mode == "screen" ? 1 - (1 - cb) * (1 - cs) : cs);
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
