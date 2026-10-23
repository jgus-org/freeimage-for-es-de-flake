// Save-side semantics through the frozen public seam (design §4). Inputs are
// built with the public ConvertFromRawBits; the bytes the library writes are
// re-read from disk and checked with the INDEPENDENT libpng/libjpeg/libwebp
// decoders and PngPeekIhdr, so a save test cannot pass by the library agreeing
// with itself. Every case needs a non-null dib (CORE-owned seam, null in the red
// phase), so these are deterministic red until ConvertFromRawBits and Save land;
// all reads are guarded so the red phase never crashes.
#include <FreeImage.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "helpers.h"
#include "helpers_codecs.h"

namespace {

using fic::Image;
using fic::Rgba;

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) {
    return {};
  }
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<uint8_t> out(n > 0 ? static_cast<size_t>(n) : 0);
  if (n > 0) {
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    out.resize(got);
  }
  std::fclose(f);
  return out;
}

bool WriteJunkFile(const std::filesystem::path& path, size_t bytes) {
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  std::vector<uint8_t> junk(bytes, 0xAB);
  const bool ok = std::fwrite(junk.data(), 1, junk.size(), f) == bytes;
  std::fclose(f);
  return ok;
}

FIBITMAP* Make32(const Image& img) {
  const int pitch = img.w * 4;
  std::vector<uint8_t> buf(static_cast<size_t>(pitch) * img.h, 0);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      Rgba c = img.at(x, y);
      uint8_t* p = buf.data() + static_cast<size_t>(y) * pitch + x * 4;
      p[0] = c.b;
      p[1] = c.g;
      p[2] = c.r;
      p[3] = c.a;
    }
  }
  return FreeImage_ConvertFromRawBits(
      buf.data(),
      img.w,
      img.h,
      pitch,
      32,
      FI_RGBA_RED_MASK,
      FI_RGBA_GREEN_MASK,
      FI_RGBA_BLUE_MASK,
      TRUE);
}

FIBITMAP* Make24(const Image& img) {
  const int pitch = ((img.w * 3 + 3) / 4) * 4;
  std::vector<uint8_t> buf(static_cast<size_t>(pitch) * img.h, 0);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      Rgba c = img.at(x, y);
      uint8_t* p = buf.data() + static_cast<size_t>(y) * pitch + x * 3;
      p[0] = c.b;
      p[1] = c.g;
      p[2] = c.r;
    }
  }
  return FreeImage_ConvertFromRawBits(
      buf.data(),
      img.w,
      img.h,
      pitch,
      24,
      FI_RGBA_RED_MASK,
      FI_RGBA_GREEN_MASK,
      FI_RGBA_BLUE_MASK,
      TRUE);
}

Rgba SampleDib(FIBITMAP* dib, int x, int ry) {
  RGBQUAD q{0, 0, 0, 0};
  const unsigned h = dib ? FreeImage_GetHeight(dib) : 0;
  FreeImage_GetPixelColor(dib, static_cast<unsigned>(x), h - 1 - static_cast<unsigned>(ry), &q);
  return Rgba{q.rgbRed, q.rgbGreen, q.rgbBlue, q.rgbReserved};
}

}  // namespace

// --- PNG: RGBA dib saved as color-type 6, non-interlaced, round-trips -------

TEST_CASE("[save §4] PNG save of a 32bpp dib writes color type 6, non-interlaced, lossless") {
  const fit::TempDir dir;
  Image img = fic::Pattern(17, 11);  // carries alpha
  FIBITMAP* dib = Make32(img);
  CHECK(dib != nullptr);
  const std::filesystem::path path = dir.file("rgba.png");
  const BOOL saved = FreeImage_Save(FIF_PNG, dib, path.c_str(), 0);
  CHECK(saved == TRUE);
  const std::vector<uint8_t> bytes = ReadFile(path);
  const fic::PngIhdr ihdr = fic::PngPeekIhdr(bytes);
  CHECK(ihdr.ok);
  CHECK(ihdr.color_type == 6);  // truecolor + alpha
  CHECK(ihdr.interlace == 0);
  CHECK(ihdr.bit_depth == 8);
  const Image back = fic::DecodePng(bytes);
  CHECK(back.w == 17);
  CHECK(back.h == 11);
  bool identical = back.w == 17 && back.h == 11;
  for (int y = 0; y < img.h && identical; ++y) {
    for (int x = 0; x < img.w; ++x) {
      if (!(back.at(x, y) == img.at(x, y))) {
        identical = false;
        break;
      }
    }
  }
  CHECK(identical);
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- WebP: flags=85 => quality 85, decodes, and leaves the caller dib intact -

TEST_CASE("[save §4] WebP save (flags=85) decodes to dims and does not mutate the input dib") {
  const fit::TempDir dir;
  Image img = fic::Pattern(20, 16);
  for (auto& p : img.px) {
    p.a = 255;
  }
  FIBITMAP* dib = Make32(img);
  CHECK(dib != nullptr);
  const Rgba before = SampleDib(dib, 3, 3);
  const std::filesystem::path path = dir.file("webp.webp");
  const BOOL saved = FreeImage_Save(FIF_WEBP, dib, path.c_str(), 85);
  CHECK(saved == TRUE);
  const std::vector<uint8_t> bytes = ReadFile(path);
  const Image back = fic::DecodeWebp(bytes);
  CHECK(back.w == 20);
  CHECK(back.h == 16);
  // The plugin must not leave the caller's dib flipped/premultiplied in place.
  CHECK(SampleDib(dib, 3, 3) == before);
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- JPEG: flags low 7 bits are quality (85), baseline decodes at dims -------

TEST_CASE("[save §4] JPEG save (flags=85) writes a decodable baseline image") {
  const fit::TempDir dir;
  Image img = fic::Pattern(20, 16);
  for (auto& p : img.px) {
    p.a = 255;
  }
  FIBITMAP* dib = Make24(img);
  CHECK(dib != nullptr);
  const std::filesystem::path path = dir.file("q85.jpg");
  const BOOL saved = FreeImage_Save(FIF_JPEG, dib, path.c_str(), 85);
  CHECK(saved == TRUE);
  const std::vector<uint8_t> bytes = ReadFile(path);
  CHECK(bytes.size() >= 2);
  CHECK((bytes.size() >= 2 && bytes[0] == 0xFF && bytes[1] == 0xD8));  // JPEG SOI
  const Image back = fic::DecodeJpeg(bytes);
  CHECK(back.w == 20);
  CHECK(back.h == 16);
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- GIF: only 1/4/8bpp is saveable; 24 and 32bpp are rejected --------------

TEST_CASE("[save §4] GIF save rejects 24bpp and 32bpp dibs (single-frame 1/4/8 only)") {
  const fit::TempDir dir;
  Image rgb = fic::Pattern(16, 16);
  for (auto& p : rgb.px) {
    p.a = 255;
  }
  FIBITMAP* d24 = Make24(rgb);
  FIBITMAP* d32 = Make32(rgb);
  CHECK(d24 != nullptr);
  CHECK(d32 != nullptr);

  const std::filesystem::path p24 = dir.file("gif24.gif");
  CHECK(FreeImage_Save(FIF_GIF, d24, p24.c_str(), 0) == FALSE);

  const std::filesystem::path p32 = dir.file("gif32.gif");
  CHECK(FreeImage_Save(FIF_GIF, d32, p32.c_str(), 0) == FALSE);

  if (d24) {
    FreeImage_Unload(d24);
  }
  if (d32) {
    FreeImage_Unload(d32);
  }
}

// --- Save opens the output file before validating, so a rejected GIF write
//     still leaves the pre-existing file truncated to zero bytes ---------------

TEST_CASE("[save §4] Save truncates an existing target even when the codec later refuses") {
  const fit::TempDir dir;
  Image rgb = fic::Pattern(16, 16);
  for (auto& p : rgb.px) {
    p.a = 255;
  }
  FIBITMAP* d24 = Make24(rgb);
  CHECK(d24 != nullptr);
  const std::filesystem::path path = dir.file("trunc.gif");
  CHECK(WriteJunkFile(path, 512));
  CHECK(ReadFile(path).size() == 512u);
  // 24bpp is not GIF-saveable; the classic open-w+b still truncates the target.
  CHECK(FreeImage_Save(FIF_GIF, d24, path.c_str(), 0) == FALSE);
  CHECK(ReadFile(path).size() == 0u);
  if (d24) {
    FreeImage_Unload(d24);
  }
}

// --- BMP: 24bpp round-trips exactly through the library loader --------------

TEST_CASE("[save §4] BMP save then reload reproduces the 24bpp pixels exactly") {
  const fit::TempDir dir;
  Image img = fic::Pattern(13, 9);
  for (auto& p : img.px) {
    p.a = 255;
  }
  FIBITMAP* dib = Make24(img);
  CHECK(dib != nullptr);
  const std::filesystem::path path = dir.file("rt.bmp");
  CHECK(FreeImage_Save(FIF_BMP, dib, path.c_str(), 0) == TRUE);
  FIBITMAP* back = FreeImage_Load(FIF_BMP, path.c_str(), 0);
  CHECK(back != nullptr);
  if (back) {
    CHECK(FreeImage_GetWidth(back) == 13u);
    CHECK(FreeImage_GetHeight(back) == 9u);
    CHECK(FreeImage_GetBPP(back) == 24);
    bool all = true;
    for (int y = 0; y < img.h && all; ++y) {
      for (int x = 0; x < img.w; ++x) {
        Rgba got = SampleDib(back, x, y);
        Rgba want = img.at(x, y);
        if (got.r != want.r || got.g != want.g || got.b != want.b) {
          all = false;
          break;
        }
      }
    }
    CHECK(all);
    FreeImage_Unload(back);
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- Save with a null dib fails without writing the target ------------------

TEST_CASE("[save §4] Save with a null dib returns FALSE and leaves the target empty") {
  const fit::TempDir dir;
  const std::filesystem::path path = dir.file("nullout.png");
  CHECK(FreeImage_Save(FIF_PNG, nullptr, path.c_str(), 0) == FALSE);
  CHECK(ReadFile(path).empty());
}
