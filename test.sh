#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$HERE"
BUILD_DIR="${BUILD_DIR:-build}"
HOST="$HERE/$BUILD_DIR/lmimg"
UNAME_S="$(uname -s)"
if [ -x /usr/bin/timeout ]; then TIMEOUT=/usr/bin/timeout
elif [ -x /usr/bin/timeout.exe ]; then TIMEOUT=/usr/bin/timeout.exe
else TIMEOUT="$(command -v gtimeout || command -v timeout)"; fi
case "$UNAME_S" in MINGW*|MSYS*|CYGWIN*) HOST="$HOST.exe";; esac
WORK="$(mktemp -d "${TMPDIR:-/tmp}/lmimg-test-XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
case "$UNAME_S" in MINGW*|MSYS*|CYGWIN*) WORK="$(cd "$WORK" && pwd -W | tr '\134' '/')";; esac
pass=0
ok(){ pass=$((pass+1)); echo "ok $pass - $1"; }
fail(){ echo "FAIL: $*" >&2; exit 1; }
sha(){ if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -c1-64; else shasum -a 256 "$1" | cut -c1-64; fi; }
mkdir -p "$BUILD_DIR"
PATH="$(cd "$BUILD_DIR" && pwd):$PATH"; export PATH
LMSCRIPT="$(scripts/select-compiler.sh)"
scripts/sync-sdk.sh --verify >/dev/null
ok "SDK matches its pinned bytes"
scripts/sync-deps.sh --verify >/dev/null
ok "dependencies match every full commit pin"
GEN=""; if [ ! -f "$BUILD_DIR/CMakeCache.txt" ] && command -v ninja >/dev/null 2>&1; then GEN="-G Ninja"; fi
# shellcheck disable=SC2086
"$TIMEOUT" 1800 cmake -S . -B "$BUILD_DIR" $GEN -DCMAKE_BUILD_TYPE=Release >"$WORK/configure.log" 2>&1 || {
 tail -70 "$WORK/configure.log"
 if [ -f "$BUILD_DIR/imagemagick-build/config.log" ]; then
  grep -B45 -A5 'C compiler cannot create executables' "$BUILD_DIR/imagemagick-build/config.log" || true
 fi
 fail configure
}
"$TIMEOUT" 600 cmake --build "$BUILD_DIR" --parallel "${BUILD_JOBS:-4}" >"$WORK/build.log" 2>&1 || { cp "$WORK/build.log" "$BUILD_DIR/lmimg-build.log"; tail -70 "$WORK/build.log"; fail build; }
ok "static release build"
ctest --test-dir "$BUILD_DIR" --output-on-failure
ok "native pixel, codec and image mathematics oracles"
[ "$("$HOST" --version)" = 'lmimg 0.1.1' ] || fail version
ok "exact version"
cp schema/lmimg.dsl.json "$WORK/schema"
scripts/gen-schema.py >/dev/null
cmp "$WORK/schema" schema/lmimg.dsl.json
"$HOST" --dsl-schema --json >"$WORK/embedded"
cmp "$WORK/embedded" schema/lmimg.dsl.json
diff -u <("$LMSCRIPT" --dsl-signatures schema/lmimg.dsl.json) <("$HOST" --dsl-signatures)
ok "generated schema, embedded bytes and compiler registry agree"
"$HOST" --notices >"$WORK/notices"
cmp "$WORK/notices" THIRD_PARTY_NOTICES
for name in ImageMagick libpng zlib libjpeg libtiff libwebp cpp-dsl-sdk; do grep -qi "$name" "$WORK/notices" || fail "missing notice $name"; done
ok "embedded notices equal complete license inventory"
case "$UNAME_S" in
 Linux) if ldd "$HOST" >/dev/null 2>&1; then fail 'dynamic Linux executable'; fi;;
 Darwin) bad="$(otool -L "$HOST" | tail -n +2 | grep -v -E '/usr/lib/(libSystem|libc\+\+|libresolv|libiconv)' || true)"; [ -z "$bad" ] || fail "non-system dylibs: $bad";;
 MINGW*|MSYS*|CYGWIN*) bad="$(objdump -p "$HOST" | grep 'DLL Name' | grep -i -E 'libstdc|libgcc|winpthread|libgomp|libpng|jpeg|tiff|webp|sharpyuv|zlib|libz[.-]|magick|msys-|cygwin' || true)"; [ -z "$bad" ] || fail "runtime DLLs: $bad";;
esac
ok "platform static linkage"
mkdir -p build images
rm -f images/*.png images/*.gif
n=0
for src in tests/*.lms examples/*.lms; do
 [ -f "$src" ] || continue
 name="$(basename "$src" .lms)"
 "$TIMEOUT" 300 "$LMSCRIPT" "$src" "build/$name.dslb" >"$WORK/$name.log" 2>&1 || { cat "$WORK/$name.log"; fail "$name compile/self execution"; }
 "$HOST" --dsl "build/$name.dslb" >"$WORK/$name.out" 2>"$WORK/$name.err" || { cat "$WORK/$name.out" "$WORK/$name.err"; fail "$name run"; }
 grep -q '"ok": true' "$WORK/$name.out" || fail "$name missing result"
 n=$((n+1))
done
ok "$n scripts compile, execute and assert expected answers"
for f in images/*.png images/*.gif; do [ -f "$f" ] && echo "$(sha "$f") $f"; done >"$WORK/before"
for src in examples/*.lms; do "$HOST" --dsl "build/$(basename "$src" .lms).dslb" >/dev/null; done
for f in images/*.png images/*.gif; do [ -f "$f" ] && echo "$(sha "$f") $f"; done >"$WORK/after"
cmp "$WORK/before" "$WORK/after"
ok "all procedural figures rerun byte-identically"
compile_snippet(){
 printf 'p = _dsl_ cli="lmimg";\n%s\nend _dsl_\nsavedsl(p, "%s/%s.dslb");\n' "$2" "$WORK" "$1" >"$WORK/$1.lms"
 "$LMSCRIPT" "$WORK/$1.lms" >"$WORK/$1.compile" 2>&1 || { cat "$WORK/$1.compile"; fail "$1 error test compile"; }
}
expect_error(){
 compile_snippet "$1" "$2"
 set +e; "$HOST" --dsl "$WORK/$1.dslb" >"$WORK/$1.out" 2>"$WORK/$1.err"; code=$?; set -e
 [ "$code" = 3 ] || fail "$1 exit $code instead of 3"
 grep -qi "$3" "$WORK/$1.out" || { cat "$WORK/$1.out"; fail "$1 JSON diagnostic"; }
 [ "$(wc -l <"$WORK/$1.err" | tr -d ' ')" = 1 ] || fail "$1 requires one stderr line"
}
expect_error dimensions 'let im=img.canvas(0,10,0.0,0.0,0.0,1.0);' 'range'
expect_error missing 'let im=img.open("missing-lmimg-file.png");' 'open\|read\|file'
expect_error pixel 'let im=img.canvas(2,2,0.0,0.0,0.0,1.0); im.pixel(2,0,0);' 'pixel\|range\|bound'
expect_error gamma 'let im=img.canvas(2,2,0.0,0.0,0.0,1.0); im.levels(0.0,0.0,1.0);' 'gamma\|level'
expect_error blend 'let im=img.canvas(2,2,0.0,0.0,0.0,1.0); im.blend(im,"imaginary",1.0,0,0);' 'blend'
expect_error mismatch 'let im=img.canvas(2,2,0.0,0.0,0.0,1.0); let b=img.canvas(3,2,0.0,0.0,0.0,1.0); im.ssim(b);' 'dimension'
expect_error expr 'let im=img.pattern(2,2,"unknown","0","0","1");' 'expression\|unknown'
expect_error assert 'img.assert_close(1.0,2.0,0.01);' 'assert_close'
expect_error curve 'let c=img.curve().point(0.0,0.0).point(0.0,1.0);' 'curve'
expect_error mask 'let a=img.canvas(2,2,1.0,1.0,1.0,1.0); let b=img.canvas(3,2,1.0,1.0,1.0,1.0); a.mask(b);' 'dimension'
expect_error perspective 'let a=img.canvas(2,2,1.0,1.0,1.0,1.0); a.perspective(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);' 'perspective'
expect_error format 'let im=img.open("not-supported.svg");' 'format'
expect_error opacity 'let a=img.canvas(2,2,1.0,1.0,1.0,1.0); a.blend(a,"over",2.0,0,0);' 'opacity'
expect_error nonfinite 'let a=img.pattern(2,2,"sqrt(-1)","0","0","1");' 'finite'
expect_error tilt_kernel 'let a=img.canvas(2,2,1.0,0.0,0.0,1.0); a.tilt_shift(img.eval("1e308"),0.25,0.75);' 'radius'
ok "fifteen error paths report exit 3, JSON and one stderr diagnostic"

# Independent-review repros stay explicit rather than relying only on gallery runs.
review_run(){
 compile_snippet "review_$1" "$2"
 "$TIMEOUT" 60 "$HOST" --dsl "$WORK/review_$1.dslb" >"$WORK/review_$1.out" 2>"$WORK/review_$1.err" || { cat "$WORK/review_$1.out" "$WORK/review_$1.err"; fail "review $1"; }
 grep -q '"ok": true' "$WORK/review_$1.out" || fail "review $1 result"
 ok "review $1: $3"
}
review_run M1 'let row=img.canvas(9,1,0.0,0.0,0.0,1.0); row.set_pixel(4,0,1.0,1.0,1.0,1.0).tilt_shift(2.0,0.0,1.0); img.assert_close(row.pixel(4,0,0),1.0,0.001); img.print("center="+str(row.pixel(4,0,0)));' 'one-row focus band preserves its white impulse'
review_run M2 'let a=img.canvas(1,1,0.2,0.4,0.6,1.0); let b=img.canvas(1,1,0.6,0.3,0.2,1.0); let c=a.copy().blend(b,"overlay",1.0,0,0); img.assert_close(c.pixel(0,0,0),0.24,0.01); let d=img.layers(1,1).add(a,"over",1.0,0,0).add(b,"difference",1.0,0,0).render(); img.assert_close(d.pixel(0,0,0),0.4,0.01);' 'overlay and layer difference match numerical answers'
review_run M3 'let a=img.canvas(3,2,1.0,0.0,0.0,1.0).selection(1,0,1,1); img.assert_close(a.pixel(1,0,3),1.0,0.001); img.assert_close(a.pixel(0,0,3),0.0,0.001); img.assert_close(a.pixel(1,1,3),0.0,0.001);' 'documented rectangle selection clips exactly its bounds'
review_run m1 'let a=img.canvas(1,1,1.0,1.0,1.0,1.0).morphology("median",16); img.assert_close(a.pixel(0,0,0),1.0,0.001);' 'morphology accepts the shared maximum radius 16'
expect_error review_m1_bound 'let a=img.canvas(1,1,1.0,1.0,1.0,1.0).morphology("median",17);' 'range'
printf 'P1\n1 1\n0\n' >"$WORK/input.pbm"
review_run m2 "let a=img.open(\"$WORK/input.pbm\"); img.assert_close(a.pixel(0,0,0),1.0,0.001); a.save(\"$WORK/output.pbm\");" 'PBM input and output are reachable'
review_run m3 "let a=img.canvas(1,1,1.0,0.0,0.0,1.0); a.save(\"$WORK/output.pgm\"); a.save(\"$WORK/output.ppm\"); let b=img.open(\"$WORK/output.pgm\"); img.assert_close(b.pixel(0,0,0),0.2126,0.01);" 'PGM converts color pixels to grayscale'
python3 - "$WORK" <<'PY'
from pathlib import Path
import sys
p=Path(sys.argv[1])
for name,magic in [('output.pbm',b'P4'),('output.pgm',b'P5'),('output.ppm',b'P6')]:
 assert (p/name).read_bytes().startswith(magic),(name,magic)
PY
review_run m4 'let a=img.canvas(96,40,0.0,0.0,0.0,0.0); let b=a.copy(); a.text(1.0,28.0,"Ink",24.0,2.0,-1.0,0.5,2.0); b.text(1.0,28.0,"Ink",24.0,1.0,0.0,0.5,1.0); img.assert_close(a.ssim(b),1.0,0.000001);' 'text clips ink and alpha before source-over compositing'
expect_error review_m5 'let a=img.open("missing-review-image.png");' 'cannot open image file'
if grep -q 'filesystem error' "$WORK/review_m5.out"; then fail 'raw filesystem wording'; fi
ok 'review m5: missing images have a concise host diagnostic'
review_run m6 "let a=img.canvas(1,1,1.0,0.0,0.0,1.0); let b=img.canvas(1,1,0.0,1.0,0.0,1.0); img.animation().add(a,8).add(b,8).save(\"$WORK/first-frame.gif\"); let first=img.open(\"$WORK/first-frame.gif\"); img.assert_close(first.pixel(0,0,0),1.0,0.001);" 'animated inputs retain the explicitly supported first frame'
python3 - <<'PY'
from pathlib import Path
notices=Path('THIRD_PARTY_NOTICES').read_text()
assert Path('third_party/libwebp/PATENTS').read_text() in notices
assert 'not compiled or linked' in notices
PY
ok 'review m7: full WebP patent grant and excluded-source disclosure are embedded'
python3 - <<'PY'
from pathlib import Path
s=Path('README.md').read_text()
for token in ['rectangular','heal','width()','height()','set_pixel','img.curve().point','Layers.masked','Layers.render','Montage.render','Animation.size','img.eval','img.assert_close','img.assert_true','img.print']:
 assert token in s,token
PY
ok 'review m9: README names selections and the omitted API/builders'
python3 tests/check-vm-operands.py "$HOST"
ok 'review m10: malformed while/range operand kinds return run errors'
python3 - <<'PY'
from pathlib import Path
s=Path('THIRD_PARTY_NOTICES').read_text()
assert Path('LICENSE').read_text() in s
assert 'Host source/build recipes: https://github.com/lmctl-ai/lmimg' in s
assert 'GNU LESSER GENERAL PUBLIC LICENSE' in s and 'GCC Runtime Library Exception' in s
PY
ok 'review m11: runtime terms and source/relink recipe pointers remain present'
if [ "${LMIMG_TEST_NETWORK:-0}" = 1 ]; then
 # Remove the build directory prepended above: this must use release installation.
 AUTORUN_PATH="${PATH#*:}"
 if PATH="$AUTORUN_PATH" command -v lmimg >/dev/null 2>&1; then fail 'public autorun must not find lmimg on PATH'; fi
 mkdir -p "$WORK/public-autorun"
 (cd "$WORK/public-autorun"; PATH="$AUTORUN_PATH" LMSCRIPT_TOOL_CACHE="$WORK/release-cache" "$TIMEOUT" 300 lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/gradient_studio.lms) >"$WORK/public-autorun.log" 2>&1 || { cat "$WORK/public-autorun.log"; fail 'public release autorun'; }
 python3 - "$WORK/public-autorun/images/gradient_studio.png" <<'PY'
from pathlib import Path
import struct,sys
b=Path(sys.argv[1]).read_bytes()
assert b[:8]==b'\x89PNG\r\n\x1a\n' and struct.unpack('>II',b[16:24])==(900,520)
PY
 ok 'reviews C1/m8: LMIMG_TEST_NETWORK gates a real empty-directory public autorun'
else
 echo 'skip public-release autorun: set LMIMG_TEST_NETWORK=1 to enable the network check'
fi
compile_snippet runaway 'for (i in range(0,1000000)) { let im=img.canvas(1024,1024,0.0,0.0,0.0,1.0); }'
set +e
LMIMG_MAX_MEMORY_MB=64 "$TIMEOUT" 90 "$HOST" --dsl "$WORK/runaway.dslb" >"$WORK/memory.out" 2>"$WORK/memory.err"; code=$?
set -e
[ "$code" = 3 ] || { cat "$WORK/memory.out" "$WORK/memory.err"; fail "64MiB runaway exited $code"; }
grep -q LMIMG_MAX_MEMORY_MB "$WORK/memory.err"
grep -q '"ok": false' "$WORK/memory.out"
[ "$(wc -l <"$WORK/memory.err" | tr -d ' ')" = 1 ] || fail 'duplicate memory diagnostics'
ok "64MiB runaway stops safely on this platform"
if [ "$UNAME_S" = Linux ]; then
 set +e
 (unset LMIMG_MAX_MEMORY_MB; ulimit -v 6291456; "$TIMEOUT" 120 "$HOST" --dsl "$WORK/runaway.dslb") >"$WORK/default.out" 2>"$WORK/default.err"; code=$?
 set -e
 [ "$code" = 3 ] || { cat "$WORK/default.out" "$WORK/default.err"; fail "default runaway exited $code"; }
 grep -q LMIMG_MAX_MEMORY_MB "$WORK/default.err"
 grep -q '"ok": false' "$WORK/default.out"
 ok "default 4GiB budget stops runaway below Linux 6GiB virtual limit"
fi
for value in '' 0 -1 1.5 abc ' 2' '2 ' +2 999999999999999999999999; do
 set +e; LMIMG_MAX_MEMORY_MB="$value" "$HOST" --version >"$WORK/env.out" 2>"$WORK/env.err"; code=$?; set -e
 [ "$code" = 2 ] || fail "invalid budget '$value' exit $code"
 grep -q LMIMG_MAX_MEMORY_MB "$WORK/env.err"
done
ok "malformed or overflowing memory budgets use exit 2"
for flags in '--unknown' '--version extra' '--dsl-schema --bogus' '--dry-run' '--dsl --bogus' '--dsl'; do
 set +e; "$HOST" $flags >/dev/null 2>&1; code=$?; set -e
 [ "$code" = 2 ] || fail "usage '$flags' exited $code"
done
set +e; "$HOST" --dsl "$WORK/missing.dslb" >"$WORK/load.out" 2>"$WORK/load.err"; code=$?; set -e
[ "$code" = 3 ] && [ -s "$WORK/load.err" ] || fail 'missing bytecode diagnostic'
ok "usage and load failures have documented exit codes"
rm -f images/gradient_studio.png
"$HOST" --dry-run --dsl build/gradient_studio.dslb >/dev/null
[ ! -f images/gradient_studio.png ] || fail 'dry run wrote output'
"$HOST" --dsl build/gradient_studio.dslb >/dev/null
ok "dry run loads without executing"
python3 scripts/check-gallery.py --generated
ok "gallery source, format, dimensions and byte budgets"
echo "all $pass checks passed"
