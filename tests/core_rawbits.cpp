// ConvertToRawBits / ConvertFromRawBits (§1.3, §2.6) in the exact shapes ES-DE calls
// them from: GIFAnimComponent's two contradictory mask orders, MiximageGenerator's
// topdown=1 export and its default-argument import, and a caller-chosen pitch.
#include "helpers.h"

namespace {

constexpr int kWidth = 3;
constexpr int kHeight = 3;

// What convertCImgToBGRA does to a top-down CImg before ES-DE hands the vector back to
// ConvertFromRawBits: it emits the rows bottom-up (§1.2).
fit::Bytes rows_reversed(const fit::Bytes& buffer, unsigned pitch, int height) {
  fit::Bytes out;
  for (int row = height - 1; row >= 0; --row) {
    const fit::Bytes source = fit::row_of(buffer, pitch, row);
    out.insert(out.end(), source.begin(), source.end());
  }
  return out;
}

}  // namespace

TEST_CASE("[rawbits] equal-bpp masks are ignored byte-identically per §1.3") {
  const unsigned pitch = fit::calc_line(kWidth, 32);
  const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
  REQUIRE(FreeImage_GetPitch(dib.get()) == pitch);

  // GIFAnimComponent.cpp:227 / MiximageGenerator.cpp:404 argument order.
  const fit::Bytes bgr = fit::export_bgr(dib, pitch, 32, TRUE);
  // GIFAnimComponent.cpp:547 argument order, which ES-DE expects to mean the same.
  const fit::Bytes rgb = fit::export_rgb(dib, pitch, 32, TRUE);
  // A third permutation, in case an implementation special-cases only these two.
  const fit::Bytes mixed = fit::to_raw_bits(dib, pitch, 32, FI_RGBA_GREEN, FI_RGBA_BLUE, FI_RGBA_RED, TRUE);

  // §1.3: src bpp == dst bpp for 24/32 is a straight memcpy of the scanline; the mask
  // arguments are never consulted. All three buffers must be identical, and they must
  // be the golden BGRA bytes (a no-op export would otherwise pass the equality checks).
  CHECK_BYTES_EQ(rgb, bgr);
  CHECK_BYTES_EQ(mixed, bgr);
  REQUIRE_BYTES_EQ(
      bgr,
      fit::golden_raw_bits(fit::make_buffer(kWidth, kHeight, 32, pitch), kWidth, kHeight, pitch, pitch, 32, TRUE));
}

TEST_CASE("[rawbits] equal-bpp masks are ignored at 24bpp too per §1.3") {
  const unsigned pitch = fit::calc_line(kWidth, 24);
  const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 24);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  const fit::Bytes bgr = fit::export_bgr(dib, pitch, 24, FALSE);
  const fit::Bytes rgb = fit::export_rgb(dib, pitch, 24, FALSE);
  CHECK_BYTES_EQ(rgb, bgr);
  REQUIRE_BYTES_EQ(
      bgr,
      fit::golden_raw_bits(fit::make_buffer(kWidth, kHeight, 24, pitch), kWidth, kHeight, pitch, pitch, 24, FALSE));
}

TEST_CASE("[rawbits] topdown reorders rows and never the bytes per §1.3 + §1.1") {
  const unsigned pitch = fit::calc_line(kWidth, 32);
  const fit::Bytes buffer = fit::make_buffer(kWidth, kHeight, 32, pitch);
  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, pitch, 32, FALSE);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  SUBCASE("topdown == FALSE: dst row i is scanline i, so the buffer is bottom-up") {
    const fit::Bytes out = fit::export_bgr(dib, pitch, 32, FALSE);
    REQUIRE_BYTES_EQ(out, fit::golden_raw_bits(buffer, kWidth, kHeight, pitch, pitch, 32, FALSE));
  }

  SUBCASE("topdown == TRUE: dst row 0 is GetScanLine(h-1), the visual top") {
    const fit::Bytes out = fit::export_bgr(dib, pitch, 32, TRUE);
    REQUIRE_BYTES_EQ(out, fit::golden_raw_bits(buffer, kWidth, kHeight, pitch, pitch, 32, TRUE));
  }

  // Whichever row order is chosen, a pixel stays B, G, R, A in memory (§1.1 BGR byte
  // order). The fixture makes all four channels distinct, so a byte shuffle inside a
  // pixel would change the golden comparison above; pin the first pixel explicitly.
  const fit::Bytes out = fit::export_bgr(dib, pitch, 32, TRUE);
  REQUIRE(out.size() >= 4);
  const fit::Pixel visual_top = fit::pattern(kHeight - 1, 0);
  CHECK(out[FI_RGBA_BLUE] == visual_top.blue);
  CHECK(out[FI_RGBA_GREEN] == visual_top.green);
  CHECK(out[FI_RGBA_RED] == visual_top.red);
  CHECK(out[FI_RGBA_ALPHA] == visual_top.alpha);
}

TEST_CASE("[rawbits] pitch larger than the line leaves the trailing bytes untouched per §1.3") {
  SUBCASE("24bpp, odd width: line 16 for 15 pixel bytes, exported at pitch 20") {
    constexpr int width = 5;
    constexpr unsigned line = 16;
    constexpr unsigned pitch = 20;
    const fit::Bytes buffer = fit::make_buffer(width, 2, 24, line);
    const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, width, 2, line, 24);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    REQUIRE(FreeImage_GetLine(dib.get()) == line);

    const fit::Bytes out = fit::export_bgr(dib, pitch, 24, FALSE);
    // §1.3: rows are written with FreeImage_GetLine bytes and dst advances by pitch,
    // so [line, pitch) is left exactly as the caller wrote it -- here the poison.
    for (int row = 0; row < 2; ++row) {
      const fit::Bytes tail(
          out.begin() + static_cast<std::ptrdiff_t>(row * pitch + line),
          out.begin() + static_cast<std::ptrdiff_t>(row * pitch + pitch));
      CHECK_BYTES_EQ(tail, fit::Bytes(pitch - line, fit::kPoison));
    }
    REQUIRE_BYTES_EQ(out, fit::golden_raw_bits(buffer, width, 2, line, pitch, 24, FALSE));
  }

  SUBCASE("32bpp: line 8 exported at pitch 16") {
    constexpr unsigned line = 8;
    constexpr unsigned pitch = 16;
    const fit::Bytes buffer = fit::make_buffer(2, 2, 32, line);
    const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, 2, 2, line, 32);
    REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

    const fit::Bytes out = fit::export_bgr(dib, pitch, 32, FALSE);
    REQUIRE_BYTES_EQ(out, fit::golden_raw_bits(buffer, 2, 2, line, pitch, 32, FALSE));
  }
}

TEST_CASE("[rawbits] ConvertFromRawBits copies the caller buffer per §1.3") {
  fit::Bytes buffer = fit::make_buffer(kWidth, kHeight, 32, fit::calc_line(kWidth, 32));
  const fit::Bytes golden = fit::golden_scanlines(buffer, kWidth, kHeight, fit::calc_line(kWidth, 32), 32, FALSE);
  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, kWidth, kHeight, fit::calc_line(kWidth, 32), 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  // §1.3: "classic always copies; the returned dib owns its memory".
  std::fill(buffer.begin(), buffer.end(), 0x00);
  REQUIRE_BYTES_EQ(fit::all_scanlines(dib), golden);
}

TEST_CASE("[rawbits] MiximageGenerator composition is a flip unless the rows come back inverted per §1.2 + §1.3") {
  const unsigned pitch = static_cast<unsigned>(kWidth) * 4;
  // The canvas vector as convertCImgToBGRA emits it: bottom-up, row 0 the visual bottom.
  const fit::Bytes canvas = fit::make_buffer(kWidth, kHeight, 32, pitch);
  const fit::Bitmap dib = fit::bitmap_from_buffer(canvas, kWidth, kHeight, pitch, 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  // MiximageGenerator.cpp:761 import + :404 export, the two raw-bits calls ES-DE makes
  // around its CImg work.
  const fit::Bytes top_down = fit::export_bgr(dib, pitch, 32, TRUE);
  REQUIRE_BYTES_EQ(top_down, fit::golden_raw_bits(canvas, kWidth, kHeight, pitch, pitch, 32, TRUE));

  SUBCASE("importing that buffer with the default topdown == FALSE flips it") {
    const fit::Bitmap flipped = fit::bitmap_from_buffer(top_down, kWidth, kHeight, pitch, 32, FALSE);
    REQUIRE_MESSAGE(flipped.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    for (int y = 0; y < kHeight; ++y) {
      CHECK_BYTES_EQ(fit::scanline_of(flipped, y), fit::scanline_of(dib, kHeight - 1 - y));
    }
  }

  SUBCASE("importing it with topdown == TRUE is the identity dib") {
    const fit::Bitmap same = fit::bitmap_from_buffer(top_down, kWidth, kHeight, pitch, 32, TRUE);
    REQUIRE_MESSAGE(same.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    REQUIRE_BYTES_EQ(fit::all_scanlines(same), fit::all_scanlines(dib));
  }

  SUBCASE("ES-DE's double flip cancels because convertCImgToBGRA re-inverts the rows") {
    // CImgUtil.cpp:31-42 writes the vector bottom-up, which is the second flip; the
    // default-argument ConvertFromRawBits then reproduces the original dib verbatim.
    const fit::Bytes canvas_again = rows_reversed(top_down, pitch, kHeight);
    const fit::Bitmap round_trip = fit::bitmap_from_buffer(canvas_again, kWidth, kHeight, pitch, 32, FALSE);
    REQUIRE_MESSAGE(round_trip.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
    REQUIRE_BYTES_EQ(fit::all_scanlines(round_trip), fit::all_scanlines(dib));
  }
}

TEST_CASE("[rawbits] a caller-chosen pitch round-trips at width * 4 per §1.3") {
  constexpr int width = 5;
  constexpr unsigned line = 16;
  constexpr unsigned pitch = static_cast<unsigned>(width) * 4;  // 20 > line
  const fit::Bytes buffer = fit::make_buffer(width, 2, 24, line);
  const fit::Bitmap dib = fit::bitmap_from_buffer(buffer, width, 2, line, 24);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  const fit::Bytes exported = fit::export_bgr(dib, pitch, 24, FALSE);
  // Re-importing at the same stride touches only the line bytes of each stride row, so
  // the untouched gap can never reach the dib: the scanline array is unchanged.
  const fit::Bitmap reimported = fit::bitmap_from_buffer(exported, width, 2, pitch, 24);
  REQUIRE_MESSAGE(reimported.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
  REQUIRE_BYTES_EQ(fit::all_scanlines(reimported), fit::all_scanlines(dib));
}

TEST_CASE("[rawbits] ConvertToRawBits no-ops on a null destination or dib per §2.6") {
  const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");
  // §2.6: classic guards FreeImage_HasPixels(dib) && bits != NULL, so both of these
  // return without writing anything; the assertion is that they do not crash.
  FreeImage_ConvertToRawBits(
      nullptr,
      dib.get(),
      kWidth * 4,
      32,
      FI_RGBA_RED_MASK,
      FI_RGBA_GREEN_MASK,
      FI_RGBA_BLUE_MASK,
      FALSE);
  FreeImage_ConvertToRawBits(
      nullptr,
      nullptr,
      kWidth * 4,
      32,
      FI_RGBA_RED_MASK,
      FI_RGBA_GREEN_MASK,
      FI_RGBA_BLUE_MASK,
      FALSE);
  CHECK(true);
}
