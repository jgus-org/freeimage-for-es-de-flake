# freeimage-for-es-de — Frozen Public API & Internal Ownership

The frozen public contract. This is the single source of truth for the public
symbols, their adopted signatures, and the internal file-ownership split.

Governing rule: where the classic FreeImage 3.18.0 header — `FreeImage/Source/FreeImage.h` in the [3.18.0 source distribution](https://sourceforge.net/projects/freeimage/files/Source%20Distribution/3.18.0/FreeImage3180.zip) — and this document disagree on **SIGNATURES**, the classic header wins; on **SEMANTICS**, this document wins. The shipped `src/include/FreeImage.h` reproduces classic signatures verbatim for every retained symbol and is trimmed to the surface in §1: classic functions ES-DE never calls are **dropped** rather than declared-and-stubbed, and §3 lists them.

A `classic 3.18.0` entry is the line that symbol occupies in that header, so a retained signature can be checked against the distribution without any of its source being vendored here.

Supported formats are **exactly** BMP(0), JPEG(2), PNG(13), GIF(25), WEBP(35).
TIFF, TARGA and the PNM family are entirely absent (no registry entry, no detection,
`FIFSupportsReading == FALSE`, enum constants removed). Kept ids keep classic numbers.

---

## 1. Public function surface (frozen — do not change signatures)

Every entry point is defined in `src/FreeImage.cpp`, the sole marshal layer (§4);
section headings below name the translation unit that holds the implementation each
entry delegates to.

### Init / version / messages — answered in `src/FreeImage.cpp` itself

| Symbol                              | Signature                                           | classic 3.18.0 |
| ----------------------------------- | --------------------------------------------------- | -------------- |
| `FreeImage_Initialise`              | `void (BOOL load_local_plugins_only = FALSE)`       | 790            |
| `FreeImage_DeInitialise`            | `void (void)`                                       |                |
| `FreeImage_GetVersion`              | `const char* (void)` — returns `"3.18.0"`           | 795            |
| `FreeImage_GetCopyrightMessage`     | `const char* (void)`                                | 796            |
| `FreeImage_SetOutputMessage`        | `void (FreeImage_OutputMessageFunction omf)`        |                |
| `FreeImage_SetOutputMessageStdCall` | `void (FreeImage_OutputMessageFunctionStdCall omf)` |                |
| `FreeImage_OutputMessageProc`       | `void (int fif, const char* fmt, ...)`              |                |

Self-init: the shared build needs no runtime init (the registry is a compile-time
table). `Initialise`/`DeInitialise` are harmless no-ops (ES-DE calls them only under
`FREEIMAGE_LIB`, which ES-DE never defines).

### Load / save / unload dispatch — `src/registry.cpp` + `src/io.cpp`

| Symbol                     | Signature                                                                                         | classic 3.18.0 |
| -------------------------- | ------------------------------------------------------------------------------------------------- | -------------- |
| `FreeImage_Unload`         | `void (FIBITMAP* dib)`                                                                            | 812            |
| `FreeImage_Load`           | `FIBITMAP* (FREE_IMAGE_FORMAT fif, const char* filename, int flags = 0)`                          |                |
| `FreeImage_LoadU`          | `FIBITMAP* (FREE_IMAGE_FORMAT, const wchar_t* filename, int flags = 0)` — POSIX narrow conversion |                |
| `FreeImage_LoadFromHandle` | `FIBITMAP* (FREE_IMAGE_FORMAT, FreeImageIO* io, fi_handle handle, int flags = 0)`                 | 821            |
| `FreeImage_Save`           | `BOOL (FREE_IMAGE_FORMAT, FIBITMAP* dib, const char* filename, int flags = 0)` — `w+b` truncates  |                |
| `FreeImage_SaveU`          | `BOOL (FREE_IMAGE_FORMAT, FIBITMAP*, const wchar_t* filename, int flags = 0)`                     |                |
| `FreeImage_SaveToHandle`   | `BOOL (FREE_IMAGE_FORMAT, FIBITMAP*, FreeImageIO*, fi_handle, int flags = 0)`                     | 824            |

### Memory I/O — `src/io.cpp`

| Symbol                     | Signature                                                                 | classic 3.18.0 |
| -------------------------- | ------------------------------------------------------------------------- | -------------- |
| `FreeImage_OpenMemory`     | `FIMEMORY* (BYTE* data = 0, DWORD size_in_bytes = 0)` — wraps, no copy    | 828            |
| `FreeImage_CloseMemory`    | `void (FIMEMORY* stream)`                                                 |                |
| `FreeImage_LoadFromMemory` | `FIBITMAP* (FREE_IMAGE_FORMAT, FIMEMORY* stream, int flags = 0)`          |                |
| `FreeImage_SaveToMemory`   | `BOOL (FREE_IMAGE_FORMAT, FIBITMAP*, FIMEMORY* stream, int flags = 0)`    |                |
| `FreeImage_TellMemory`     | `long (FIMEMORY* stream)`                                                 |                |
| `FreeImage_SeekMemory`     | `BOOL (FIMEMORY* stream, long offset, int origin)`                        |                |
| `FreeImage_AcquireMemory`  | `BOOL (FIMEMORY* stream, BYTE** data, DWORD* size_in_bytes)`              |                |
| `FreeImage_ReadMemory`     | `unsigned (void* buffer, unsigned size, unsigned count, FIMEMORY*)`       |                |
| `FreeImage_WriteMemory`    | `unsigned (const void* buffer, unsigned size, unsigned count, FIMEMORY*)` |                |

The `FreeImageIO` used by the `*FromMemory` entry points takes the `FIMEMORY*` itself
as its `fi_handle` (classic convention). `CloseMemory` never frees a wrapped
(caller-owned) buffer.

### File-type / format queries — `src/registry.cpp`

| Symbol                            | Signature                                                                                                        | classic 3.18.0 |
| --------------------------------- | ---------------------------------------------------------------------------------------------------------------- | -------------- |
| `FreeImage_GetFileType`           | `FREE_IMAGE_FORMAT (const char* filename, int size = 0)` — iterate registry ascending; not-found ⇒ `FIF_UNKNOWN` |                |
| `FreeImage_GetFileTypeU`          | `FREE_IMAGE_FORMAT (const wchar_t* filename, int size = 0)` — returns `FIF_UNKNOWN` on POSIX                     | 882            |
| `FreeImage_GetFileTypeFromMemory` | `FREE_IMAGE_FORMAT (FIMEMORY* stream, int size = 0)` — leaves stream at 0                                        |                |
| `FreeImage_GetFIFFromFilename`    | `FREE_IMAGE_FORMAT (const char* filename)` — extension table over the five                                       |                |
| `FreeImage_GetFIFFromFilenameU`   | `FREE_IMAGE_FORMAT (const wchar_t* filename)`                                                                    |                |
| `FreeImage_FIFSupportsReading`    | `BOOL (FREE_IMAGE_FORMAT fif)` — TRUE iff a registered plugin has `load`                                         |                |
| `FreeImage_FIFSupportsWriting`    | `BOOL (FREE_IMAGE_FORMAT fif)` — TRUE iff registered plugin has `save`                                           |                |

`validate` must leave the stream position at 0; `GetFileType*` seek back to 0 before
each validate and after the scan (ES-DE loads immediately after detection).

### Multi-page — `src/multipage.cpp`

| Symbol                                | Signature                                                                                                                                                       |
| ------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `FreeImage_OpenMultiBitmap`           | `FIMULTIBITMAP* (FREE_IMAGE_FORMAT, const char* filename, BOOL create_new, BOOL read_only, BOOL keep_cache_in_memory = FALSE, int flags = 0)` — may return NULL |
| `FreeImage_OpenMultiBitmapFromHandle` | `FIMULTIBITMAP* (FREE_IMAGE_FORMAT, FreeImageIO* io, fi_handle handle, int flags = 0)`                                                                          |
| `FreeImage_CloseMultiBitmap`          | `BOOL (FIMULTIBITMAP* bitmap, int flags = 0)`                                                                                                                   |
| `FreeImage_GetPageCount`              | `int (FIMULTIBITMAP* bitmap)`                                                                                                                                   |
| `FreeImage_LockPage`                  | `FIBITMAP* (FIMULTIBITMAP* bitmap, int page)` — re-decodes each lock, no cache                                                                                  |
| `FreeImage_UnlockPage`                | `void (FIMULTIBITMAP* bitmap, FIBITMAP* data, BOOL changed)`                                                                                                    |

Handle-opened multibitmaps never close the caller's `FILE*`. `GIF_PLAYBACK` hands back a composited full-canvas 32bpp page carrying that frame's delay as a `FIMD_ANIMATION` `FrameTime` tag.

### Bitmap info / pixels / bits — `src/dib.cpp` (accessors inline in `dib.h`), pixel ops in `src/convert.cpp`

| Symbol                    | Signature                                                           | classic 3.18.0 |
| ------------------------- | ------------------------------------------------------------------- | -------------- |
| `FreeImage_GetImageType`  | `FREE_IMAGE_TYPE (FIBITMAP* dib)`                                   | 894            |
| `FreeImage_GetBits`       | `BYTE* (FIBITMAP* dib)`                                             | 904            |
| `FreeImage_GetScanLine`   | `BYTE* (FIBITMAP* dib, int scanline)` — scanline 0 == visual BOTTOM | 905            |
| `FreeImage_GetPixelIndex` | `BOOL (FIBITMAP*, unsigned x, unsigned y, BYTE* value)`             |                |
| `FreeImage_GetPixelColor` | `BOOL (FIBITMAP*, unsigned x, unsigned y, RGBQUAD* value)`          |                |
| `FreeImage_SetPixelIndex` | `BOOL (FIBITMAP*, unsigned x, unsigned y, BYTE* value)`             |                |
| `FreeImage_SetPixelColor` | `BOOL (FIBITMAP*, unsigned x, unsigned y, RGBQUAD* value)`          |                |
| `FreeImage_GetBPP`        | `unsigned (FIBITMAP* dib)`                                          | 915            |
| `FreeImage_GetWidth`      | `unsigned (FIBITMAP* dib)`                                          | 916            |
| `FreeImage_GetHeight`     | `unsigned (FIBITMAP* dib)`                                          | 917            |
| `FreeImage_GetLine`       | `unsigned (FIBITMAP* dib)`                                          | 918            |
| `FreeImage_GetPitch`      | `unsigned (FIBITMAP* dib)`                                          | 919            |

All `FIBITMAP*` parameters are **non-const** (classic spelling); ES-DE passes
non-const pointers.

### Conversion / raw bits — `src/convert.cpp`

| Symbol                           | Signature                                                                                                                                                                             |
| -------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `FreeImage_ConvertTo24Bits`      | `FIBITMAP* (FIBITMAP* dib)` — alpha discarded                                                                                                                                         |
| `FreeImage_ConvertTo32Bits`      | `FIBITMAP* (FIBITMAP* dib)` — 32bpp ⇒ clone; indexed+transparency ⇒ alpha from table; else 0xFF                                                                                       |
| `FreeImage_ConvertFromRawBits`   | `FIBITMAP* (BYTE* bits, int width, int height, int pitch, unsigned bpp, unsigned r, unsigned g, unsigned b, BOOL topdown = FALSE)` — copies input                                     |
| `FreeImage_ConvertFromRawBitsEx` | `FIBITMAP* (BOOL copySource, BYTE* bits, FREE_IMAGE_TYPE, int w, int h, int pitch, unsigned bpp, unsigned r, unsigned g, unsigned b, BOOL topdown = FALSE)`                           |
| `FreeImage_ConvertToRawBits`     | `void (BYTE* bits, FIBITMAP* dib, int pitch, unsigned bpp, unsigned r, unsigned g, unsigned b, BOOL topdown = FALSE)` — masks ignored for 24/32→24/32; `topdown` flips row order only |

### Toolkit — `src/resample.cpp` / `src/convert.cpp`

| Symbol                           | Signature                                                                                                                     | classic 3.18.0 |
| -------------------------------- | ----------------------------------------------------------------------------------------------------------------------------- | -------------- |
| `FreeImage_Rescale`              | `FIBITMAP* (FIBITMAP* dib, int dst_width, int dst_height, FREE_IMAGE_FILTER filter = FILTER_CATMULLROM)` — `src/resample.cpp` | 1113           |
| `FreeImage_PreMultiplyWithAlpha` | `BOOL (FIBITMAP* dib)` — 32bpp FIT_BITMAP only, in place; `src/convert.cpp`                                                   |                |

### Metadata — tag storage in `src/dib.cpp`, lookups in `src/FreeImage.cpp`

| Symbol                  | Signature                                                                                                                           | classic 3.18.0 |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------- | -------------- |
| `FreeImage_GetTagCount` | `DWORD (FITAG* tag)`                                                                                                                | 1059           |
| `FreeImage_GetTagValue` | `const void* (FITAG* tag)`                                                                                                          |                |
| `FreeImage_GetMetadata` | `BOOL (FREE_IMAGE_MDMODEL model, FIBITMAP* dib, const char* key, FITAG** tag)` — no allocate-on-miss; returned tag owned by the dib | 1078           |

---

## 2. Types, macros and constants exposed by the header

The type/macro/enum surface tracks classic (it has no link cost and maximizes source compatibility); only the **format registry** and the **TIFF/TARGA/PNM enum constants** are trimmed.

- Scalar typedefs: `BOOL=int32_t`, `BYTE=uint8_t`, `WORD=uint16_t`, `DWORD=uint32_t`,
  `LONG=int32_t`, `INT64=int64_t`, `UINT64=uint64_t`. `FIBOOL=int32_t` is an additive
  alias; `BOOL` is what every declared function uses.
- Structs: `RGBQUAD` (BGR order: `rgbBlue, rgbGreen, rgbRed, rgbReserved`), `RGBTRIPLE`
  (`rgbtBlue, rgbtGreen, rgbtRed`), `BITMAPINFOHEADER`, `BITMAPINFO`; opaque
  `FIBITMAP`, `FIMULTIBITMAP`, `FIMEMORY`, `FIMETADATA`, `FITAG` (`{void* data;}`);
  `FreeImageIO` + `fi_handle` + `FI_ReadProc/WriteProc/SeekProc/TellProc`.
- Additive (do not change values): `typedef FREE_IMAGE_FILTER FREE_IMAGE_RESAMPLE;`
  and the `FreeImageResampleFilter*` symbolic aliases; `FREEIMAGE_VERSION "3.18.0"`.
- Build forces `FREEIMAGE_COLORORDER_BGR` on little-endian (classic default); the
  installed pkg-config passes `-DFREEIMAGE_COLORORDER=0`.

### Enum / constant table (exact numeric values)

`FREE_IMAGE_FORMAT`: `FIF_UNKNOWN=-1, FIF_BMP=0, FIF_JPEG=2, FIF_PNG=13, FIF_GIF=25, FIF_WEBP=35`.
(TIFF/TARGA/PNM/… removed.)

`FREE_IMAGE_TYPE`: `FIT_UNKNOWN=0, FIT_BITMAP=1, FIT_UINT16=2, FIT_INT16=3, FIT_UINT32=4,
FIT_INT32=5, FIT_FLOAT=6, FIT_DOUBLE=7, FIT_COMPLEX=8, FIT_RGB16=9, FIT_RGBA16=10,
FIT_RGBF=11, FIT_RGBAF=12`.

`FREE_IMAGE_COLOR_TYPE`: `FIC_MINISWHITE=0, FIC_MINISBLACK=1, FIC_RGB=2, FIC_PALETTE=3,
FIC_RGBALPHA=4, FIC_CMYK=5`.

`FREE_IMAGE_QUANTIZE`: `FIQ_WUQUANT=0, FIQ_NNQUANT=1, FIQ_LFPQUANT=2`.

`FREE_IMAGE_DITHER`: `FID_FS=0, FID_BAYER4x4=1, FID_BAYER8x8=2, FID_CLUSTER6x6=3,
FID_CLUSTER8x8=4, FID_CLUSTER16x16=5, FID_BAYER16x16=6`.

`FREE_IMAGE_FILTER`: `FILTER_BOX=0, FILTER_BICUBIC=1, FILTER_BILINEAR=2, FILTER_BSPLINE=3,
FILTER_CATMULLROM=4, FILTER_LANCZOS3=5`.

`FREE_IMAGE_COLOR_CHANNEL`: `FICC_RGB=0, FICC_RED=1, FICC_GREEN=2, FICC_BLUE=3, FICC_ALPHA=4,
FICC_BLACK=5, FICC_REAL=6, FICC_IMAG=7, FICC_MAG=8, FICC_PHASE=9`.

`FREE_IMAGE_MDTYPE`: `FIDT_NOTYPE=0, FIDT_BYTE=1, FIDT_ASCII=2, FIDT_SHORT=3, FIDT_LONG=4,
FIDT_RATIONAL=5, FIDT_SBYTE=6, FIDT_UNDEFINED=7, FIDT_SSHORT=8, FIDT_SLONG=9,
FIDT_SRATIONAL=10, FIDT_FLOAT=11, FIDT_DOUBLE=12, FIDT_IFD=13, FIDT_PALETTE=14,
FIDT_LONG8=16, FIDT_SLONG8=17, FIDT_IFD8=18`.

`FREE_IMAGE_MDMODEL`: `FIMD_NODATA=-1, FIMD_COMMENTS=0, FIMD_EXIF_MAIN=1, FIMD_EXIF_EXIF=2,
FIMD_EXIF_GPS=3, FIMD_EXIF_MAKERNOTE=4, FIMD_EXIF_INTEROP=5, FIMD_IPTC=6, FIMD_XMP=7,
FIMD_GEOTIFF=8, FIMD_ANIMATION=9, FIMD_CUSTOM=10, FIMD_EXIF_RAW=11`.

Pixel index / masks / shifts (little-endian BGR):
`FI_RGBA_RED=2, GREEN=1, BLUE=0, ALPHA=3`; `FI_RGBA_RED_MASK=0x00FF0000,
GREEN_MASK=0x0000FF00, BLUE_MASK=0x000000FF, ALPHA_MASK=0xFF000000`;
`FI_RGBA_RED_SHIFT=16, GREEN_SHIFT=8, BLUE_SHIFT=0, ALPHA_SHIFT=24`;
`FI_RGBA_RGB_MASK = RED|GREEN|BLUE`. `FI16_555_*`/`FI16_565_*` masks/shifts present.

Load/save flags (classic values): `FIF_LOAD_NOPIXELS=0x8000`; `BMP_DEFAULT=0`,
`BMP_SAVE_RLE=1`; `GIF_DEFAULT=0`, `GIF_LOAD256=1`, `GIF_PLAYBACK=2`;
`JPEG_DEFAULT=0, JPEG_FAST=0x0001, JPEG_ACCURATE=0x0002, JPEG_CMYK=0x0004,
JPEG_EXIFROTATE=0x0008, JPEG_GREYSCALE=0x0010, JPEG_QUALITYSUPERB=0x80,
JPEG_QUALITYGOOD=0x0100, JPEG_QUALITYNORMAL=0x0200, JPEG_QUALITYAVERAGE=0x0400,
JPEG_QUALITYBAD=0x0800, JPEG_PROGRESSIVE=0x2000, JPEG_SUBSAMPLING_411=0x1000,
JPEG_SUBSAMPLING_420=0x4000, JPEG_SUBSAMPLING_422=0x8000, JPEG_SUBSAMPLING_444=0x10000,
JPEG_OPTIMIZE=0x20000, JPEG_BASELINE=0x40000`; `PNG_DEFAULT=0, PNG_IGNOREGAMMA=1,
PNG_Z_BEST_SPEED=0x0001, PNG_Z_DEFAULT_COMPRESSION=0x0006, PNG_Z_NO_COMPRESSION=0x0100,
PNG_INTERLACED=0x0200`; `WEBP_DEFAULT=0, WEBP_LOSSLESS=0x100`; `FI_COLOR_*`;
`FI_RESCALE_DEFAULT=0, FI_RESCALE_TRUE_COLOR=0x01, FI_RESCALE_OMIT_METADATA=0x02`.
(TIFF/TARGA/PNM flag groups removed.)

---

## 3. Functions dropped from the shipped header

Classic 3.18.0 functions ES-DE never references are omitted rather than declared-and-stubbed; each would otherwise need a stub to keep `link_all` resolving. If a test or future consumer needs one, re-add it in classic spelling and provide a definition.

- Allocate/clone: `FreeImage_Allocate`, `AllocateT`, `Clone`, `HasPixels`,
  `AllocateEx`, `AllocateExT`, `Copy`, `Paste`, `CreateView`, `Composite`,
  `FillBackground`, `EnlargeCanvas`.
- Helpers: `IsLittleEndian`, `LookupX11Color`, `LookupSVGColor`.
- File-type/handles: `GetFileTypeFromHandle`, `Validate`, `ValidateU`,
  `ValidateFromHandle`, `ValidateFromMemory`.
- Plugin registry (all): `RegisterLocalPlugin`, `RegisterExternalPlugin`,
  `GetFIFCount`, `SetPluginEnabled`, `IsPluginEnabled`, `GetFIFFromFormat`,
  `GetFIFFromMime`, `GetFormatFromFIF`, `GetFIFExtensionList`, `GetFIFDescription`,
  `GetFIFRegExpr`, `GetFIFMimeType`, `FIFSupportsExportBPP`, `FIFSupportsExportType`,
  `FIFSupportsICCProfiles`, `FIFSupportsNoPixels`.
- Multipage write/reorder: `SaveMultiBitmapToHandle`, `AppendPage`, `InsertPage`,
  `DeletePage`, `MovePage`, `GetLockedPageNumbers`, `LoadMultiBitmapFromMemory`,
  `SaveMultiBitmapToMemory`.
- DIB info extras: `GetColorsUsed`, `GetDIBSize`, `GetMemorySize`, `GetPalette`,
  `GetDotsPerMeterX/Y`, `SetDotsPerMeterX/Y`, `GetInfoHeader`, `GetInfo`,
  `GetColorType`, `GetRedMask`, `GetGreenMask`, `GetBlueMask`, `GetTransparencyCount`,
  `GetTransparencyTable`, `SetTransparent`, `SetTransparencyTable`, `IsTransparent`,
  `SetTransparentIndex`, `GetTransparentIndex`, `HasBackgroundColor`,
  `GetBackgroundColor`, `SetBackgroundColor`, `GetThumbnail`, `SetThumbnail`.
- ICC: `GetICCProfile`, `CreateICCProfile`, `DestroyICCProfile` (the `FIICCPROFILE`
  struct is likewise dropped).
- Line converters: all `FreeImage_ConvertLine*` (24 of them).
- Smart conversions: `ConvertTo4Bits`, `ConvertTo8Bits`, `ConvertToGreyscale`,
  `ConvertTo16Bits555`, `ConvertTo16Bits565`, `ColorQuantize`, `ColorQuantizeEx`,
  `Threshold`, `Dither`, `ConvertToFloat`, `ConvertToRGBF`, `ConvertToRGBAF`,
  `ConvertToUINT16`, `ConvertToRGB16`, `ConvertToRGBA16`, `ConvertToStandardType`,
  `ConvertToType`.
- Tone mapping: `ToneMapping`, `TmoDrago03`, `TmoReinhard05`, `TmoReinhard05Ex`,
  `TmoFattal02`.
- ZLib: `ZLibCompress`, `ZLibUncompress`, `ZLibGZip`, `ZLibGUnzip`, `ZLibCRC32`.
- Metadata (only `GetMetadata`/`GetTagCount`/`GetTagValue` kept): `CreateTag`,
  `DeleteTag`, `CloneTag`, `GetTagKey`, `GetTagDescription`, `GetTagID`, `GetTagType`,
  `GetTagLength`, `SetTagKey`, `SetTagDescription`, `SetTagID`, `SetTagType`,
  `SetTagCount`, `SetTagLength`, `SetTagValue`, `FindFirstMetadata`,
  `FindNextMetadata`, `FindCloseMetadata`, `SetMetadata`, `SetMetadataKeyValue`,
  `GetMetadataCount`, `CloneMetadata`, `TagToString`.
- JPEG lossless ops: `JPEGTransform`, `JPEGTransformU`, `JPEGCrop`, `JPEGCropU`,
  `JPEGTransformFromHandle`, `JPEGTransformCombined`, `JPEGTransformCombinedU`,
  `JPEGTransformCombinedFromMemory`.
- Rotation/color: `Rotate`, `RotateEx`, `FlipHorizontal`, `FlipVertical`,
  `MakeThumbnail`, `RescaleRect`, `AdjustCurve`, `AdjustGamma`, `AdjustBrightness`,
  `AdjustContrast`, `Invert`, `GetHistogram`, `GetAdjustColorsLookupTable`,
  `AdjustColors`, `ApplyColorMapping`, `SwapColors`, `ApplyPaletteIndexMapping`,
  `SwapPaletteIndices`, `GetChannel`, `SetChannel`, `GetComplexChannel`,
  `SetComplexChannel`, `MultigridPoissonSolver`.

---

## 4. Internal architecture

The layering, bottom-up. Each concern is a `foo.h` plus the `foo.cpp` that defines its
declarations in order; dependency direction is strict — a lower layer never includes a
higher one.

- `src/dib.h` / `src/dib.cpp` — the core type. `fi::Dib`: bottom-up DIB with
  4-byte-aligned pitch, private pixel/palette/tag state behind accessors
  (`pixels()`, `row(y)`, `palette()`, `transparency()`, `is_indexed()`, ...), tag
  storage (`Tag`, `TagSet`, `find()` returning `FITAG const*`), and the `Dib::make(...)`
  factory that enforces the bpp/mask table. No public fields, no C-ABI handle types.
- `src/codec.h` — the codec seam: `class Codec`, a pure-virtual interface
  (`format_name`/`extensions`/`description`, `validate`, `load`, `save`, `page_count`)
  operating on `FreeImageIO&` + `fi_handle` and handing back an owning
  `std::unique_ptr<Dib>`. No function pointers, no handle types cross it.
- `src/codecs/{bmp,jpeg,png,gif,webp}.{h,cpp}` — one stateless `Codec` each, depending
  only on `dib.h`/`codec.h`/`io.h` and its own backend. Each TU exposes one
  `Codec const` object (e.g. `fi::png_codec`) for the registry.
- `src/registry.h` / `src/registry.cpp` — the aggregation layer. A sparse table indexed
  by the classic FIF value with only the five ids populated; `registered()`, `detect()`
  (iterates ascending, restores the stream to 0), `from_extension()`, and the
  `load`/`save` dispatch wrappers. **This is the only TU that includes the codec
  headers.**
- `src/io.h` / `src/io.cpp` — the `FreeImageIO` adapters (file and memory) and the
  `FIMEMORY`-backed stream (`MemoryStream` is file-local; `io.h` publishes the
  handle-level operations). `FILE_ptr`/`FIMEMORY_ptr` live here.
- `src/convert.h` / `src/convert.cpp` — conversions, raw-bits import/export, pixel
  access, premultiply.
- `src/resample.h` / `src/resample.cpp` — `rescale()`, taking a `Dib const&` and
  returning an owning `std::unique_ptr<Dib>`.
- `src/multipage.h` / `src/multipage.cpp` — the `FIMULTIBITMAP` container.
- `src/abi.h` / `src/abi.cpp` — the FIBITMAP↔Dib bridge (`FIBITMAP_ptr`,
  `adopt_bitmap`/`make_bitmap`/`make_bitmap_copy`, `deref`). Audience: the marshal TU
  only; no codec or core layer ever sees it.
- `src/FreeImage.cpp` — the sole marshal layer: every public entry point, validating and
  converting at the handle seam and delegating one level down. Answering trivial
  requests (version, copyright, message sink, init no-ops) is the only logic it holds.

Because handles never appear below the seam, the public ABI and the internal C++
interface are decoupled by construction rather than by convention.

## 5. Tests

`tests/` holds the doctest suite (pinned via pkg-config; the suite is built only with
`-DFREEIMAGE_FOR_ES_DE_TESTS=ON`). `helpers_codecs.{h,cpp}` carry independent reference
encoders (libpng/libjpeg-turbo/libwebp/giflib plus hand-built GIF89a builders), so
oracle expectations never come from the implementation under test. `esde_surface.cpp`
replicates ES-DE's own call sites against the public header — the drop-in proof
translation unit.

## 6. Notes / decisions already locked

- `GetVersion()` == `"3.18.0"`; version macros `3/18/0`. MIT licensed,
  Copyright (c) 2026 Joshua Gustafson (`LICENSE`, `meta.license`, and
  `GetCopyrightMessage()` all carry the attribution).
- GIF save of non-indexed dibs returns FALSE (no auto-quantize). PNG applies classic
  `gAMA` correction on load. Shared build self-initializes.
- Soname: built with `VERSION 3.18.0` / `SOVERSION 3` → `libfreeimage.so.3` +
  unversioned `libfreeimage.so`; ES-DE links via the package dir + `freeimage.pc`.
- Backends (nixpkgs `nixos-26.05`): `zlib`, `libjpeg_turbo`, `libpng`, `giflib`,
  `libwebp` (no libtiff). CMake resolves them via pkg-config on a best-effort,
  non-required basis, each module on its own so one absent `.pc` cannot drop the
  others: `libjpeg`, `libpng16`, `libwebp`, `zlib`. giflib ships no pkg-config module
  in nixpkgs and is passed to the linker by name.
