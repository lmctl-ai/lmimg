#include "image.hpp"
#include "expr.hpp"
#include "memory_budget.hpp"

#include <MagickWand/MagickWand.h>
#if __has_include("embedded_font.hpp")
#include "embedded_font.hpp"
#define LMIMG_HAS_EMBEDDED_FONT 1
#else
#define LMIMG_HAS_EMBEDDED_FONT 0
#endif
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>

namespace lmimg {
namespace {
std::once_flag genesis;
void init() { std::call_once(genesis, [] { MagickWandGenesis(); }); }

[[noreturn]] void fail(MagickWand* w, const char* what) {
  ExceptionType severity;
  char* detail = w ? MagickGetException(w, &severity) : nullptr;
  std::string message = what;
  if (detail && *detail) { message += ": "; message += detail; }
  if (detail) MagickRelinquishMemory(detail);
  throw std::runtime_error(message);
}
void ok(MagickBooleanType v, MagickWand* w, const char* what) {
  if (v == MagickFalse) fail(w, what);
}
void valid_size(std::size_t w, std::size_t h) {
  if (!w || !h || w > std::numeric_limits<std::size_t>::max() / h ||
      w * h > (std::size_t(1) << 31))
    throw std::invalid_argument("image dimensions are invalid or too large");
  membudget::require(w * h * 8);
}
double clip(double x) { return std::max(0.0, std::min(1.0, x)); }
PixelWand* color_wand(Color c) {
  PixelWand* p = NewPixelWand();
  PixelSetRed(p, clip(c.r) * QuantumRange); PixelSetGreen(p, clip(c.g) * QuantumRange);
  PixelSetBlue(p, clip(c.b) * QuantumRange); PixelSetAlpha(p, clip(c.a) * QuantumRange);
  return p;
}
void color_draw(DrawingWand* d, Color c) {
  PixelWand* p = color_wand(c); DrawSetFillColor(d, p); DestroyPixelWand(p);
}
std::size_t bytes_for(MagickWand* w) {
  return MagickGetImageWidth(w) * MagickGetImageHeight(w) * 8;
}
}

struct Image::Impl {
  MagickWand* wand = nullptr;
  std::size_t charged = 0;
  explicit Impl(MagickWand* p) : wand(p), charged(bytes_for(p)) {
    try { membudget::charge(charged); } catch (...) { DestroyMagickWand(wand); wand = nullptr; throw; }
  }
  ~Impl() { if (wand) DestroyMagickWand(wand); membudget::release(charged); }
  Impl(const Impl&) = delete;
};

Image::Image() : impl_() {}
Image::Image(std::unique_ptr<Impl> p) : impl_(std::move(p)) {}
Image::~Image() = default;
Image::Image(Image&&) noexcept = default;
Image& Image::operator=(Image&&) noexcept = default;
Image::Image(const Image& x) {
  if (x.impl_) { init(); MagickWand* p = CloneMagickWand(x.impl_->wand); if (!p) fail(x.impl_->wand, "clone"); impl_ = std::make_unique<Impl>(p); }
}
Image& Image::operator=(const Image& x) { if (this != &x) { Image t(x); *this = std::move(t); } return *this; }
void Image::require_image() const { if (!impl_) throw std::runtime_error("image is empty"); }

Image Image::create(std::size_t w, std::size_t h, Color fill) {
  init(); valid_size(w, h); MagickWand* p = NewMagickWand(); PixelWand* c = color_wand(fill);
  ok(MagickNewImage(p, w, h, c), p, "create image"); DestroyPixelWand(c);
  return Image(std::make_unique<Impl>(p));
}
Image Image::open(const std::string& path) {
  init(); if (path.empty()) throw std::invalid_argument("image path is empty");
  MagickWand* p = NewMagickWand(); ok(MagickReadImage(p, path.c_str()), p, "read image");
  valid_size(MagickGetImageWidth(p), MagickGetImageHeight(p)); return Image(std::make_unique<Impl>(p));
}
Image Image::clone() const { return Image(*this); }
void Image::save(const std::string& path) const { require_image(); if (path.empty()) throw std::invalid_argument("image path is empty"); ok(MagickWriteImage(impl_->wand, path.c_str()), impl_->wand, "write image"); }
void Image::save(const std::string& path, const std::string& format) const { require_image(); MagickWand* p = CloneMagickWand(impl_->wand); if (!p) fail(impl_->wand,"clone"); ok(MagickSetImageFormat(p, format.c_str()),p,"set format"); ok(MagickWriteImage(p,path.c_str()),p,"write image"); DestroyMagickWand(p); }
std::size_t Image::width() const { require_image(); return MagickGetImageWidth(impl_->wand); }
std::size_t Image::height() const { require_image(); return MagickGetImageHeight(impl_->wand); }

Color Image::pixel(std::size_t x, std::size_t y) const {
  require_image(); if (x >= width() || y >= height()) throw std::out_of_range("pixel coordinates");
  PixelWand* p = NewPixelWand(); ok(MagickGetImagePixelColor(impl_->wand,x,y,p),impl_->wand,"read pixel");
  Color c{PixelGetRed(p)/QuantumRange,PixelGetGreen(p)/QuantumRange,PixelGetBlue(p)/QuantumRange,PixelGetAlpha(p)/QuantumRange}; DestroyPixelWand(p); return c;
}
Image& Image::set_pixel(std::size_t x, std::size_t y, Color c) { if (x >= width() || y >= height()) throw std::out_of_range("pixel coordinates"); return mutate([&](MagickWand* p){ PixelWand* q=color_wand(c); ok(MagickSetImagePixelColor(p,x,y,q),p,"write pixel"); DestroyPixelWand(q); }); }

template<class F> Image& Image::mutate(F&& fn) {
  require_image(); MagickWand* p = CloneMagickWand(impl_->wand); if (!p) fail(impl_->wand,"clone image");
  try { fn(p); valid_size(MagickGetImageWidth(p),MagickGetImageHeight(p)); auto n=std::make_unique<Impl>(p); impl_.swap(n); }
  catch (...) { DestroyMagickWand(p); throw; }
  return *this;
}
Image& Image::resize(std::size_t w,std::size_t h){valid_size(w,h);return mutate([&](MagickWand*p){ok(MagickResizeImage(p,w,h,LanczosFilter,1.0),p,"resize");});}
Image& Image::crop(std::size_t x,std::size_t y,std::size_t w,std::size_t h){valid_size(w,h);if(x>width()||y>height()||w>width()-x||h>height()-y)throw std::out_of_range("crop rectangle");return mutate([&](MagickWand*p){ok(MagickCropImage(p,w,h,x,y),p,"crop");ok(MagickResetImagePage(p,"+0+0"),p,"reset crop page");});}
Image& Image::rotate(double d,Color bg){if(!std::isfinite(d))throw std::invalid_argument("rotation is not finite");return mutate([&](MagickWand*p){PixelWand*q=color_wand(bg);ok(MagickRotateImage(p,q,d),p,"rotate");DestroyPixelWand(q);});}
Image& Image::flip(bool horizontal){return mutate([&](MagickWand*p){ok(horizontal?MagickFlopImage(p):MagickFlipImage(p),p,"flip");});}
Image& Image::levels(double black,double gamma,double white){if(!(gamma>0)&&std::isfinite(gamma))throw std::invalid_argument("gamma must be positive");return mutate([&](MagickWand*p){ok(MagickLevelImage(p,black*QuantumRange,gamma,white*QuantumRange),p,"levels");});}
Image& Image::grayscale(){return mutate([&](MagickWand*p){ok(MagickSetImageColorspace(p,GRAYColorspace),p,"grayscale");});}
Image& Image::sepia(double t){return mutate([&](MagickWand*p){ok(MagickSepiaToneImage(p,t),p,"sepia");});}
Image& Image::blur(double r,double s){if(r<0||s<0)throw std::invalid_argument("blur radii must be nonnegative");return mutate([&](MagickWand*p){ok(MagickBlurImage(p,r,s),p,"blur");});}
Image& Image::sharpen(double r,double s){return mutate([&](MagickWand*p){ok(MagickSharpenImage(p,r,s),p,"sharpen");});}
Image& Image::edges(double r){return mutate([&](MagickWand*p){ok(MagickEdgeImage(p,r),p,"edges");});}
Image& Image::unsharp(double r,double s,double a,double t){return mutate([&](MagickWand*p){ok(MagickUnsharpMaskImage(p,r,s,a,t),p,"unsharp mask");});}
Image& Image::vignette(double r,double s){return mutate([&](MagickWand*p){ok(MagickVignetteImage(p,r,s,0,0),p,"vignette");});}
Image& Image::tilt_shift(double radius,double a,double b){if(a<0||b<a||b>1)throw std::invalid_argument("tilt shift focus range");return mutate([&](MagickWand*p){MagickWand*q=CloneMagickWand(p);ok(MagickBlurImage(q,radius,radius),q,"tilt shift blur");size_t h=MagickGetImageHeight(p),w=MagickGetImageWidth(p);std::vector<unsigned char> src(w*h*4),dst(w*h*4);ok(MagickExportImagePixels(p,0,0,w,h,"RGBA",CharPixel,src.data()),p,"tilt shift pixels");ok(MagickExportImagePixels(q,0,0,w,h,"RGBA",CharPixel,dst.data()),q,"tilt shift blurred pixels");for(size_t y=0;y<h;y++){double z=double(y)/(h?h-1:1);if(z>=a&&z<=b)for(size_t x=0;x<w;x++)for(int k=0;k<4;k++)dst[(y*w+x)*4+k]=src[(y*w+x)*4+k];}ok(MagickImportImagePixels(p,0,0,w,h,"RGBA",CharPixel,dst.data()),p,"tilt shift pixels");DestroyMagickWand(q);});}

Image& Image::blend(const Image& o,const std::string& mode,double opacity,std::size_t x,std::size_t y){o.require_image();if(opacity<0||opacity>1)throw std::invalid_argument("opacity must be in [0,1]");CompositeOperator op=OverCompositeOp;if(mode=="multiply")op=MultiplyCompositeOp;else if(mode=="screen")op=ScreenCompositeOp;else if(mode!="over")throw std::invalid_argument("unsupported blend mode");return mutate([&](MagickWand*p){MagickWand*q=CloneMagickWand(o.impl_->wand);ok(MagickEvaluateImage(q,MultiplyEvaluateOperator,opacity),q,"set blend opacity");ok(MagickCompositeImage(p,q,op,x,y),p,"blend");DestroyMagickWand(q);});}
Image& Image::rect(double x,double y,double w,double h,Color fill,Color stroke,double sw){return mutate([&](MagickWand*p){DrawingWand*d=NewDrawingWand();color_draw(d,fill);PixelWand*q=color_wand(stroke);DrawSetStrokeColor(d,q);DrawSetStrokeWidth(d,sw);DrawRectangle(d,x,y,x+w,y+h);ok(MagickDrawImage(p,d),p,"draw rectangle");DestroyPixelWand(q);DestroyDrawingWand(d);});}
Image& Image::circle(double x,double y,double r,Color fill,Color stroke,double sw){return mutate([&](MagickWand*p){DrawingWand*d=NewDrawingWand();color_draw(d,fill);PixelWand*q=color_wand(stroke);DrawSetStrokeColor(d,q);DrawSetStrokeWidth(d,sw);DrawCircle(d,x,y,x+r,y);ok(MagickDrawImage(p,d),p,"draw circle");DestroyPixelWand(q);DestroyDrawingWand(d);});}
Image& Image::line(double x1,double y1,double x2,double y2,Color c,double sw){return mutate([&](MagickWand*p){DrawingWand*d=NewDrawingWand();color_draw(d,c);DrawSetStrokeWidth(d,sw);DrawLine(d,x1,y1,x2,y2);ok(MagickDrawImage(p,d),p,"draw line");DestroyDrawingWand(d);});}
Image& Image::text(double x,double y,const std::string& value,double size,Color c,const std::string& font){if(size<=0||!std::isfinite(size))throw std::invalid_argument("text size");return mutate([&](MagickWand*p){DrawingWand*d=NewDrawingWand();
#if LMIMG_HAS_EMBEDDED_FONT
    if(font=="embedded") { DrawSetFont(d, lmimg_embedded_font_name()); }
    else if(!font.empty()) DrawSetFont(d,font.c_str());
#else
    if(!font.empty() && font!="embedded") DrawSetFont(d,font.c_str());
#endif
    DrawSetFontSize(d,size);color_draw(d,c);DrawAnnotation(d,x,y,(const unsigned char*)value.c_str());ok(MagickDrawImage(p,d),p,"draw text");DestroyDrawingWand(d);});}
Image& Image::metadata(const std::string& k,const std::string& v){return mutate([&](MagickWand*p){ok(MagickSetImageProperty(p,k.c_str(),v.c_str()),p,"set metadata");});}

Stats Image::stats() const {require_image();Stats s; s.width=width();s.height=height();s.pixels=s.width*s.height;std::vector<double> px(s.pixels*4);ok(MagickExportImagePixels(impl_->wand,0,0,s.width,s.height,"RGBA",DoublePixel,px.data()),impl_->wand,"export pixels");double sum[4]={},sq[4]={},lo[4]={1,1,1,1},hi[4]={};for(size_t i=0;i<s.pixels;i++)for(int k=0;k<4;k++){double v=px[i*4+k];sum[k]+=v;sq[k]+=v*v;lo[k]=std::min(lo[k],v);hi[k]=std::max(hi[k],v);}double*n[]={&s.mean.r,&s.mean.g,&s.mean.b,&s.mean.a};double*d[]={&s.standard_deviation.r,&s.standard_deviation.g,&s.standard_deviation.b,&s.standard_deviation.a};double*mn[]={&s.minimum.r,&s.minimum.g,&s.minimum.b,&s.minimum.a};double*mx[]={&s.maximum.r,&s.maximum.g,&s.maximum.b,&s.maximum.a};for(int k=0;k<4;k++){*n[k]=sum[k]/s.pixels;*d[k]=std::sqrt(std::max(0.,sq[k]/s.pixels-*n[k]* *n[k]));*mn[k]=lo[k];*mx[k]=hi[k];}return s;}
std::string Image::histogram_csv() const {require_image();std::vector<double> px(width()*height()*4);ok(MagickExportImagePixels(impl_->wand,0,0,width(),height(),"RGBA",DoublePixel,px.data()),impl_->wand,"export histogram pixels");std::size_t bins[256]={};for(size_t i=0;i<width()*height();i++){int v=int(std::lround(255*clip((px[i*4]+px[i*4+1]+px[i*4+2])/3)));bins[v]++;}std::ostringstream out;out<<"bin,count\n";for(int i=0;i<256;i++)out<<i<<","<<bins[i]<<"\n";return out.str();}
double Image::psnr(const Image&o)const{require_image();o.require_image();if(width()!=o.width()||height()!=o.height())throw std::invalid_argument("PSNR dimensions differ");std::vector<double>a(width()*height()*4),b(a.size());ok(MagickExportImagePixels(impl_->wand,0,0,width(),height(),"RGBA",DoublePixel,a.data()),impl_->wand,"export PSNR");ok(MagickExportImagePixels(o.impl_->wand,0,0,width(),height(),"RGBA",DoublePixel,b.data()),o.impl_->wand,"export PSNR");double e=0;for(size_t i=0;i<a.size();i++){double d=a[i]-b[i];e+=d*d;}e/=a.size();return e==0?std::numeric_limits<double>::infinity():10*std::log10(1/e);}
double Image::ssim(const Image&o)const{require_image();o.require_image();if(width()!=o.width()||height()!=o.height())throw std::invalid_argument("SSIM dimensions differ");auto a=stats(),b=o.stats();double c1=.0001,c2=.0009;double m1=(a.mean.r+a.mean.g+a.mean.b)/3,m2=(b.mean.r+b.mean.g+b.mean.b)/3;double v1=(a.standard_deviation.r*a.standard_deviation.r+a.standard_deviation.g*a.standard_deviation.g+a.standard_deviation.b*a.standard_deviation.b)/3,v2=(b.standard_deviation.r*b.standard_deviation.r+b.standard_deviation.g*b.standard_deviation.g+b.standard_deviation.b*b.standard_deviation.b)/3;return ((2*m1*m2+c1)*(2*std::sqrt(v1*v2)+c2))/((m1*m1+m2*m2+c1)*(v1+v2+c2));}

Image Image::pattern(std::size_t w,std::size_t h,const std::string&r,const std::string&g,const std::string&b,const std::string&a){Image out=create(w,h);Expr er(r,{"x","y"}),eg(g,{"x","y"}),eb(b,{"x","y"}),ea(a,{"x","y"});for(size_t y=0;y<h;y++)for(size_t x=0;x<w;x++){double v[2]={w>1?double(x)/(w-1):0,h>1?double(y)/(h-1):0};out.set_pixel(x,y,{clip(er.eval(v)),clip(eg.eval(v)),clip(eb.eval(v)),clip(ea.eval(v))});}return out;}

Image montage(const std::vector<Image>& xs,std::size_t cols,std::size_t gap,Color bg){if(xs.empty()||!cols)throw std::invalid_argument("montage requires images and columns");size_t rows=(xs.size()+cols-1)/cols,mw=0,mh=0;for(auto&x:xs){mw=std::max(mw,x.width());mh=std::max(mh,x.height());}Image out=Image::create(cols*mw+(cols+1)*gap,rows*mh+(rows+1)*gap,bg);for(size_t i=0;i<xs.size();i++)out.blend(xs[i],"over",1,gap+(i%cols)*(mw+gap),gap+(i/cols)*(mh+gap));return out;}
Animation& Animation::add(const Image&f,std::size_t d){if(d>65535)throw std::invalid_argument("GIF delay too large");frames_.push_back({f.clone(),d});return *this;}
void Animation::save(const std::string&path)const{if(frames_.empty())throw std::runtime_error("empty animation");init();MagickWand*w=NewMagickWand();for(auto&f:frames_){MagickWand*p=CloneMagickWand(f.image.impl_.get()->wand);MagickSetImageDelay(p,f.delay);MagickAddImage(w,p);DestroyMagickWand(p);}ok(MagickWriteImages(w,MagickTrue,path.c_str()),w,"write animation");DestroyMagickWand(w);}
} // namespace lmimg
