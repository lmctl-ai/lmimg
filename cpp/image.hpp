#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lmimg {

struct Color {
  double r = 0, g = 0, b = 0, a = 1;
};
struct Stats {
  std::size_t width = 0, height = 0, pixels = 0;
  Color mean, standard_deviation;
  Color minimum, maximum;
};

class Image {
public:
  Image();
  Image(Image &&) noexcept;
  Image &operator=(Image &&) noexcept;
  Image(const Image &);
  Image &operator=(const Image &);
  ~Image();

  static Image create(std::size_t width, std::size_t height, Color fill = {});
  static Image open(const std::string &path);
  Image clone() const;
  void save(const std::string &path) const;
  void save(const std::string &path, const std::string &format) const;

  std::size_t width() const noexcept;
  std::size_t height() const noexcept;
  Color pixel(std::size_t x, std::size_t y) const;
  Image &set_pixel(std::size_t x, std::size_t y, Color c);

  Image &resize(std::size_t width, std::size_t height);
  Image &crop(std::size_t x, std::size_t y, std::size_t width,
              std::size_t height);
  Image &rotate(double degrees, Color background = {});
  Image &flip(bool horizontal = false);
  Image &levels(double black, double gamma, double white);
  Image &curves(const std::vector<double> &control_points);
  Image &hsl(double hue_degrees, double saturation_scale = 1.0,
             double lightness_scale = 1.0);
  Image &channel_mix(const std::array<double, 9> &matrix);
  Image &grayscale();
  Image &sepia(double strength = 80.0);
  Image &blur(double radius, double sigma);
  Image &sharpen(double radius, double sigma);
  Image &edges(double radius = 1.0);
  Image &unsharp(double radius, double sigma, double amount,
                 double threshold = 0.05);
  Image &vignette(double radius = 0.0, double sigma = 0.0);
  Image &tilt_shift(double blur_radius, double focus_start, double focus_end);
  Image &morphology(const std::string &operation, std::size_t radius = 1);
  Image &apply_mask(const Image &mask);
  Image &selection(std::size_t x, std::size_t y, std::size_t width,
                   std::size_t height);
  Image &heal(std::size_t x, std::size_t y, std::size_t width,
              std::size_t height, std::size_t radius = 4);
  Image &perspective(double top_left_x, double top_left_y, double top_right_x,
                     double top_right_y, double bottom_right_x,
                     double bottom_right_y, double bottom_left_x,
                     double bottom_left_y);
  Image &lens(double distortion);
  Image &blend(const Image &overlay, const std::string &mode = "over",
               double opacity = 1.0, std::size_t x = 0, std::size_t y = 0);
  Image &rect(double x, double y, double w, double h, Color fill,
              Color stroke = {}, double stroke_width = 0);
  Image &circle(double x, double y, double radius, Color fill,
                Color stroke = {}, double stroke_width = 0);
  Image &line(double x1, double y1, double x2, double y2, Color color,
              double width = 1);
  Image &text(double x, double y, const std::string &value, double point_size,
              Color color = {1, 1, 1, 1}, const std::string &font = "embedded");
  Image &metadata(const std::string &key, const std::string &value);
  std::string property(const std::string &key) const;

  Stats stats() const;
  std::string histogram_csv() const;
  double psnr(const Image &other) const;
  double ssim(const Image &other) const;

  // A deterministic procedural RGBA image. Variables are x and y in [0,1].
  static Image pattern(std::size_t width, std::size_t height,
                       const std::string &red, const std::string &green,
                       const std::string &blue, const std::string &alpha = "1");

private:
  friend class Animation;
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit Image(std::unique_ptr<Impl> impl);
  void require_image() const;
  template <class F> Image &mutate(F &&f, std::size_t metadata_add = 0);
};

class Animation {
public:
  Animation &add(const Image &frame, std::size_t delay_centiseconds = 10);
  void save(const std::string &path) const;
  std::size_t size() const noexcept { return frames_.size(); }

private:
  struct Frame {
    Image image;
    std::size_t delay;
  };
  std::vector<Frame> frames_;
};

Image montage(const std::vector<Image> &images, std::size_t columns,
              std::size_t gap = 0, Color background = {});

} // namespace lmimg
