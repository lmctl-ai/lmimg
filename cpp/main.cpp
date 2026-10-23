#include "lmimg_dslvm.hpp"
#include "expr.hpp"
#include "image.hpp"
#include "memory_budget.hpp"
#include "notices_embedded.hpp"
#include "schema_embedded.hpp"
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
namespace {
using dslvm::Args;
using dslvm::Value;
using lmimg::Animation;
using lmimg::Image;
struct Curve {
  std::vector<double> points;
};
struct LayerStack {
  struct Layer {
    Image image;
    std::string mode;
    double opacity;
    size_t x, y;
  };
  Image background;
  std::vector<Layer> layers;
};
struct Montage {
  std::vector<Image> images;
};
struct Host {
  std::vector<std::unique_ptr<Curve>> curves;
  std::vector<std::unique_ptr<LayerStack>> stacks;
  std::vector<std::unique_ptr<Montage>> montages;
  std::vector<std::unique_ptr<Image>> images;
  std::vector<std::unique_ptr<Animation>> animations;
  template <class T>
  Value hold(std::vector<std::unique_ptr<T>> &arena, const char *type,
             T value) {
    arena.push_back(std::make_unique<T>(std::move(value)));
    return Value::object(type, arena.back().get());
  }
  Value image(Image value) { return hold(images, "Image", std::move(value)); }
};
Image &image(void *p) { return *static_cast<Image *>(p); }
Animation &animation(void *p) { return *static_cast<Animation *>(p); }
size_t integer(const Value &v, size_t lo = 0, size_t hi = 2147483647) {
  if (v.i < 0 || static_cast<uint64_t>(v.i) < lo ||
      static_cast<uint64_t>(v.i) > hi)
    throw dslvm::Error("integer outside allowed range");
  return static_cast<size_t>(v.i);
}
lmimg::Color color(Args &a, size_t start) {
  return {a[start].f, a[start + 1].f, a[start + 2].f, a[start + 3].f};
}
double channel(lmimg::Color c, const Value &v) {
  switch (integer(v, 0, 3)) {
  case 0:
    return c.r;
  case 1:
    return c.g;
  case 2:
    return c.b;
  default:
    return c.a;
  }
}
void output(const std::string &path, const std::string &data) {
  std::filesystem::path dest(path);
  if (dest.has_parent_path())
    std::filesystem::create_directories(dest.parent_path());
  std::ofstream f(path, std::ios::binary);
  if (!f)
    throw dslvm::Error("cannot write file: " + path);
  f.write(data.data(), static_cast<std::streamsize>(data.size()));
  f.close();
  if (!f)
    throw dslvm::Error("file write failed: " + path);
}
void check(dslvm::Registry &r, const char *type, const char *name,
           std::vector<std::string> ps, std::string ret, dslvm::Method fn) {
  std::string who = std::string(type) + "." + name;
  r.method(type, name, ps, ret, [who, ps, fn](void *self, Args &a) {
    for (size_t i = 0; i < ps.size(); ++i) {
      const auto &v = a[i];
      bool valid = ps[i] == "float"    ? v.kind == Value::FLOAT
                   : ps[i] == "int"    ? v.kind == Value::INT
                   : ps[i] == "string" ? v.kind == Value::STR
                   : ps[i] == "bool"
                       ? v.kind == Value::BOOL
                       : v.kind == Value::OBJ && v.obj && v.type == ps[i];
      if (!valid)
        throw dslvm::Error(who + " argument " + std::to_string(i + 1) +
                           " must be " + ps[i]);
      if (v.kind == Value::FLOAT && !std::isfinite(v.f))
        throw dslvm::Error(who + " requires finite arguments");
    }
    try {
      return fn(self, a);
    } catch (const membudget::Exceeded &) {
      throw;
    } catch (const dslvm::Error &) {
      throw;
    } catch (const std::exception &e) {
      throw dslvm::Error(e.what());
    }
  });
}
void register_host(dslvm::Registry &r, Host &h) {
  r.context("img", "Img");
  check(r, "Image", "property", {"string"}, "string", [](void *p, Args &a) {
    return Value::string(image(p).property(a[0].s));
  });
  check(r, "Img", "curve", {}, "Curve", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return h.hold(h.curves, "Curve", Curve{});
  });
  check(r, "Curve", "point", {"float", "float"}, "Curve",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          auto &c = *static_cast<Curve *>(p);
          if (a[0].f < 0 || a[0].f > 1 || a[1].f < 0 || a[1].f > 1 ||
              (!c.points.empty() && a[0].f <= c.points[c.points.size() - 2]))
            throw dslvm::Error(
                "curve points must increase in x and lie in [0,1]");
          c.points.push_back(a[0].f);
          c.points.push_back(a[1].f);
          return Value::object("Curve", p);
        });
  check(r, "Image", "curves", {"Curve"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).curves(static_cast<Curve *>(a[0].obj)->points);
          return Value::object("Image", p);
        });
  check(r, "Image", "hsl", {"float", "float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).hsl(a[0].f, a[1].f, a[2].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "channelmix",
        {"float", "float", "float", "float", "float", "float", "float", "float",
         "float"},
        "Image", [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).channel_mix({a[0].f, a[1].f, a[2].f, a[3].f, a[4].f, a[5].f,
                                a[6].f, a[7].f, a[8].f});
          return Value::object("Image", p);
        });
  check(r, "Image", "morphology", {"string", "int"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).morphology(a[0].s, integer(a[1], 0, 16));
          return Value::object("Image", p);
        });
  check(r, "Image", "mask", {"Image"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).apply_mask(image(a[0].obj));
          return Value::object("Image", p);
        });
  check(r, "Image", "selection", {"int", "int", "int", "int"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).selection(integer(a[0]), integer(a[1]), integer(a[2]),
                             integer(a[3]));
          return Value::object("Image", p);
        });
  check(r, "Image", "heal", {"int", "int", "int", "int", "int"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).heal(integer(a[0]), integer(a[1]), integer(a[2]),
                        integer(a[3]), integer(a[4]));
          return Value::object("Image", p);
        });
  check(
      r, "Image", "perspective",
      {"float", "float", "float", "float", "float", "float", "float", "float"},
      "Image", [&h](void *p, Args &a) -> Value {
        (void)h;
        (void)p;
        (void)a;
        image(p).perspective(a[0].f, a[1].f, a[2].f, a[3].f, a[4].f, a[5].f,
                             a[6].f, a[7].f);
        return Value::object("Image", p);
      });
  check(r, "Image", "lens", {"float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).lens(a[0].f);
          return Value::object("Image", p);
        });
  check(r, "Img", "layers", {"int", "int"}, "Layers",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          LayerStack stack;
          stack.background =
              Image::create(integer(a[0], 1), integer(a[1], 1), {0, 0, 0, 0});
          return h.hold(h.stacks, "Layers", std::move(stack));
        });
  check(r, "Layers", "add", {"Image", "string", "float", "int", "int"},
        "Layers", [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          if (a[2].f < 0 || a[2].f > 1)
            throw dslvm::Error("layer opacity must be in [0,1]");
          if (a[1].s != "over" && a[1].s != "multiply" && a[1].s != "screen" &&
              a[1].s != "overlay" && a[1].s != "difference" &&
              a[1].s != "darken" && a[1].s != "lighten" &&
              a[1].s != "addition" && a[1].s != "subtract")
            throw dslvm::Error("unsupported layer blend");
          auto &stack = *static_cast<LayerStack *>(p);
          stack.layers.push_back({image(a[0].obj).clone(), a[1].s, a[2].f,
                                  integer(a[3]), integer(a[4])});
          return Value::object("Layers", p);
        });
  check(r, "Layers", "masked",
        {"Image", "Image", "string", "float", "int", "int"}, "Layers",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          if (a[3].f < 0 || a[3].f > 1)
            throw dslvm::Error("layer opacity must be in [0,1]");
          if (a[2].s != "over" && a[2].s != "multiply" && a[2].s != "screen" &&
              a[2].s != "overlay" && a[2].s != "difference" &&
              a[2].s != "darken" && a[2].s != "lighten" &&
              a[2].s != "addition" && a[2].s != "subtract")
            throw dslvm::Error("unsupported layer blend");
          auto &stack = *static_cast<LayerStack *>(p);
          auto im = image(a[0].obj).clone();
          im.apply_mask(image(a[1].obj));
          stack.layers.push_back(
              {std::move(im), a[2].s, a[3].f, integer(a[4]), integer(a[5])});
          return Value::object("Layers", p);
        });
  check(r, "Layers", "render", {}, "Image", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    auto &stack = *static_cast<LayerStack *>(p);
    auto out = stack.background.clone();
    for (auto &layer : stack.layers)
      out.blend(layer.image, layer.mode, layer.opacity, layer.x, layer.y);
    return h.image(std::move(out));
  });
  check(r, "Img", "montage", {}, "Montage", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return h.hold(h.montages, "Montage", Montage{});
  });
  check(r, "Montage", "add", {"Image"}, "Montage",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          static_cast<Montage *>(p)->images.push_back(image(a[0].obj).clone());
          return Value::object("Montage", p);
        });
  check(r, "Montage", "render",
        {"int", "int", "float", "float", "float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return h.image(lmimg::montage(static_cast<Montage *>(p)->images,
                                        integer(a[0], 1), integer(a[1]),
                                        color(a, 2)));
        });

  check(r, "Img", "canvas", {"int", "int", "float", "float", "float", "float"},
        "Image", [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return h.image(
              Image::create(integer(a[0], 1), integer(a[1], 1), color(a, 2)));
        });
  check(r, "Img", "pattern",
        {"int", "int", "string", "string", "string", "string"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return h.image(Image::pattern(integer(a[0], 1), integer(a[1], 1),
                                        a[2].s, a[3].s, a[4].s, a[5].s));
        });
  check(r, "Img", "open", {"string"}, "Image", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return h.image(Image::open(a[0].s));
  });
  check(r, "Img", "animation", {}, "Animation",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return h.hold(h.animations, "Animation", Animation{});
        });
  check(r, "Img", "panel", {"Image", "Image", "int"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return h.image(lmimg::montage({image(a[0].obj), image(a[1].obj)}, 2,
                                        integer(a[2]), {1, 1, 1, 1}));
        });
  check(r, "Img", "eval", {"string"}, "float", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    lmimg::Expr e(a[0].s, {});
    double v = e.eval(nullptr);
    if (!std::isfinite(v))
      throw dslvm::Error("nonfinite expression result");
    return Value::real(v);
  });
  check(r, "Img", "assert_close", {"float", "float", "float"}, "void",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          if (a[2].f < 0 || std::abs(a[0].f - a[1].f) > a[2].f)
            throw dslvm::Error("assert_close failed");
          return Value::none();
        });
  check(r, "Img", "assert_true", {"bool"}, "void",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          if (!a[0].b)
            throw dslvm::Error("assert_true failed");
          return Value::none();
        });
  check(r, "Img", "print", {"string"}, "void", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    std::cout << a[0].s << "\n";
    return Value::none();
  });
  check(r, "Image", "copy", {}, "Image", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return h.image(image(p).clone());
  });
  check(r, "Image", "width", {}, "int", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return Value::integer(image(p).width());
  });
  check(r, "Image", "height", {}, "int", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return Value::integer(image(p).height());
  });
  check(r, "Image", "pixel", {"int", "int", "int"}, "float",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return Value::real(
              channel(image(p).pixel(integer(a[0]), integer(a[1])), a[2]));
        });
  check(r, "Image", "set_pixel",
        {"int", "int", "float", "float", "float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).set_pixel(integer(a[0]), integer(a[1]), color(a, 2));
          return Value::object("Image", p);
        });
  check(r, "Image", "resize", {"int", "int"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).resize(integer(a[0], 1), integer(a[1], 1));
          return Value::object("Image", p);
        });
  check(r, "Image", "crop", {"int", "int", "int", "int"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).crop(integer(a[0]), integer(a[1]), integer(a[2]),
                        integer(a[3]));
          return Value::object("Image", p);
        });
  check(r, "Image", "rotate", {"float", "float", "float", "float", "float"},
        "Image", [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).rotate(a[0].f, color(a, 1));
          return Value::object("Image", p);
        });
  check(r, "Image", "flip", {"bool"}, "Image", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    image(p).flip(a[0].b);
    return Value::object("Image", p);
  });
  check(r, "Image", "levels", {"float", "float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).levels(a[0].f, a[1].f, a[2].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "grayscale", {}, "Image", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    image(p).grayscale();
    return Value::object("Image", p);
  });
  check(r, "Image", "sepia", {"float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).sepia(a[0].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "blur", {"float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).blur(a[0].f, a[1].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "sharpen", {"float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).sharpen(a[0].f, a[1].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "edges", {"float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).edges(a[0].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "unsharp", {"float", "float", "float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).unsharp(a[0].f, a[1].f, a[2].f, a[3].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "vignette", {"float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).vignette(a[0].f, a[1].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "tilt_shift", {"float", "float", "float"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).tilt_shift(a[0].f, a[1].f, a[2].f);
          return Value::object("Image", p);
        });
  check(r, "Image", "blend", {"Image", "string", "float", "int", "int"},
        "Image", [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).blend(image(a[0].obj), a[1].s, a[2].f, integer(a[3]),
                         integer(a[4]));
          return Value::object("Image", p);
        });
  check(
      r, "Image", "rect",
      {"float", "float", "float", "float", "float", "float", "float", "float"},
      "Image", [&h](void *p, Args &a) -> Value {
        (void)h;
        (void)p;
        (void)a;
        image(p).rect(a[0].f, a[1].f, a[2].f, a[3].f, color(a, 4));
        return Value::object("Image", p);
      });
  check(r, "Image", "circle",
        {"float", "float", "float", "float", "float", "float", "float"},
        "Image", [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).circle(a[0].f, a[1].f, a[2].f, color(a, 3));
          return Value::object("Image", p);
        });
  check(r, "Image", "line",
        {"float", "float", "float", "float", "float", "float", "float", "float",
         "float"},
        "Image", [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).line(a[0].f, a[1].f, a[2].f, a[3].f, color(a, 4), a[8].f);
          return Value::object("Image", p);
        });
  check(
      r, "Image", "text",
      {"float", "float", "string", "float", "float", "float", "float", "float"},
      "Image", [&h](void *p, Args &a) -> Value {
        (void)h;
        (void)p;
        (void)a;
        image(p).text(a[0].f, a[1].f, a[2].s, a[3].f, color(a, 4));
        return Value::object("Image", p);
      });
  check(r, "Image", "metadata", {"string", "string"}, "Image",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          image(p).metadata(a[0].s, a[1].s);
          return Value::object("Image", p);
        });
  check(r, "Image", "mean", {"int"}, "float", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return Value::real(channel(image(p).stats().mean, a[0]));
  });
  check(
      r, "Image", "stddev", {"int"}, "float", [&h](void *p, Args &a) -> Value {
        (void)h;
        (void)p;
        (void)a;
        return Value::real(channel(image(p).stats().standard_deviation, a[0]));
      });
  check(r, "Image", "min", {"int"}, "float", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return Value::real(channel(image(p).stats().minimum, a[0]));
  });
  check(r, "Image", "max", {"int"}, "float", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return Value::real(channel(image(p).stats().maximum, a[0]));
  });
  check(r, "Image", "ssim", {"Image"}, "float",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return Value::real(image(p).ssim(image(a[0].obj)));
        });
  check(r, "Image", "psnr", {"Image"}, "float",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          return Value::real(image(p).psnr(image(a[0].obj)));
        });
  check(r, "Image", "histogram", {"string"}, "void",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          output(a[0].s, image(p).histogram_csv());
          return Value::none();
        });
  check(r, "Image", "save", {"string"}, "void",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          std::filesystem::path dest(a[0].s);
          if (dest.has_parent_path())
            std::filesystem::create_directories(dest.parent_path());
          image(p).save(a[0].s);
          return Value::none();
        });
  check(r, "Animation", "add", {"Image", "int"}, "Animation",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          animation(p).add(image(a[0].obj), integer(a[1], 0, 65535));
          return Value::object("Animation", p);
        });
  check(r, "Animation", "save", {"string"}, "void",
        [&h](void *p, Args &a) -> Value {
          (void)h;
          (void)p;
          (void)a;
          std::filesystem::path dest(a[0].s);
          if (dest.has_parent_path())
            std::filesystem::create_directories(dest.parent_path());
          animation(p).save(a[0].s);
          return Value::none();
        });
  check(r, "Animation", "size", {}, "int", [&h](void *p, Args &a) -> Value {
    (void)h;
    (void)p;
    (void)a;
    return Value::integer(animation(p).size());
  });
}
const char *usage =
    "usage: lmimg --version | --notices | --dsl-schema [--json] | "
    "--dsl-signatures | [--dry-run] --dsl FILE [FILE ...]\nResource limit: "
    "LMIMG_MAX_MEMORY_MB (positive whole MiB; default 4096).\n";
void diagnostic(const char *text) {
  std::cerr << "lmimg: ";
  for (const char *p = text; *p; ++p)
    std::cerr << ((*p == '\r' || *p == '\n') ? ' ' : *p);
  std::cerr << '\n';
}
int error(const char *phase, const dslvm::Error &e) {
  diagnostic(e.what());
  return dslvm::report_error(phase, e);
}
int execute(int argc, char **argv) {
  Host h;
  dslvm::Registry reg;
  register_host(reg, h);
  if (argc == 2 && std::string(argv[1]) == "--version") {
    std::cout << "lmimg " LMIMG_VERSION "\n";
    return 0;
  }
  if (argc == 2 && std::string(argv[1]) == "--notices") {
    std::cout.write(reinterpret_cast<const char *>(lmimg::embedded_notices),
                    lmimg::embedded_notices_size);
    return 0;
  }
  if (argc >= 2 && std::string(argv[1]) == "--dsl-schema") {
    if (argc == 3 && std::string(argv[2]) == "--json") {
      std::cout.write(reinterpret_cast<const char *>(lmimg::embedded_schema),
                      lmimg::embedded_schema_size);
      return 0;
    }
    if (argc == 2) {
      for (auto &s : reg.signatures())
        std::cout << s << '\n';
      return 0;
    }
    std::cerr << usage;
    return 2;
  }
  if (argc == 2 && std::string(argv[1]) == "--dsl-signatures") {
    for (auto &s : reg.signatures())
      std::cout << s << '\n';
    return 0;
  }
  bool dry = false;
  int at = 1;
  if (argc > at && std::string(argv[at]) == "--dry-run") {
    dry = true;
    ++at;
  }
  if (argc <= at || std::string(argv[at]) != "--dsl") {
    std::cerr << usage;
    return 2;
  }
  std::vector<std::string> files;
  for (int i = at + 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--dry-run" && !dry)
      dry = true;
    else if (arg.empty() || arg.rfind("--", 0) == 0) {
      std::cerr << usage;
      return 2;
    } else
      files.push_back(arg);
  }
  if (files.empty()) {
    std::cerr << usage;
    return 2;
  }
  std::string schema(reinterpret_cast<const char *>(lmimg::embedded_schema),
                     lmimg::embedded_schema_size);
  std::vector<dslvm::Program> programs;
  for (auto &path : files) {
    try {
      programs.push_back(
          dslvm::Program::load(dslvm::read_file(path), schema, reg));
    } catch (const dslvm::Error &e) {
      return error("load", e);
    }
  }
  if (dry) {
    dslvm::report_ok();
    return 0;
  }
  dslvm::Context ctx;
  int img = 0;
  ctx.set("img", &img);
  for (auto &program : programs) {
    try {
      program.run(ctx, reg);
    } catch (const dslvm::Error &e) {
      return error("run", e);
    }
    dslvm::report_ok();
  }
  return 0;
}
} // namespace
int main(int argc, char **argv) {
#ifdef _WIN32
  _setmode(_fileno(stdout), _O_BINARY);
  _setmode(_fileno(stderr), _O_BINARY);
#endif
  const char *env = std::getenv("LMIMG_MAX_MEMORY_MB");
  if (env) {
    bool valid = *env;
    for (const char *p = env; *p; ++p)
      if (*p < '0' || *p > '9')
        valid = false;
    if (!valid) {
      std::cerr << "lmimg: LMIMG_MAX_MEMORY_MB must be a positive whole number "
                   "of MiB\n";
      return 2;
    }
  }
  if (!membudget::configure("lmimg", "LMIMG_MAX_MEMORY_MB"))
    return 2;
  try {
    return execute(argc, argv);
  } catch (const membudget::Exceeded &e) {
    return dslvm::report_error("run", dslvm::Error(e.what()));
  } catch (const std::exception &e) {
    return error("run", dslvm::Error(e.what()));
  }
}
