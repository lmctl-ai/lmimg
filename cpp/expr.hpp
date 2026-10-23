// Tiny expression compiler: "sin(x)*exp(-k*t)+y^2" -> evaluable tree over named
// variables. Scripts cannot pass functions (the DSL has no closures), so every
// numeric routine that needs a function takes an expression string instead.
#pragma once
#include <cctype>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace lmimg {

class Expr {
public:
  // vars: names bound to slots 0..n-1 of the vector passed to eval(); params:
  // fixed named constants.
  Expr(const std::string &text, const std::vector<std::string> &vars,
       const std::map<std::string, double> &params = {},
       const std::map<std::string, double> &overrides = {})
      : src_(text), vars_(vars), params_(params), overrides_(overrides) {
    if (text.size() > 4096)
      throw std::invalid_argument("expression exceeds 4096 bytes");
    unsigned depth = 0;
    for (char c : text) {
      if (c == '(' && ++depth > 128)
        throw std::invalid_argument("expression nesting exceeds 128");
      if (c == ')' && depth)
        --depth;
    }
    pos_ = 0;
    root_ = parse_add();
    skip();
    if (pos_ != src_.size())
      fail("unexpected '" + std::string(1, src_[pos_]) + "'");
  }
  double eval(const double *v) const { return root_->eval(v); }

private:
  struct Node {
    virtual ~Node() = default;
    virtual double eval(const double *v) const = 0;
  };
  using P = std::unique_ptr<Node>;
  struct Num : Node {
    double x;
    explicit Num(double a) : x(a) {}
    double eval(const double *) const override { return x; }
  };
  struct Var : Node {
    int i;
    explicit Var(int a) : i(a) {}
    double eval(const double *v) const override { return v[i]; }
  };
  struct Un : Node {
    std::function<double(double)> f;
    P a;
    double eval(const double *v) const override { return f(a->eval(v)); }
  };
  struct Bin : Node {
    std::function<double(double, double)> f;
    P a, b;
    double eval(const double *v) const override {
      return f(a->eval(v), b->eval(v));
    }
  };

  std::string src_;
  std::vector<std::string> vars_;
  std::map<std::string, double> params_;
  std::map<std::string, double> overrides_;
  size_t pos_ = 0;
  P root_;

  [[noreturn]] void fail(const std::string &m) const {
    throw std::runtime_error("expression \"" + src_ + "\": " + m +
                             " at column " + std::to_string(pos_ + 1));
  }
  void skip() {
    while (pos_ < src_.size() && std::isspace((unsigned char)src_[pos_]))
      pos_++;
  }
  bool eat(char c) {
    skip();
    if (pos_ < src_.size() && src_[pos_] == c) {
      pos_++;
      return true;
    }
    return false;
  }
  static P bin(std::function<double(double, double)> f, P a, P b) {
    auto n = std::make_unique<Bin>();
    n->f = std::move(f);
    n->a = std::move(a);
    n->b = std::move(b);
    return n;
  }
  static P un(std::function<double(double)> f, P a) {
    auto n = std::make_unique<Un>();
    n->f = std::move(f);
    n->a = std::move(a);
    return n;
  }

  P parse_add() {
    P l = parse_mul();
    for (;;) {
      if (eat('+'))
        l = bin([](double a, double b) { return a + b; }, std::move(l),
                parse_mul());
      else if (eat('-'))
        l = bin([](double a, double b) { return a - b; }, std::move(l),
                parse_mul());
      else
        return l;
    }
  }
  P parse_mul() {
    P l = parse_unary();
    for (;;) {
      if (eat('*'))
        l = bin([](double a, double b) { return a * b; }, std::move(l),
                parse_unary());
      else if (eat('/'))
        l = bin([](double a, double b) { return a / b; }, std::move(l),
                parse_unary());
      else
        return l;
    }
  }
  P parse_unary() {
    if (eat('-'))
      return un([](double a) { return -a; }, parse_unary());
    if (eat('+'))
      return parse_unary();
    return parse_pow();
  }
  P parse_pow() {
    P base = parse_atom();
    if (eat('^') || (skip(), pos_ + 1 < src_.size() && src_[pos_] == '*' &&
                                 src_[pos_ + 1] == '*' && (pos_ += 2))) {
      P e = parse_unary(); // right associative, -x allowed in the exponent
      return bin([](double a, double b) { return std::pow(a, b); },
                 std::move(base), std::move(e));
    }
    return base;
  }
  P parse_atom() {
    skip();
    if (pos_ >= src_.size())
      fail("unexpected end");
    char c = src_[pos_];
    if (c == '(') {
      pos_++;
      P e = parse_add();
      if (!eat(')'))
        fail("expected ')'");
      return e;
    }
    if (std::isdigit((unsigned char)c) || c == '.') {
      size_t used = 0;
      double x = std::stod(src_.substr(pos_), &used);
      pos_ += used;
      return std::make_unique<Num>(x);
    }
    if (std::isalpha((unsigned char)c) || c == '_') {
      size_t s = pos_;
      while (pos_ < src_.size() &&
             (std::isalnum((unsigned char)src_[pos_]) || src_[pos_] == '_'))
        pos_++;
      std::string id = src_.substr(s, pos_ - s);
      skip();
      if (pos_ < src_.size() && src_[pos_] == '(') {
        pos_++;
        std::vector<P> args;
        if (!eat(')')) {
          do
            args.push_back(parse_add());
          while (eat(','));
          if (!eat(')'))
            fail("expected ')'");
        }
        return call(id, std::move(args));
      }
      auto ov = overrides_.find(id);
      if (ov != overrides_.end())
        return std::make_unique<Num>(ov->second);
      for (size_t i = 0; i < vars_.size(); i++)
        if (vars_[i] == id)
          return std::make_unique<Var>((int)i);
      auto it = params_.find(id);
      if (it != params_.end())
        return std::make_unique<Num>(it->second);
      if (id == "pi")
        return std::make_unique<Num>(3.14159265358979323846);
      if (id == "e")
        return std::make_unique<Num>(2.71828182845904523536);
      fail("unknown name '" + id + "'");
    }
    fail(std::string("unexpected '") + c + "'");
  }
  P call(const std::string &f, std::vector<P> a) {
    auto one = [&](double (*fn)(double)) -> P {
      if (a.size() != 1)
        fail(f + " takes 1 argument");
      return un(fn, std::move(a[0]));
    };
    auto two = [&](double (*fn)(double, double)) -> P {
      if (a.size() != 2)
        fail(f + " takes 2 arguments");
      return bin(fn, std::move(a[0]), std::move(a[1]));
    };
    if (f == "sin")
      return one(std::sin);
    if (f == "cos")
      return one(std::cos);
    if (f == "tan")
      return one(std::tan);
    if (f == "asin")
      return one(std::asin);
    if (f == "acos")
      return one(std::acos);
    if (f == "atan")
      return one(std::atan);
    if (f == "sinh")
      return one(std::sinh);
    if (f == "cosh")
      return one(std::cosh);
    if (f == "tanh")
      return one(std::tanh);
    if (f == "exp")
      return one(std::exp);
    if (f == "log" || f == "ln")
      return one(std::log);
    if (f == "log10")
      return one(std::log10);
    if (f == "sqrt")
      return one(std::sqrt);
    if (f == "abs")
      return one(std::fabs);
    if (f == "floor")
      return one(std::floor);
    if (f == "ceil")
      return one(std::ceil);
    if (f == "sign")
      return one([](double x) { return (double)((x > 0) - (x < 0)); });
    if (f == "atan2")
      return two(std::atan2);
    if (f == "pow")
      return two(std::pow);
    if (f == "min")
      return two([](double x, double y) { return x < y ? x : y; });
    if (f == "max")
      return two([](double x, double y) { return x > y ? x : y; });
    if (f == "mod")
      return two(std::fmod);
    fail("unknown function '" + f + "'");
  }
};

inline std::vector<std::string> split_names(const std::string &s) {
  std::vector<std::string> out;
  std::string cur;
  for (char c : s) {
    if (c == ',' || std::isspace((unsigned char)c)) {
      if (!cur.empty())
        out.push_back(cur), cur.clear();
    } else
      cur += c;
  }
  if (!cur.empty())
    out.push_back(cur);
  return out;
}

} // namespace lmimg
