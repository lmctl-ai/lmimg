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

`THIRD_PARTY_NOTICES` carries the attribution, license URLs, exact pins, and
the required no-warranty terms printed by `lmimg --notices`. The complete
upstream license texts are retained in each fetched source checkout and are
identified by path and hash there. No GPL, LGPL, or AGPL code is part of the
intended closure.
