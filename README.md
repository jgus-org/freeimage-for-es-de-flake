# freeimage-for-es-de

A Nix flake that builds `freeimage-for-es-de`, a from-scratch implementation of the part of the [Classic FreeImage 3.18](https://freeimage.sourceforge.io) API that [ES-DE](https://es-de.org) uses. It presents the `FreeImage.h` interface, the `libfreeimage.so.3` soname and the `freeimage` pkg-config module, so FreeImage-based image loading compiles and links against it unchanged.

## Outputs

| Attribute                                                | What you get                                                                                                                         |
| -------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------ |
| `packages.default`                                       | `include/FreeImage.h`, `lib/libfreeimage.so.3`, and `lib/pkgconfig/freeimage.pc`.                                                    |
| `packages.freeimage-for-es-de`                           | The same library under its own name.                                                                                                 |
| `checks.smoke`, `checks.link-all`, `checks.esde-surface` | Standalone programs compiled and run against the _installed_ package, which is what qualifies the shipped header, module and soname. |
| `checks.unit`                                            | The internal unit suite.                                                                                                             |

Published for `x86_64-linux`, `aarch64-linux`, `x86_64-darwin`, and `aarch64-darwin`. `libfreeimage.so.3` is the ELF name for the shipped library; a Mach-O build of the same version carries the `.dylib` spelling of it instead, and the `freeimage` module resolves either way.

```nix
nativeBuildInputs = [ pkgs.pkg-config ];
buildInputs = [ freeimage-for-es-de ];   # then: pkg-config --cflags --libs freeimage
```

## What you get

- Five formats: BMP, JPEG, PNG, GIF and WebP, each keeping its classic `FIF_` value. TIFF, TARGA and the PNM family are absent in every sense — no registry entry, no detection, and no `FIF_TIFF`-style constant in the header, so code that names one does not compile. A file matching no registered format reports `FIF_UNKNOWN` and fails to load.
- Classic functions that ES-DE never calls are absent from the header rather than declared and stubbed; code that calls one fails to build. [`API.md`](API.md) is the frozen surface: every retained symbol's signature, the exact enum and flag values, and the dropped inventory.
- Pixel order is BGR, the classic little-endian default, and the module's `Cflags` carry `-DFREEIMAGE_COLORORDER=0`. Take them: the header falls back to BGR only when nothing else defines that macro, so a build that defines it otherwise reads red and green swapped against the stored pixels.
- Writing is narrower than reading: GIF output is a single frame of 1/4/8-bit indexed data with no automatic quantization, so a non-indexed bitmap is rejected. RLE-compressed and OS/2 BMP data, JPEG or PNG carried inside a BMP, and bit fields at depths other than 16 bpp take the same rejection path classic uses, with the reason reported to the message sink.
- Animated GIF playback works through the classic multi-page `FreeImageIO` path, and each frame's delay is exposed as the `FIMD_ANIMATION` `FrameTime` tag in milliseconds.
- JPEG scale-on-load, PNG gamma and interlacing, and the GIF and WebP save options behave as the classic flags describe.
- MIT. It is not FreeImage and contains no FreeImage source.
