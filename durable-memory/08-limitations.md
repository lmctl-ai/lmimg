# Scope and limitations

- Q8 storage quantizes each channel to 256 values; this release is not a high-dynamic-range or color-managed photographic pipeline.
- SSIM is the documented global luminance statistic, not multiscale or tiled SSIM.
- Healing-lite is a small local correction, not generative inpainting.
- Only the bundled raster formats and embedded font are supported. No external ImageMagick delegates, modules, PDF/PostScript/Ghostscript, GIMP/GEGL plug-ins, or system font search are part of the API.
- Objects remain in their host arena until execution ends; use the documented total memory budget for long scripts.
- No GUI, interactive shell, device capture, or project-file editor is included.

- Text currently accepts ASCII only. Raster `open` reads the first frame of animated input; GIF output is built explicitly with `img.animation()`.

- Built-in selections are rectangular. Use a procedural image mask for other silhouettes; there is no free-form selection editor.
- Image input must remain unchanged during `open`: header inspection and first-frame decoding are not an atomic snapshot. Dimensions are checked both before and after decoding, and the memory policy still applies.
