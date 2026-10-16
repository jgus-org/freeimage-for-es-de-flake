# freeimage-for-es-de

A Nix flake that provides a minimal, self-authored reimplementation of the subset of the
Classic FreeImage 3.x public API used by [ES-DE](https://es-de.org) (EmulationStation
Desktop Edition). It is a standalone compatibility shim: it provides the `FreeImage.h`
interface, the `libfreeimage.so.3` soname, and the `freeimage` pkg-config module so that
ES-DE's FreeImage-based image loading compiles and links unchanged.

This project is not affiliated with, endorsed by, or derived from the FreeImage
project. The API surface reproduces classic FreeImage 3.18.0 names, spellings, and
constant values for source compatibility.

## Outputs

| Output | Contents |
| --- | --- |
| `packages.<system>.freeimage-for-es-de` | The library, its `FreeImage.h`, and the `freeimage` pkg-config module |
| `packages.<system>.default` | The same package |
| `checks.<system>.smoke`, `.link-all`, `.esde-surface` | Consumer translation units compiled against the *installed* package and run, so the shipped headers, module and soname are proven from the outside |
| `checks.<system>.unit` | The doctest suite, built from the same sources with its CMake option on |

## Consuming

```nix
{
  inputs.freeimage-for-es-de.url = "github:jgus-org/freeimage-for-es-de-flake";
}
```

```nix
{
  nativeBuildInputs = [ pkgs.pkg-config ];
  buildInputs = [ freeimage-for-es-de.packages.${system}.freeimage-for-es-de ];
}
```

Color order is fixed to BGR (`FREEIMAGE_COLORORDER=0`, the classic little-endian default)
through the installed module's `Cflags`. A consumer that links by hand instead of asking
pkg-config has to define that macro itself, or it will read the opposite byte order from
the one the DIB stores.

## What is implemented

The core engine: the bottom-up DIB model with its geometry, pixel-access and metadata
entry points, the 24/32-bit conversions and premultiplied alpha, raw-bits conversion in
both directions, `FreeImage_Rescale`, the memory streams, the format registry and its
load/save dispatch, and the multi-page container used for animated GIF playback.

Only five formats are registered: **BMP(0), JPEG(2), PNG(13), GIF(25), WEBP(35)**, each
keeping its classic enum value. TIFF, TARGA and the PNM family are absent in every
sense: no registry entry, no plugin, no detection, and their enum constants are removed
from the header. A file whose signature matches no registered plugin is reported as
`FIF_UNKNOWN` by `FreeImage_GetFileType`/`FreeImage_GetFileTypeFromMemory`, and loading
it through the registry fails with the documented failure value (`nullptr` / `FALSE`).
`FreeImage_GetFIFFromFilename` maps extensions over the same five formats only.

## Supported formats

| Format | fif | Read | Write |
| --- | --- | --- | --- |
| BMP | `FIF_BMP` | BI_RGB 1/4/8/16/24/32 bpp; BI_BITFIELDS at 16 bpp (565 or 555 from the file masks); V2–V5 headers accepted | BI_RGB 1/4/8/16/24/32 bpp, uncompressed (16 bpp written as BI_BITFIELDS) |
| JPEG | `FIF_JPEG` | colour ⇒ 24 bpp, single component ⇒ 8 bpp with a greyscale palette; CMYK converted to RGB; DCT scale-on-load | baseline or progressive, 24 bpp, 32 bpp (alpha dropped), 8 bpp grey |
| PNG | `FIF_PNG` | grey and palette images keep their 1/4/8 bpp indexed form (2-bit expanded, as classic); RGB ⇒ 24 bpp; RGBA, grey+alpha and RGB with `tRNS` ⇒ 32 bpp | colour type 0/2/3/6 chosen from the DIB, interlaced on request |
| GIF | `FIF_GIF` | raw frame as 8/4/1 bpp indexed with the frame palette and a per-index transparency table; `GIF_PLAYBACK` composites to a full-canvas 32 bpp page | single frame, 1/4/8 bpp indexed only |
| WebP | `FIF_WEBP` | 24 bpp, or 32 bpp when the bitstream carries alpha | lossy or lossless |

Beyond these, a plugin rejects the file the way classic rejects an unsupported input:
`FreeImage_Load` returns `nullptr` and the output-message sink reports why. RLE-compressed
and OS/2 BMP data, JPEG/PNG-in-BMP containers, bit fields at depths other than 16 bpp, and
GIF writes of a non-indexed DIB (no automatic quantization) all take that path.

### Load and save flags

- **JPEG** — load: `JPEG_ACCURATE` selects the accurate IDCT, `JPEG_GREYSCALE` forces a
  greyscale decode, and the top 16 bits carry a requested longest-side size, which maps to
  libjpeg's 1/2, 1/4 or 1/8 DCT scaling (`JPEG_FAST` idct method). Save: the low 7 bits are
  the quality value, the `JPEG_QUALITYSUPERB`…`JPEG_QUALITYBAD` presets name the classic
  quality points, and quality 75 applies when the low bits are zero; `JPEG_SUBSAMPLING_*`,
  `JPEG_PROGRESSIVE`, `JPEG_OPTIMIZE` and `JPEG_BASELINE` are honored. `JPEG_CMYK` is not
  implemented (CMYK sources are converted to RGB on load).
- **PNG** — load: a `gAMA` chunk applies classic's `png_set_gamma(screen 2.2, file gamma)`
  whenever it is present, unless `PNG_IGNOREGAMMA` is passed. Save: `PNG_INTERLACED` writes
  Adam7; the low 4 bits select the zlib compression level (1–9) and `PNG_Z_NO_COMPRESSION`
  writes level 0; `PNG_Z_BEST_SPEED` and `PNG_Z_DEFAULT_COMPRESSION` are the classic named
  levels. The written colour type follows the DIB: a grey ramp (with `tRNS` instead of an
  inverted mono pass when the DIB carries transparency), an indexed colour map, truecolor,
  or truecolor with alpha.
- **GIF** — load: `GIF_LOAD256` and, for the animation path, `GIF_PLAYBACK`, which returns a
  fully initialized logical canvas composited through the frames (disposal honored, the
  transparent index skipped, painted alpha 255) as 32 bpp. Both paths store the frame delay
  as the `FIMD_ANIMATION` `FrameTime` tag in milliseconds (`delay_cs * 10`), which is what
  ES-DE's animation component reads. Save: single frame only; `FrameTime` and
  `DisposalMethod` tags carry the delay and disposal method of the written frame.
- **WebP** — save: `WEBP_LOSSLESS` selects lossless; otherwise the low 7 bits are the quality
  (clamped to 100, 0 meaning the library default of 75). Encode method is 6.

Color order is fixed to BGR (`FREEIMAGE_COLORORDER=0`, the classic little-endian default)
via the installed pkg-config `Cflags`, so a consumer compiled against this package reads
the same byte order the DIB stores.

## Target API surface

- Formats ES-DE switches on, with classic 3.18.0 enum values: `FIF_UNKNOWN`,
  `FIF_BMP`, `FIF_JPEG`, `FIF_PNG`, `FIF_GIF`, `FIF_WEBP`.
- Loading via file name, `wchar_t` file name, memory streams, and multi-page animation
  playback through `FreeImageIO` callbacks (`FreeImage_OpenMultiBitmapFromHandle` and page
  locking) for animated GIFs.
- Conversion to 24/32-bit DIBs, raw-bits conversion in both directions, resampling
  (`FreeImage_Rescale`), premultiplied alpha, and animation metadata (`FIMD_ANIMATION` tags).
- Classic functions ES-DE never calls are dropped from the header rather than
  declared-and-stubbed; the inventory is in [`./API.md`](./API.md).

See [`./API.md`](./API.md) for the frozen public surface — every retained symbol's adopted
signature, the exact enum and flag values, the dropped-function inventory, and the
internal file-ownership map.

## Layout

- `src/include/FreeImage.h` — the public header, installed as `include/FreeImage.h`
- `src/internal.h` — the DIB model, plugin interface, and registry seam
- `src/*.cpp` — core, memory, format/registry, bitmap, multipage, metadata, resample
- `src/codecs/` — one translation unit per codec, each a `fi::Plugin` descriptor
- `tests/` — the doctest unit suite plus the standalone consumer TUs (`smoke.cpp`,
  `link_all.cpp`, `esde_surface.cpp`) compiled against the installed package

Every codec is driven through the caller's `FreeImageIO`, so file, memory and custom-stream
loading share one code path and nothing in the library touches stdio.

## Build

```sh
nix build .#freeimage-for-es-de
```

The CMake project is C++23 and finds its codec backends through pkg-config
(`libjpeg`, `libpng16`, `libwebp`, `zlib`; giflib has no pkg-config module and is linked
by name). The unit suite is opt-in in CMake (`FREEIMAGE_FOR_ES_DE_TESTS=ON`), so a package
build stays production-only.

## Tests

The doctest suite runs in `checks.unit`, which configures the same `CMakeLists.txt` with
the test option on and executes the test binary in its check phase — the suite passing is a
build-time requirement of that output, and its per-case summary is in the build log:

```sh
nix build .#checks.<system>.unit
nix log "$(nix build --no-link --print-out-paths .#checks.<system>.unit)"
```

To iterate inside CMake directly, configure with `-DFREEIMAGE_FOR_ES_DE_TESTS=ON` and run
the `check` target; the test sources are globbed, so adding a test file needs no CMake edit.

## License

MIT (see `LICENSE`), Copyright (c) 2026 Joshua Gustafson.
Reimplementing the FreeImage API surface (names, values, semantics) is intended to
stay clean-room; no FreeImage source is vendored here.

A consumer's binary links the codec backends it resolves through pkg-config — `zlib`,
`libjpeg-turbo`, `libpng`, `giflib` and `libwebp` — each carrying its own licence. There
is no libtiff, matching the absence of TIFF above.

