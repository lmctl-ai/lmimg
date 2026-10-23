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

Release evidence and fresh-directory public autorun URL results are recorded here
when the platform matrix and release verification finish.
