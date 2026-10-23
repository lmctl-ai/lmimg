# Libraries and licenses

lmimg uses ImageMagick 7.1.2-34 (commit
`a0c4bf729a26a5c2feb28b0365b47a8db736078a`) for its MagickCore and MagickWand
image operations.  The build is static, Q8, non-HDRI, disables OpenMP and
loadable modules, and disables XML, X11, Ghostscript-adjacent, LCMS, and
other optional delegates.  The ImageMagick License and attribution are
embedded in `THIRD_PARTY_NOTICES` and are printed by `lmimg --notices`.

PNG, JPEG, TIFF, and WebP support is enabled only when the pinned build finds
a compatible platform delegate.  A delegate's complete license and commit
must be added to `THIRD_PARTY_NOTICES` before enabling it for release.  No
GPL, LGPL, or AGPL code is part of the intended closure.
