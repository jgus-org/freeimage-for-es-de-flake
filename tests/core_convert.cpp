// Conversion, premultiply and pixel-access semantics (§2.4, §2.5, §1.4).
//
// Golden bytes are computed from the contract's own arithmetic and from the fixture
// pattern, never from the dib under test.
#include <cstddef>

#include "helpers.h"

namespace {

// §1.4: RGBQUAD is packed with the BGR field order of the build's color order.
static_assert(sizeof(RGBQUAD) == 4);
static_assert(
    offsetof(RGBQUAD, rgbBlue) == 0 && offsetof(RGBQUAD, rgbGreen) == 1 && offsetof(RGBQUAD, rgbRed) == 2 &&
    offsetof(RGBQUAD, rgbReserved) == 3);

constexpr int kWidth = 2;
constexpr int kHeight = 3;

// Golden scanline array for a 32bpp dib whose scanline y holds pattern(y, x).
// §2.4: 24 -> 32 sets alpha 0xFF; the palette/RGB bytes keep the BGR order.
fit::Bytes golden32(int width, int height, BYTE alpha) {
  fit::Bytes out;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const fit::Pixel pixel = fit::pattern(y, x);
      out.insert(out.end(), {pixel.blue, pixel.green, pixel.red, alpha});
    }
  }
  return out;
}

// Golden scanline array for the same dib at 24bpp: no alpha channel (§2.4).
// Row padding follows the padded-calc_line internal contract: all_scanlines() reads
// fit::calc_line(width, 24) bytes per row, and the pitch tail is zero-filled.
fit::Bytes golden24(int width, int height) {
  const unsigned line = fit::calc_line(width, 24);
  fit::Bytes out;
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const fit::Pixel pixel = fit::pattern(y, x);
      out.insert(out.end(), {pixel.blue, pixel.green, pixel.red});
    }
    out.resize(out.size() + line - static_cast<unsigned>(width) * 3, 0);
  }
  return out;
}

// §2.4: per-pixel alpha and the three color channels fed to PreMultiplyWithAlpha,
// with the contract's exact integer results: c = (a * c + 127) / 255, a == 0 forces
// black and a == 255 is the identity.
struct PremultiplyCase {
  BYTE alpha;
  BYTE channel;
  BYTE result;
};

constexpr PremultiplyCase kPremultiplyCases[] = {
    {0x00, 200, 0},  // a == 0 => black, whatever the channel was
    {0x00, 0, 0},
    {0xFF, 200, 200},  // a == 255 => unchanged
    {0xFF, 1, 1},
    {0x01, 0xFF, 1},   // (255 + 127) / 255
    {0x01, 0x01, 0},   // (1 + 127) / 255
    {0x80, 0x01, 1},   // (128 + 127) / 255
    {0x7F, 0x7F, 63},  // (16129 + 127) / 255
    {0x80, 0xFF, 128},
    {0xFE, 0xFF, 254},
    {0xC8, 0x80, 100},
};

}  // namespace

TEST_CASE("[convert] ConvertTo32Bits: 24 -> 32 sets alpha 0xFF and keeps BGR per §2.4") {
  const unsigned line24 = fit::calc_line(kWidth, 24);
  const fit::Bytes buffer = fit::make_buffer(kWidth, kHeight, 24, line24);
  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, line24, 24);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  const fit::Bitmap converted = fit::Bitmap(FreeImage_ConvertTo32Bits(dib.get()));
  REQUIRE_MESSAGE(converted.get() != nullptr, "ConvertTo32Bits must succeed on a 24bpp dib (§2.4)");
  CHECK(converted.get() != dib.get());
  CHECK(FreeImage_GetWidth(converted.get()) == static_cast<unsigned>(kWidth));
  CHECK(FreeImage_GetHeight(converted.get()) == static_cast<unsigned>(kHeight));
  CHECK(FreeImage_GetBPP(converted.get()) == 32u);
  REQUIRE(FreeImage_GetLine(converted.get()) == static_cast<unsigned>(kWidth) * 4u);
  REQUIRE_BYTES_EQ(fit::all_scanlines(converted), golden32(kWidth, kHeight, 0xFF));
}

TEST_CASE("[convert] ConvertTo32Bits: a 32bpp source is a clone per §2.4") {
  const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  const fit::Bitmap clone = fit::Bitmap(FreeImage_ConvertTo32Bits(dib.get()));
  REQUIRE_MESSAGE(clone.get() != nullptr, "ConvertTo32Bits must succeed on a 32bpp dib (§2.4)");
  // A clone is a new dib the caller unloads separately.
  CHECK(clone.get() != dib.get());
  CHECK(FreeImage_GetBPP(clone.get()) == 32u);
  CHECK(FreeImage_GetPitch(clone.get()) == FreeImage_GetPitch(dib.get()));
  REQUIRE_BYTES_EQ(fit::all_scanlines(clone), fit::all_scanlines(dib));
  // The same clause asks for metadata to be cloned. It is not observable through the
  // frozen seam: SetMetadata is dropped from the shipped header (API.md §3), so only
  // a codec load can attach a tag. Covered from the codec side instead.
}

TEST_CASE("[convert] ConvertTo24Bits discards alpha per §2.4 (Scraper 32 -> 24 path)") {
  const unsigned line32 = fit::calc_line(kWidth, 32);
  // The fixture's alpha varies per row, so a leaked alpha channel would be visible.
  const fit::Bytes buffer = fit::make_buffer(kWidth, kHeight, 32, line32);
  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, line32, 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  const fit::Bitmap converted = fit::Bitmap(FreeImage_ConvertTo24Bits(dib.get()));
  REQUIRE_MESSAGE(converted.get() != nullptr, "ConvertTo24Bits must succeed on a 32bpp dib (§2.4)");
  CHECK(FreeImage_GetWidth(converted.get()) == static_cast<unsigned>(kWidth));
  CHECK(FreeImage_GetHeight(converted.get()) == static_cast<unsigned>(kHeight));
  CHECK(FreeImage_GetBPP(converted.get()) == 24u);
  REQUIRE(FreeImage_GetLine(converted.get()) == fit::calc_line(kWidth, 24));
  REQUIRE_BYTES_EQ(fit::all_scanlines(converted), golden24(kWidth, kHeight));
}

TEST_CASE("[convert] ConvertTo24Bits clones a 24bpp source per §2.4") {
  const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 24);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  const fit::Bitmap clone = fit::Bitmap(FreeImage_ConvertTo24Bits(dib.get()));
  REQUIRE_MESSAGE(clone.get() != nullptr, "ConvertTo24Bits must succeed on a 24bpp dib (§2.4)");
  CHECK(clone.get() != dib.get());
  CHECK(FreeImage_GetBPP(clone.get()) == 24u);
  REQUIRE_BYTES_EQ(fit::all_scanlines(clone), fit::all_scanlines(dib));
}

TEST_CASE("[convert] ConvertTo32Bits / ConvertTo24Bits reject a null dib per §2.4") {
  CHECK(FreeImage_ConvertTo32Bits(nullptr) == nullptr);
  CHECK(FreeImage_ConvertTo24Bits(nullptr) == nullptr);
}

TEST_CASE("[convert] PreMultiplyWithAlpha exact integer math per §2.4") {
  // Independently derived from the contract formula before the library is consulted.
  for (const PremultiplyCase& c : kPremultiplyCases) {
    INFO("alpha " << static_cast<int>(c.alpha) << " channel " << static_cast<int>(c.channel));
    CHECK(fit::premultiplied(c.channel, c.alpha) == c.result);
  }

  const unsigned width = static_cast<unsigned>(sizeof(kPremultiplyCases) / sizeof(kPremultiplyCases[0]));
  const unsigned pitch = width * 4;
  fit::Bytes buffer(static_cast<std::size_t>(pitch), 0);
  fit::Bytes expected(static_cast<std::size_t>(pitch), 0);
  for (unsigned x = 0; x < width; ++x) {
    const PremultiplyCase& c = kPremultiplyCases[x];
    // Every color channel carries the same input value, so the three channel writes
    // are all exercised, and the alpha byte must survive untouched.
    const fit::Pixel source{c.channel, c.channel, c.channel, c.alpha};
    source.store(buffer.data() + x * 4, 32);
    const fit::Pixel result{c.result, c.result, c.result, c.alpha};
    result.store(expected.data() + x * 4, 32);
  }

  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, static_cast<int>(width), 1, pitch, 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
  REQUIRE(FreeImage_PreMultiplyWithAlpha(dib.get()) == TRUE);
  REQUIRE_BYTES_EQ(fit::scanline_of(dib, 0), expected);
}

TEST_CASE("[convert] PreMultiplyWithAlpha is a FALSE no-op below 32bpp and on null per §2.4") {
  CHECK(FreeImage_PreMultiplyWithAlpha(nullptr) == FALSE);

  const unsigned line24 = fit::calc_line(kWidth, 24);
  const fit::Bytes buffer = fit::make_buffer(kWidth, kHeight, 24, line24);
  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, line24, 24);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  const fit::Bytes before = fit::all_scanlines(dib);
  CHECK(FreeImage_PreMultiplyWithAlpha(dib.get()) == FALSE);
  REQUIRE_BYTES_EQ(fit::all_scanlines(dib), before);
}

TEST_CASE("[pixel] GetPixelColor reads scanlines, y measured from the visual bottom per §2.5 + §1.1") {
  SUBCASE("32bpp: the alpha byte is returned in rgbReserved") {
    const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 32);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    for (unsigned y = 0; y < static_cast<unsigned>(kHeight); ++y) {
      for (unsigned x = 0; x < static_cast<unsigned>(kWidth); ++x) {
        RGBQUAD value{};
        INFO("x " << x << ", y " << y);
        REQUIRE(FreeImage_GetPixelColor(dib.get(), x, y, &value) == TRUE);
        const fit::Pixel pixel = fit::pattern(static_cast<int>(y), static_cast<int>(x));
        CHECK(value.rgbBlue == pixel.blue);
        CHECK(value.rgbGreen == pixel.green);
        CHECK(value.rgbRed == pixel.red);
        CHECK(value.rgbReserved == pixel.alpha);
      }
    }
  }

  SUBCASE("a top-down source buffer puts the visual bottom row at y == 0") {
    // Declaring the fixture top-down makes scanline y the fixture's last-minus-y row,
    // so y == 0 must read the visually bottom row of the image (§1.1).
    const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 32, TRUE);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    RGBQUAD value{};
    REQUIRE(FreeImage_GetPixelColor(dib.get(), 1, 0, &value) == TRUE);
    const fit::Pixel bottom = fit::pattern(kHeight - 1, 1);
    CHECK(value.rgbBlue == bottom.blue);
    CHECK(value.rgbGreen == bottom.green);
    CHECK(value.rgbRed == bottom.red);
    REQUIRE(FreeImage_GetPixelColor(dib.get(), 1, static_cast<unsigned>(kHeight) - 1, &value) == TRUE);
    const fit::Pixel top = fit::pattern(0, 1);
    CHECK(value.rgbBlue == top.blue);
    CHECK(value.rgbGreen == top.green);
    CHECK(value.rgbRed == top.red);
  }

  SUBCASE("24bpp: rgbReserved is 0") {
    const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 24);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    for (unsigned y = 0; y < static_cast<unsigned>(kHeight); ++y) {
      for (unsigned x = 0; x < static_cast<unsigned>(kWidth); ++x) {
        RGBQUAD value{};
        INFO("x " << x << ", y " << y);
        REQUIRE(FreeImage_GetPixelColor(dib.get(), x, y, &value) == TRUE);
        const fit::Pixel pixel = fit::pattern(static_cast<int>(y), static_cast<int>(x));
        CHECK(value.rgbBlue == pixel.blue);
        CHECK(value.rgbGreen == pixel.green);
        CHECK(value.rgbRed == pixel.red);
        CHECK(value.rgbReserved == 0);
      }
    }
  }

  SUBCASE("out-of-range coordinates return FALSE and leave the value untouched") {
    const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 32);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    const RGBQUAD sentinel{0x11, 0x22, 0x33, 0x44};
    const auto as_bytes = [](const RGBQUAD& q) { return fit::Bytes{q.rgbBlue, q.rgbGreen, q.rgbRed, q.rgbReserved}; };

    RGBQUAD value = sentinel;
    CHECK(FreeImage_GetPixelColor(dib.get(), static_cast<unsigned>(kWidth), 0, &value) == FALSE);
    CHECK_BYTES_EQ(as_bytes(value), as_bytes(sentinel));

    value = sentinel;
    CHECK(FreeImage_GetPixelColor(dib.get(), 0, static_cast<unsigned>(kHeight), &value) == FALSE);
    CHECK_BYTES_EQ(as_bytes(value), as_bytes(sentinel));
  }
}
