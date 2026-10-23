# Purpose and boundaries

lmimg is an MIT scripting-only host for procedural images and batch raster editing. It binds ImageMagick's native C libraries in the same typed DSL format as lmnum/lmplot. The public repo is https://github.com/lmctl-ai/lmimg; intended initial release is v0.1.0.

The MEDIA_BRIEF license decision excludes GIMP and GEGL. Neither their code nor their libraries are used. Editing operations inspired by familiar raster workflows are original MIT implementations on normalized pixels. The implementation is deliberately smaller than a desktop editor: no GUI, plug-ins, project format, or arbitrary external delegates. All gallery inputs are procedural and generated in their scripts.

Programs own images through a host arena until the run ends, ensuring stable object handles even when DSL variables are reassigned. Edits return the same object; copying is explicit. PNG output and GIF animation need no runtime tool or data file. An embedded permissively licensed font supports text without a system font installation.
