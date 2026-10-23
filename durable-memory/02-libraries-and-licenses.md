# Libraries and licenses

lmimg uses ImageMagick 7.1.2-34 (commit
`a0c4bf729a26a5c2feb28b0365b47a8db736078a`) for static Q8, non-HDRI
MagickCore and MagickWand. Configure disables modules, OpenMP, X11, system
font discovery, and the optional GPL or system delegates. `CMakeLists.txt`
reads ImageMagick's generated `config/config.h` and fails unless PNG, JPEG,
TIFF, WebP, and zlib were all detected. This prevents a successful build from
silently shipping a codec-less binary.

The codec pins are zlib `51b7f2abdade71cd9bb0e7a373ef2610ec6f9daf`, libpng
`ed217e3e601d8e462f7fd1e04bed43ac42212429`, libjpeg-turbo
`289df647c0f208dad7db63f24830361257812b1b`, libtiff
`8b20804fc0ddeaa93667b799b5e1a2a7dc9e3fb2`, and libwebp
`ca332209cb5567c9b249c86788cb2dbf8847e760`. stb_truetype is pinned at
`003501f6d7a8668ce8b6d875e34dd97ad0d5d5b5`; the embedded Roboto source is
Apache-2.0 at `38062f4b4a0be4346d07a928408da21602545e9e`.

`THIRD_PARTY_NOTICES` embeds complete upstream license texts and ImageMagick's
required NOTICE, the IJG README/license and attribution, the selected MIT option
for stb, Apache-2.0 for Roboto, the SDK's MIT terms, and platform runtime licenses.
The standard glibc/GCC/MinGW runtime terms are separate from the permissive
image-library closure. No GIMP/GEGL or copyleft image delegate is used.

Pins must be commit objects rather than annotated-tag objects: the original
codec tag SHAs checked out to different peeled HEAD commits and failed CI.
`sync-deps.sh` now checks object type, HEAD and clean source state.

Since 0.1.2 configure also passes `--without-pango --without-gvc --without-zip
--without-dmr --without-autotrace --without-dps --without-flif --without-fpx
--without-rsvg --without-uhdr` (these were auto-detected before and pango is
LGPL), and CMake fails if `config.h` defines any `*_DELEGATE` other than
PNG, JPEG, TIFF, WEBP, WEBPMUX and ZLIB (plus WINGDI32, the Windows GDI system library, on MinGW). An `nm` audit of the static archives
found no freetype, lcms, fontconfig, lzma, zstd, jbig, openjp2, bzip2,
ghostscript, libxml, fftw or OpenMP symbols; libtiff is built with jbig, lerc,
lzma, zstd and libdeflate off.
