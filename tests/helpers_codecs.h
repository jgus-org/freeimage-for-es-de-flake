// Independent reference fixtures and codecs for the CODEC + GIF + resample TDD
// suite. These deliberately do NOT use the library under test: PNG/JPEG/WebP go
// through libpng/libjpeg-turbo/libwebp, and PNG/GIF/BMP byte streams are also
// hand-built, so the tests attack the frozen public API from a direction that
// cannot inherit a bug from the implementation.
//
// Nothing here calls FreeImage_*; the module is free of the library so it
// compiles and links against only the external image backends. Reading a
// FIBITMAP back through the public API lives in the test translation units.
#pragma once

#include <cstdint>
#include <vector>

namespace fic {

// Reference pixel. Logical RGBA; channel order is independent of the DIB's
// in-memory BGR(A) layout (the public API translates on GetPixelColor).
struct Rgba {
  uint8_t r = 0;
  uint8_t g = 0;
  uint8_t b = 0;
  uint8_t a = 255;
  bool operator==(const Rgba&) const = default;
};

// Reference canvas, stored top-down: row 0 is the VISUAL TOP. (The DIB under
// test is bottom-up; the test TUs do the y-flip when comparing.)
struct Image {
  int w = 0;
  int h = 0;
  std::vector<Rgba> px;  // w*h, row-major, top-down

  Rgba at(int x, int y) const { return px[static_cast<size_t>(y) * w + x]; }
};

// --- procedural fixtures --------------------------------------------------

// Row/column-identifiable ramp that also carries semi-transparent alpha, so a
// pixel's (x,y) can be recovered and alpha survives round-trips.
Image Pattern(int w, int h);

Image Solid(int w, int h, Rgba c);

// Two horizontal halves; used for orientation probes where lossy coding makes
// per-pixel equality impossible but the split is preserved.
Image Split(int w, int h, Rgba top, Rgba bottom);

// --- encoders (independent backends / hand builders) ----------------------

std::vector<uint8_t> EncodePngRGB(const Image&);   // color type 2, 8-bit  => load 24bpp
std::vector<uint8_t> EncodePngRGBA(const Image&);  // color type 6, 8-bit  => load 32bpp
std::vector<uint8_t> EncodePngGray(const Image&);  // color type 0, 8-bit  => load 8bpp
std::vector<uint8_t> EncodePngGrayWithGama(const Image&, double file_gamma);

std::vector<uint8_t> EncodeJpeg(const Image&, int quality = 75);      // baseline YCbCr => 24bpp
std::vector<uint8_t> EncodeJpegGray(const Image&, int quality = 75);  // grayscale       => 8bpp

std::vector<uint8_t> EncodeWebp(const Image&, int quality = 75);  // alpha iff any a<255

std::vector<uint8_t> EncodeBmp24(const Image&);  // BI_RGB 24bpp, bottom-up
std::vector<uint8_t> EncodeBmp32(const Image&);  // BI_RGB 32bpp, bottom-up, BGRA
std::vector<uint8_t> EncodeBmp8(const Image&);   // BI_RGB 8bpp indexed + palette
std::vector<uint8_t> EncodeBmp4(const Image&);   // BI_RGB 4bpp indexed + palette
std::vector<uint8_t> EncodeBmp1(const Image&);   // BI_RGB 1bpp indexed + palette

// --- reference decoders (independent backends) ----------------------------

struct PngIhdr {
  int bit_depth = 0;
  int color_type = 0;
  int compression = 0;
  int filter = 0;
  int interlace = 0;
  bool ok = false;
};

PngIhdr PngPeekIhdr(const std::vector<uint8_t>&);  // parse IHDR without decoding

Image DecodePng(const std::vector<uint8_t>&);   // libpng -> RGBA top-down (empty on failure)
Image DecodeJpeg(const std::vector<uint8_t>&);  // libjpeg -> RGB top-down (empty on failure)
Image DecodeWebp(const std::vector<uint8_t>&);  // libwebp -> RGBA top-down (empty on failure)

// --- reference Lanczos3 (design §2.8 written spec, straight port) ---------

// Per-channel, straight-alpha (alpha is a 4th independent channel), rounded to
// nearest byte. Deterministic. Output has the requested dst dimensions.
Image Lanczos3Reference(const Image& src, int dst_w, int dst_h);

// --- GIF89a byte-stream builder -------------------------------------------

struct GifPalette {
  std::vector<Rgba> entries;  // up to 256; padded to a power of two on emit
};

struct GifFrame {
  int left = 0;
  int top = 0;
  int w = 0;
  int h = 0;
  std::vector<uint8_t> idx;  // w*h indices, top-down
  int disposal = 0;          // 0 unspecified, 1 leave, 2 background, 3 previous
  int delay_cs = 0;          // GCE delay, 1/100 s units
  bool transparent = false;
  int trans_index = 0;
  std::vector<Rgba> local;  // optional local color table (empty => use global)
};

std::vector<uint8_t>
BuildGif(int logical_w, int logical_h, const GifPalette& global, int bg_index, const std::vector<GifFrame>& frames);

// Cross-check helper: run an independent decode of a built GIF (via giflib) and
// return the per-frame transparency index (-1 when the frame has none), the
// decoded disposal/delay, and the composited-by-giflib top-left pixel. Used only
// by the verification harness to prove the builder emitted the intended bytes;
// the suite itself asserts against hand-computed goldens, not giflib.
struct GifProbe {
  int page_count = 0;
  std::vector<int> trans_index;
  std::vector<int> disposal;
  std::vector<int> delay_cs;
  std::vector<int> frame_w;
  std::vector<int> frame_h;
  std::vector<int> frame_left;
  std::vector<int> frame_top;
};

GifProbe GifProbeBytes(const std::vector<uint8_t>& bytes);  // giflib decode; page_count==0 on failure

}  // namespace fic
