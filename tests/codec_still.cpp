// Per-format load + detection behavior through the frozen public API. Each
// TEST_CASE name cites the design.md clause it pins. Independent fixtures come
// from helpers_codecs (libpng/libjpeg/libwebp/hand-built BMP), never the library.
#include <FreeImage.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "helpers_codecs.h"

namespace {

using fic::Image;
using fic::Rgba;

// Load bytes from memory with the given fif; the caller keeps `bytes` alive.
FIBITMAP* LoadMem(const std::vector<uint8_t>& bytes, FREE_IMAGE_FORMAT fif, int flags = 0) {
  FIMEMORY* m = FreeImage_OpenMemory(const_cast<BYTE*>(bytes.data()), static_cast<DWORD>(bytes.size()));
  FIBITMAP* dib = FreeImage_LoadFromMemory(fif, m, flags);
  FreeImage_CloseMemory(m);
  return dib;
}

// Reference Image is top-down; the DIB is bottom-up (scanline 0 == visual
// bottom). GetPixelColor(x, y) reads scanline y. So reference row `ry`
// (0 = visual top) corresponds to dib scanline (height-1-ry).
bool SampleEq(FIBITMAP* dib, const Image& ref, int x, int ry) {
  RGBQUAD got;
  if (!FreeImage_GetPixelColor(dib, static_cast<unsigned>(x), static_cast<unsigned>(dib ? ref.h - 1 - ry : 0), &got)) {
    return false;
  }
  Rgba want = ref.at(x, ry);
  // Classic GetPixelColor reports rgbReserved = 0 for a 24bpp dib (PixelAccess.cpp:95-101),
  // so alpha is only comparable when the dib carries it; 32bpp still compares it.
  return got.rgbRed == want.r && got.rgbGreen == want.g && got.rgbBlue == want.b &&
         (FreeImage_GetBPP(dib) == 24 || got.rgbReserved == want.a);
}

}  // namespace

// --- §3: detection magic bytes => FIFSupportsReading => load; bpp matrix ----

TEST_CASE("[still §3] RGB PNG detects as PNG and loads 24bpp") {
  Image img = fic::Pattern(16, 12);
  std::vector<uint8_t> png = fic::EncodePngRGB(img);
  FIMEMORY* m = FreeImage_OpenMemory(png.data(), static_cast<DWORD>(png.size()));
  CHECK(FreeImage_GetFileTypeFromMemory(m, 0) == FIF_PNG);
  CHECK(FreeImage_FIFSupportsReading(FIF_PNG) == TRUE);
  FIBITMAP* dib = FreeImage_LoadFromMemory(FIF_PNG, m);
  FreeImage_CloseMemory(m);
  CHECK(dib != nullptr);
  if (dib) {
    CHECK(FreeImage_GetImageType(dib) == FIT_BITMAP);
    CHECK(FreeImage_GetBPP(dib) == 24);
    CHECK(FreeImage_GetWidth(dib) == 16u);
    CHECK(FreeImage_GetHeight(dib) == 12u);
    FreeImage_Unload(dib);
  }
}

TEST_CASE("[still §1.1/§3] RGBA PNG loads 32bpp, bottom-up golden via GetPixelColor") {
  Image img = fic::Pattern(13, 9);
  std::vector<uint8_t> png = fic::EncodePngRGBA(img);
  FIBITMAP* dib = LoadMem(png, FIF_PNG);
  CHECK(dib != nullptr);
  if (dib) {
    CHECK(FreeImage_GetBPP(dib) == 32);
    // y = 0 (visual bottom) must equal the reference BOTTOM row, not the top.
    CHECK(SampleEq(dib, img, 3, 0));  // visual top
    CHECK(SampleEq(dib, img, 3, 8));  // visual bottom
    CHECK(SampleEq(dib, img, 0, 4));
    CHECK(SampleEq(dib, img, 12, 4));
    // Explicit orientation pin: scanline 0 (bottom) == reference last row.
    RGBQUAD bottom;
    CHECK(FreeImage_GetPixelColor(dib, 5, 0, &bottom));
    Rgba ref_bottom = img.at(5, img.h - 1);
    CHECK(bottom.rgbRed == ref_bottom.r);
    CHECK(bottom.rgbBlue == ref_bottom.b);
    FreeImage_Unload(dib);
  }
}

TEST_CASE("[still §3] grayscale PNG loads 8bpp") {
  Image img = fic::Pattern(8, 8);
  std::vector<uint8_t> png = fic::EncodePngGray(img);
  FIMEMORY* gm = FreeImage_OpenMemory(png.data(), static_cast<DWORD>(png.size()));
  CHECK(FreeImage_GetFileTypeFromMemory(gm, 0) == FIF_PNG);
  FreeImage_CloseMemory(gm);
  FIBITMAP* dib = LoadMem(png, FIF_PNG);
  CHECK(dib != nullptr);
  if (dib) {
    CHECK(FreeImage_GetBPP(dib) == 8);
    CHECK(FreeImage_GetWidth(dib) == 8u);
    FreeImage_Unload(dib);
  }
}

TEST_CASE("[still §3] baseline JPEG loads 24bpp; gray JPEG loads 8bpp") {
  Image img = fic::Pattern(24, 18);
  FIBITMAP* c = LoadMem(fic::EncodeJpeg(img, 90), FIF_JPEG);
  CHECK(c != nullptr);
  if (c) {
    CHECK(FreeImage_GetBPP(c) == 24);
    CHECK(FreeImage_GetWidth(c) == 24u);
    FreeImage_Unload(c);
  }
  FIBITMAP* g = LoadMem(fic::EncodeJpegGray(img, 90), FIF_JPEG);
  CHECK(g != nullptr);
  if (g) {
    CHECK(FreeImage_GetBPP(g) == 8);
    FreeImage_Unload(g);
  }
}

TEST_CASE("[still §3] JPEG bottom-up orientation (lossy-safe region test)") {
  // Top half blue, bottom half red; JPEG is lossy so compare region interiors.
  Image img = fic::Split(24, 24, Rgba{0, 0, 255, 255}, Rgba{255, 0, 0, 255});
  FIBITMAP* dib = LoadMem(fic::EncodeJpeg(img, 95), FIF_JPEG);
  CHECK(dib != nullptr);
  if (dib) {
    RGBQUAD bottom, top;
    CHECK(FreeImage_GetPixelColor(dib, 12, 1, &bottom));  // scanline 1 == visual bottom band == RED
    CHECK(FreeImage_GetPixelColor(dib, 12, 22, &top));    // scanline 22 == visual top band == BLUE
    CHECK(bottom.rgbRed > 150);
    CHECK(bottom.rgbBlue < 100);
    CHECK(top.rgbBlue > 150);
    CHECK(top.rgbRed < 100);
    FreeImage_Unload(dib);
  }
}

TEST_CASE("[still §3] WebP opaque loads 24bpp, WebP with alpha loads 32bpp") {
  Image opaque = fic::Pattern(16, 16);
  for (auto& p : opaque.px) {
    p.a = 255;
  }
  FIBITMAP* o = LoadMem(fic::EncodeWebp(opaque, 90), FIF_WEBP);
  CHECK(o != nullptr);
  if (o) {
    CHECK(FreeImage_GetBPP(o) == 24);
    FreeImage_Unload(o);
  }
  Image alpha = fic::Pattern(16, 16);  // Pattern carries a<255
  FIBITMAP* a = LoadMem(fic::EncodeWebp(alpha, 90), FIF_WEBP);
  CHECK(a != nullptr);
  if (a) {
    CHECK(FreeImage_GetBPP(a) == 32);
    FreeImage_Unload(a);
  }
}

TEST_CASE("[still §3] BMP load output bpp per file bpp (1/4/8/24/32)") {
  Image img = fic::Pattern(16, 16);
  struct Case {
    std::vector<uint8_t> bytes;
    int bpp;
  };
  const Case cases[] = {
      {fic::EncodeBmp1(img), 1},
      {fic::EncodeBmp4(img), 4},
      {fic::EncodeBmp8(img), 8},
      {fic::EncodeBmp24(img), 24},
      {fic::EncodeBmp32(img), 32},
  };
  for (const Case& c : cases) {
    FIBITMAP* dib = LoadMem(c.bytes, FIF_BMP);
    CHECK(dib != nullptr);
    if (dib) {
      CHECK(FreeImage_GetBPP(dib) == static_cast<unsigned>(c.bpp));
      CHECK(FreeImage_GetWidth(dib) == 16u);
      CHECK(FreeImage_GetHeight(dib) == 16u);
      FreeImage_Unload(dib);
    }
  }
}

TEST_CASE("[still §3] BMP 24/32 bottom-up golden (lossless exact)") {
  Image img = fic::Pattern(11, 7);
  FIBITMAP* b24 = LoadMem(fic::EncodeBmp24(img), FIF_BMP);
  CHECK(b24 != nullptr);
  if (b24) {
    CHECK(SampleEq(b24, img, 2, 0));
    CHECK(SampleEq(b24, img, 2, 6));
    CHECK(SampleEq(b24, img, 10, 3));
    FreeImage_Unload(b24);
  }
  FIBITMAP* b32 = LoadMem(fic::EncodeBmp32(img), FIF_BMP);
  CHECK(b32 != nullptr);
  if (b32) {
    CHECK(SampleEq(b32, img, 2, 0));
    CHECK(SampleEq(b32, img, 8, 5));
    FreeImage_Unload(b32);
  }
}

// --- §2.3: detection precedence, position restore, robustness ---------------

static FREE_IMAGE_FORMAT DetectMem(const std::vector<uint8_t>& b) {
  FIMEMORY* m = FreeImage_OpenMemory(const_cast<BYTE*>(b.data()), static_cast<DWORD>(b.size()));
  FREE_IMAGE_FORMAT fif = FreeImage_GetFileTypeFromMemory(m, 0);
  FreeImage_CloseMemory(m);
  return fif;
}

TEST_CASE("[still §2.3] GetFileTypeFromMemory detects each retained format") {
  Image img = fic::Pattern(8, 8);
  CHECK(DetectMem(fic::EncodePngRGB(img)) == FIF_PNG);
  CHECK(DetectMem(fic::EncodeJpeg(img, 90)) == FIF_JPEG);
  CHECK(DetectMem(fic::EncodeWebp(img, 90)) == FIF_WEBP);
  CHECK(DetectMem(fic::EncodeBmp24(img)) == FIF_BMP);
  fic::GifFrame f;
  f.w = f.h = 4;
  f.idx.assign(16, 0);
  std::vector<fic::GifFrame> frames(1, f);
  fic::GifPalette pal;
  pal.entries = {Rgba{0, 0, 0}, Rgba{255, 0, 0}};
  CHECK(DetectMem(fic::BuildGif(8, 8, pal, 0, frames)) == FIF_GIF);
}

TEST_CASE("[still §2.3] GetFileTypeFromMemory on garbage / null => UNKNOWN") {
  const std::vector<uint8_t> junk = {'n', 'o', 't', 'a', 'n', 'i', 'm', 'a', 'g', 'e', '!', '!'};
  CHECK(DetectMem(junk) == FIF_UNKNOWN);
  CHECK(FreeImage_GetFileTypeFromMemory(nullptr, 0) == FIF_UNKNOWN);
}

TEST_CASE("[still §2.3] detection iterates FIFs ascending, first match wins") {
  // A buffer whose first two bytes are the BMP magic and which also carries the
  // full PNG signature. FIF_BMP (0) is checked before FIF_PNG (13), so BMP wins
  // even though the PNG magic is present.
  const uint8_t pngsig[] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  std::vector<uint8_t> b = {'B', 'M'};
  b.insert(b.end(), std::begin(pngsig), std::end(pngsig));
  b.resize(64, 0);
  CHECK(DetectMem(b) == FIF_BMP);
}

TEST_CASE("[still §2.3] GetFileTypeFromMemory leaves the stream at position 0") {
  Image img = fic::Pattern(10, 10);
  std::vector<uint8_t> png = fic::EncodePngRGB(img);
  FIMEMORY* m = FreeImage_OpenMemory(png.data(), static_cast<DWORD>(png.size()));
  CHECK(FreeImage_GetFileTypeFromMemory(m, 0) == FIF_PNG);
  CHECK(FreeImage_TellMemory(m) == 0);
  // Because the stream was rewound, a load on the SAME stream still succeeds.
  FIBITMAP* dib = FreeImage_LoadFromMemory(FIF_PNG, m);
  CHECK(dib != nullptr);
  if (dib) {
    CHECK(FreeImage_GetWidth(dib) == 10u);
    FreeImage_Unload(dib);
  }
  FreeImage_CloseMemory(m);
}

TEST_CASE("[still §2.3/§3] truncated PNG loads as null without crashing") {
  Image img = fic::Pattern(20, 20);
  std::vector<uint8_t> png = fic::EncodePngRGB(img);
  png.resize(png.size() / 3);  // cut off mid-IDAT
  FIBITMAP* dib = LoadMem(png, FIF_PNG);
  CHECK(dib == nullptr);
}

TEST_CASE("[still §2.3] extension and content disagree: loader honors the passed FIF") {
  Image img = fic::Pattern(8, 8);
  // Correct extension => correct FIF (filename lookup is extension-only).
  CHECK(FreeImage_GetFIFFromFilename("photo.png") == FIF_PNG);
  CHECK(FreeImage_GetFIFFromFilename("photo.jpg") == FIF_JPEG);
  std::vector<uint8_t> png = fic::EncodePngRGB(img);
  // PNG bytes forced through the JPEG plugin (extension-driven) => reject, null.
  FIBITMAP* wrong = LoadMem(png, FIF_JPEG);
  CHECK(wrong == nullptr);
  // The same bytes through the correct plugin => load.
  FIBITMAP* right = LoadMem(png, FIF_PNG);
  CHECK(right != nullptr);
  if (right) {
    FreeImage_Unload(right);
  }
}

TEST_CASE("[still §2.2] removed formats are not supported") {
  // FIF_TIFF/PNM/TARGA are removed from the enum; assert their old ids report
  // no read/write support (17 = historic TARGA, 10 = historic PNM, 1 = JPEG used
  // only as a positive control).
  CHECK(FreeImage_FIFSupportsReading(static_cast<FREE_IMAGE_FORMAT>(17)) == FALSE);
  CHECK(FreeImage_FIFSupportsWriting(static_cast<FREE_IMAGE_FORMAT>(17)) == FALSE);
  CHECK(FreeImage_FIFSupportsReading(static_cast<FREE_IMAGE_FORMAT>(10)) == FALSE);
  // Positive control: the five retained formats support both directions.
  for (FREE_IMAGE_FORMAT f : {FIF_BMP, FIF_JPEG, FIF_PNG, FIF_GIF, FIF_WEBP}) {
    CHECK(FreeImage_FIFSupportsReading(f) == TRUE);
    CHECK(FreeImage_FIFSupportsWriting(f) == TRUE);
  }
}

TEST_CASE("[still §2.3] GetFIFFromFilenameU / case-insensitive extension") {
  CHECK(FreeImage_GetFIFFromFilename("IMAGE.PNG") == FIF_PNG);
  CHECK(FreeImage_GetFIFFromFilename("image.WebP") == FIF_WEBP);
  CHECK(FreeImage_GetFIFFromFilename("no-extension") == FIF_UNKNOWN);
}
