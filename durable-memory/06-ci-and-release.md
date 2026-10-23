# CI and release

The workflow builds Linux x64 and arm64, macOS arm64 and x64, and Windows x64.
The compiler is npm `@lmctl-ai/lmctl@0.2.369`. Each job runs the complete test.sh
suite and packages a raw executable plus LICENSE and THIRD_PARTY_NOTICES.

Main must pass all five jobs before tagging v0.1.0. The tag workflow repeats all
five builds, checks that the Linux binary reports exactly `lmimg 0.1.0`, and
publishes the five platform executables with the two license files.

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

Release evidence and fresh-directory public autorun URL results are recorded here
when the platform matrix and release verification finish.
