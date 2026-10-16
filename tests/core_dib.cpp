// DIB geometry, the 4-byte row alignment, and the bottom-up orientation contract.
//
// ConvertFromRawBits is the only public dib constructor available to a test (the
// classic Allocate/Clone family is dropped from the shipped header), so every dib
// here is built from a caller-owned raw buffer whose contents the contract pins.
#include <cstring>

#include "helpers.h"

namespace {

constexpr int kWidth = 2;
constexpr int kHeight = 4;

struct Geometry {
  int width;
  int bpp;
  unsigned line;
};

}  // namespace

TEST_CASE("[dib] geometry + pitch alignment for odd widths per §1.1 CalculateLine / §2.4") {
  // §1.1: GetLine(w, bpp) == (((w * bpp + 7) >> 3) + 3) & ~3, evaluated by hand.
  static constexpr Geometry cases[] = {
      {1, 24, 4},
      {2, 24, 8},
      {3, 24, 12},
      {5, 24, 16},
      {7, 24, 24},
      {9, 24, 28},
      {1, 32, 4},
      {3, 32, 12},
      {5, 32, 20},
      {7, 32, 28},
      {8, 32, 32},
  };
  for (const Geometry& geometry : cases) {
    INFO("width " << geometry.width << ", bpp " << geometry.bpp);
    REQUIRE(fit::calc_line(geometry.width, geometry.bpp) == geometry.line);

    const fit::Bitmap dib = fit::bitmap_from_pattern(geometry.width, 2, static_cast<unsigned>(geometry.bpp));
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    CHECK(FreeImage_GetWidth(dib.get()) == static_cast<unsigned>(geometry.width));
    CHECK(FreeImage_GetHeight(dib.get()) == 2u);
    CHECK(FreeImage_GetBPP(dib.get()) == static_cast<unsigned>(geometry.bpp));
    CHECK(FreeImage_GetLine(dib.get()) == geometry.line);
    // §1.1: "GetPitch == GetLine for internal dibs".
    CHECK(FreeImage_GetPitch(dib.get()) == geometry.line);
    // §2.4: FIT_BITMAP for everything the library produces.
    CHECK(FreeImage_GetImageType(dib.get()) == FIT_BITMAP);
  }
}

TEST_CASE("[dib] bottom-up proof: scanline 0 is the visual bottom per §1.1 + §1.3") {
  const unsigned line = fit::calc_line(kWidth, 32);
  // Read as a top-down buffer, fixture row 0 is the visual top and row h-1 the
  // visual bottom; the topdown argument is what tells the library which it is.
  const fit::Bytes buffer = fit::make_buffer(kWidth, kHeight, 32, line);

  SUBCASE("topdown == TRUE: rows fill scanlines 0..h-1, then the dib is flipped (§1.3)") {
    const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, line, 32, TRUE);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    // Scanline 0 == visual bottom == the buffer's last row.
    CHECK_BYTES_EQ(fit::scanline_of(dib, 0), fit::row_of(buffer, line, kHeight - 1));
    REQUIRE_BYTES_EQ(fit::all_scanlines(dib), fit::golden_scanlines(buffer, kWidth, kHeight, line, 32, TRUE));
  }

  SUBCASE("topdown == FALSE: the ES-DE default treats buffer row 0 as the visual bottom (§1.3)") {
    const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, line, 32, FALSE);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    CHECK_BYTES_EQ(fit::scanline_of(dib, 0), fit::row_of(buffer, line, 0));
    REQUIRE_BYTES_EQ(fit::all_scanlines(dib), fit::golden_scanlines(buffer, kWidth, kHeight, line, 32, FALSE));
    // §1.1: scanline y is the linear index bits + pitch * y, so scanline 0 is the
    // array start and the rows run bottom to top.
    CHECK(FreeImage_GetBits(dib.get()) == FreeImage_GetScanLine(dib.get(), 0));
    for (int y = 0; y < kHeight; ++y) {
      CHECK(FreeImage_GetScanLine(dib.get(), y) == FreeImage_GetBits(dib.get()) + FreeImage_GetPitch(dib.get()) * y);
    }
  }
}

TEST_CASE("[dib] null-dib entry points return 0 / NULL per §2.4 + §2.1 null guards") {
  CHECK(FreeImage_GetWidth(nullptr) == 0u);
  CHECK(FreeImage_GetHeight(nullptr) == 0u);
  CHECK(FreeImage_GetBPP(nullptr) == 0u);
  CHECK(FreeImage_GetLine(nullptr) == 0u);
  CHECK(FreeImage_GetPitch(nullptr) == 0u);
  CHECK(FreeImage_GetBits(nullptr) == nullptr);
  CHECK(FreeImage_GetScanLine(nullptr, 0) == nullptr);
  // Classic reports FIT_UNKNOWN for a missing dib; §2.1 pins the classic return.
  CHECK(FreeImage_GetImageType(nullptr) == FIT_UNKNOWN);
}

TEST_CASE("[dib] ImageIO scanline copy loop keeps the bottom-up row order per §1.2") {
  const unsigned line = fit::calc_line(kWidth, 32);
  const fit::Bytes buffer = fit::make_buffer(kWidth, kHeight, 32, line);
  // The bottom-up shape ES-DE's canvas/screenshot vectors are in (§1.2 table).
  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, line, 32, FALSE);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
  // ImageIO copies width * 4 bytes per row, so the library must report that stride.
  REQUIRE(FreeImage_GetPitch(dib.get()) == static_cast<unsigned>(kWidth) * 4u);

  // ImageIO.cpp:47-51 verbatim: out row i receives GetScanLine(i).
  fit::Bytes texture(static_cast<std::size_t>(kWidth) * kHeight * 4, 0);
  for (std::size_t i = 0; i < static_cast<std::size_t>(kHeight); ++i) {
    const BYTE* scanLine = FreeImage_GetScanLine(dib.get(), static_cast<int>(i));
    REQUIRE(scanLine != nullptr);
    std::memcpy(texture.data() + i * kWidth * 4, scanLine, static_cast<std::size_t>(kWidth) * 4);
  }
  // So the texture buffer is the bottom-up DIB verbatim: row 0 is the visual bottom.
  REQUIRE_BYTES_EQ(texture, buffer);
}
