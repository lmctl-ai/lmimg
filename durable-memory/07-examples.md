# Examples and gallery

The lmimg examples are self-contained procedural image stories. Each binds the
host with `_dsl_ cli="lmimg" install="github:lmctl-ai/lmimg@latest"`, saves its
bytecode, executes it, and writes `images/<name>.png` in the current directory.
This makes each raw GitHub example runnable from an empty directory with:

`lmctl autorun https://raw.githubusercontent.com/lmctl-ai/lmimg/main/examples/<name>.lms`

The checked-in `docs/images/` gallery is generated from those same programs.
Images are kept near 900 pixels wide and below 300 KiB; all content is
procedural and contains no copyrighted photographs. Gallery validation checks
PNG/GIF magic bytes, file-size limits, example links, self-executing output,
and byte-identical reruns for deterministic images. Native scientific tests
cover pixel geometry, color transforms, blur behavior, blend compositing,
statistics, histogram output, PSNR/SSIM, codec round trips, text bounds,
montage geometry, and GIF animation.
