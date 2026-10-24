# CI and release

The workflow builds Linux x64 and arm64, macOS arm64 and x64, and Windows x64.
The compiler is npm `@lmctl-ai/lmctl@0.2.369`. Each job runs the complete test.sh
suite and packages a raw executable plus LICENSE and THIRD_PARTY_NOTICES.

Main must pass all five jobs before tagging a release. The tag workflow repeats
all five builds and checks the Linux binary version against the tag. v0.1.0
attached five executables and two license files; v0.1.1 attaches exactly five
executables, with complete license notices embedded in each binary.

Early CI findings: codec pins must be peeled commit objects rather than annotated
tag objects. ImageMagick's static WebP closure needs SharpYUV. Private pkg-config
searches must include both lib/pkgconfig and share/pkgconfig, where zlib installs
its .pc file. Missing PNG/TIFF delegate detection is a hard configuration error.
Windows Autoconf paths need POSIX conversion; a native Windows drive path is not
usable for discovering configure's auxiliary files under Bash.

Windows uses the runner's native MinGW compiler consistently for all source
compilation and the pinned MSYS2 setup action for POSIX build tools. Putting
MSYS2 usr/bin globally before Git Bash utilities mixes the two /tmp roots.
CMake selects one MSYS2 bash/make/coreutils environment only around ImageMagick
configure/build/install, explicitly passing the same native C/C++ compiler.
The resulting host must not import an MSYS2, codec, GCC, or C++ runtime DLL.

The Windows private pkg-config adapter reads the installed pinned codec metadata
and supplies the static closure. ImageMagick still performs real compile/link
probes; PNG/JPEG/TIFF/WebP/zlib support macros are hard requirements. The adapter
does not assert that a missing library exists.

ImageMagick's Windows blob-private header selects struct _stat64, while its
MinGW path leaves wstat mapped to a different struct stat ABI. CMake copies the
pinned source into the build tree and changes NTStatWide's call to _wstat64.
The modified file carries a prominent lmimg comment and the change is recorded
in THIRD_PARTY_NOTICES. The verified dependency checkout stays unchanged.
Do not replace the correction with a pointer-warning suppression.

A forced-include shim was rejected: MSYS rewrote a joined drive-path flag, and
a relative include then predeclared strchr during Autoconf's deliberately
undeclared-function test. The source correction leaves those real probes intact.
Failure-only CI artifacts preserve complete configuration and final-link logs.
Windows consumers define ImageMagick's `_LIB` macro; otherwise its headers infer
DLL imports from CRT macros even though the linked archives are static. Its
built-in Windows URL coder also references URLDownloadToFileA, so the static
closure links the system urlmon library. Runtime raster-format and policy
restrictions remain in force.

On macOS, PNG_SHARED=OFF does not disable libpng's separate framework target.
PNG_FRAMEWORK=OFF and CMAKE_FIND_FRAMEWORK=NEVER keep the static executable from
linking a build-tree png.framework.

## Release evidence — 2026-10-23

Public repository: https://github.com/lmctl-ai/lmimg. Released version:
[v0.1.0](https://github.com/lmctl-ai/lmimg/releases/tag/v0.1.0), tag commit
`783e5905bd49947e3e0e777267f48f5c3fb3d8eb`.

- [Main matrix](https://github.com/lmctl-ai/lmimg/actions/runs/40012020437): all five platforms green.
- [Tagged matrix and release](https://github.com/lmctl-ai/lmimg/actions/runs/40013609624): all five platforms and publication green.
- Linux runs all 17 test groups; macOS/Windows run 16 (the extra Linux group checks the default 4 GiB budget under a 6 GiB virtual-memory limit).
- The native oracle has 92 active checks, 10 DSL programs execute, and 15 error cases verify exit 3, JSON, and one stderr line.

All seven release assets were downloaded and checked. Both Linux executables
are static ELF files of the stated architecture; macOS assets are arm64/x64
Mach-O files; Windows is an x64 PE executable importing only Windows system
and UCRT DLLs. The Linux binary reports exactly `lmimg 0.1.0`. Its embedded
notices and both attached license files match the repository byte for byte.

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| LICENSE | 1076 | `949eb9b711d9b64ae472b87b94916e8427575a75d0d218f6c4cbccf8c48d46f3` |
| THIRD_PARTY_NOTICES | 146134 | `720e261348b821ec1cfeb780d9ee66e45550e2a5ed899a72fbc18200720d6758` |
| lmimg-linux-arm64 | 8942320 | `4b542a631361e4be9338e879d085bddc1aeb350a1558529a2b578d96e74d368e` |
| lmimg-linux-x64 | 10168048 | `27e09fbd4ae1bef202d87ba61b9f3d41d40a08d48e73f8c49ecb216bcad86abc` |
| lmimg-macos-arm64 | 7297928 | `db92d72e3bcfdfa3998957258ce07ec779bf97a59c70662fda32e448a2598e02` |
| lmimg-macos-x64 | 8183032 | `677ded27b570202c15217b77ee38877a44777fe9d701bfb6013cbd4b28d3c243` |
| lmimg-windows-x64.exe | 8729600 | `1f221cc9f911dc7eb0bc00467a868d589b6547213fb80ff9f688af49d0554a63` |

## Public autorun verification

Every command below was run with `/usr/bin/timeout 300` in a separate empty
temporary directory. Neither lmimg nor lmplot was on PATH. A fresh private
`LMSCRIPT_TOOL_CACHE` installed the released Linux asset; its cache metadata
records tag `v0.1.0` and SHA-256
`27e09fbd4ae1bef202d87ba61b9f3d41d40a08d48e73f8c49ecb216bcad86abc`,
which matches the separately downloaded release binary.

| Command | Seconds | PNG bytes |
| --- | ---: | ---: |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/gradient_studio.lms` | 3.17 | 4032 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blend_modes.lms` | 0.87 | 7298 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/levels_lab.lms` | 0.82 | 4272 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/sepia_poster.lms` | 0.93 | 23737 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blur_reveal.lms` | 0.85 | 21927 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/edge_map.lms` | 0.91 | 14178 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/thumbnail_grid.lms` | 0.78 | 7880 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/animation_wave.lms` | 1.12 | 11771 |

All eight commands returned 0 and generated their 900×520 PNG in `images/`.
The eight PNGs and the animation GIF matched the committed files byte for byte.
The GIF is 640×360 with eight frames, each 80 ms, and is 11,099 bytes.
All gallery assets remain below the PNG/GIF size budgets.

No open implementation or release items remain. The deliberate Q8, ASCII-text,
first-frame input, and global-SSIM limits are listed in note 08.


## v0.1.1 review release — 2026-10-23

Released [v0.1.1](https://github.com/lmctl-ai/lmimg/releases/tag/v0.1.1) from
`efc0b3f828ddabdda97721f5f9202434e33b8dca` after the independent review.

- [Main CI](https://github.com/lmctl-ai/lmimg/actions/runs/40017099712): all five platform jobs green.
- [Tagged CI and publication](https://github.com/lmctl-ai/lmimg/actions/runs/40019405293): all five platform jobs and release green.
- 107 native checks; 16 DSL programs (13 examples plus three regressions); 31 Linux test groups and 30 macOS/Windows groups with live network integration enabled.
- The reviewed one-row focus/blend/layer script and all four malformed loop bytecodes were also run against the downloaded released Linux executable.

Exactly five executable assets are attached. Complete host/dependency/runtime
terms, the WebP patent grant and source/build pointers are embedded in
`--notices`; downloaded Linux output matches THIRD_PARTY_NOTICES byte for byte.
Both Linux assets are static ELF files, macOS assets are matching Mach-O files,
and the Windows x64 PE imports only Windows/UCRT DLLs.

| Asset | Bytes | SHA-256 |
| --- | ---: | --- |
| lmimg-linux-arm64 | 8942320 | `69999876c1173b552f2abdfcbd5f6918693ff9b97361b69b07e4981d7039d0e9` |
| lmimg-linux-x64 | 10172144 | `234427c9e488ab1fac5892f82f52abd9c27de09ece2303adf1e2490f58ac19ec` |
| lmimg-macos-arm64 | 7298200 | `68098987824c84324aec08d1a24dfb71340a409d768a04c17850a459f1af89a5` |
| lmimg-macos-x64 | 8191504 | `1b02a8b8909ba872301dbb45a782885d6024fa7d7f425a821e501564a6117565` |
| lmimg-windows-x64.exe | 8732672 | `f406f4d9b5bfdf1d5dce4f1ce7a726a85b0d0619a63ba8b570afee92abfe7424` |

All thirteen raw example URLs below were run with timeout 300 in separate empty
directories, with neither lmimg nor lmplot on PATH and a freshly created private
cache. The installed metadata records `v0.1.1`, asset `lmimg-linux-x64`, SHA-256
`234427c9e488ab1fac5892f82f52abd9c27de09ece2303adf1e2490f58ac19ec`;
its bytes equal the independently downloaded release and `--version` reports
exactly `lmimg 0.1.1`.

| Autorun example | Seconds | PNG bytes |
| --- | ---: | ---: |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/gradient_studio.lms` | 3.37 | 4032 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blend_modes.lms` | 1.23 | 7298 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/levels_lab.lms` | 1.36 | 4272 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/sepia_poster.lms` | 1.23 | 23737 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/blur_reveal.lms` | 1.29 | 21927 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/edge_map.lms` | 1.36 | 14178 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/thumbnail_grid.lms` | 1.19 | 7880 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/animation_wave.lms` | 1.73 | 11771 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/procedural_photo.lms` | 1.71 | 114597 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/watermark_story.lms` | 1.60 | 34251 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/batch_resize.lms` | 1.36 | 29194 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/before_after_panels.lms` | 1.63 | 66165 |
| `lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/contact_sheet_story.lms` | 1.36 | 23949 |

All commands returned 0. All thirteen PNGs and the eight-frame, 80 ms/frame GIF
match the committed gallery byte for byte. The batch example additionally writes
180×240, 210×280 and 240×320 PNGs, verified from their file headers.

Review dispositions, including intentionally scoped rectangular selections and
stable-input requirements, are recorded in note 09. No implementation or release
work remains for this review follow-up.

## v0.1.3 Linux dynamic glibc rollout (Rule 14)

Starting in v0.1.3, Linux builds run on `ubuntu-22.04` and `ubuntu-22.04-arm`
(minimum dynamic glibc >= 2.35) and link with `-static-libstdc++ -static-libgcc`
while dynamically linking the system glibc (`libc.so.6`, `libm.so.6`,
`libpthread.so.0`, `libdl.so.2`, `librt.so.1`, `ld-linux*`) and statically
linking every third-party image and codec library. `test.sh` enforces the
`readelf -d` NEEDED allowlist on Linux.
