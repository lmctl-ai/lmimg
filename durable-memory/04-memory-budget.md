# Total memory budget

`cpp/memory_budget.hpp` and `.cpp` were copied unchanged from the lmnum memory-budget worktree at commit b932657a4d67245b62f3b48180c50cd6d1d0eb90. The main lmnum checkout initially did not yet contain them. The shared implementation counts actual C++ allocator block sizes through replacement new/delete, with aligned and platform-specific block-size support.

The host configures `membudget` before building its registry or initializing ImageMagick. Default limit: 4096 MiB. `LMIMG_MAX_MEMORY_MB` must contain only ASCII digits and represent a positive non-overflowing whole number of MiB; the host validates syntax before calling the unchanged shared parser. Empty, signed, whitespace-padded, fractional and overflowing values exit 2, including query commands.

Persistent native image storage is charged conservatively and released with its owner. Known temporary storage is preflighted before C allocations. Disk/map cache spilling is disabled. Resource errors are not swallowed: the allocator prints one stderr line naming the variable, then the outer host catch returns JSON and exit 3 without a duplicate diagnostic. Other exceptions receive one sanitized stderr line and JSON.

`test.sh` compiles a retained-image runaway loop. It must fail with exit 3 at 64 MiB on every platform. On Linux a second test uses the default 4096 MiB under a 6144 MiB virtual-memory shell ceiling, proving the host limit triggers before that ceiling. The executable itself never calls setrlimit.

The unchanged source was rechecked against that Git object: `memory_budget.cpp` SHA-256 `999b2de7f12296e2d8c2737194ef391a1d6235dda20fdc893259e83e2abd46cc`, header SHA-256 `c3154f484d059ccbe3c6836f259279adb839b22fa0aaea4b8f26e7664945747f`.
