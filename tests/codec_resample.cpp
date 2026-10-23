// FreeImage_Rescale (FILTER_LANCZOS3) semantics, design §2.8. Inputs are built
// through the public ConvertFromRawBits seam; the independent Lanczos3 reference
// lives in helpers_codecs so the tolerance check cannot inherit an implementation
// bug. In the red phase both ConvertFromRawBits and Rescale return nullptr, so
// every "produced a rescaled image" assertion is a deterministic failure and every
// pixel comparison is guarded behind a non-null check (no crashes).
#include <FreeImage.h>
#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <vector>

#include "helpers_codecs.h"

namespace {

using fic::Image;
using fic::Rgba;

constexpr int kPitch24(int w) { return ((w * 3 + 3) / 4) * 4; }

// Build a bottom-up DIB whose visual content matches the top-down reference.
// topdown=TRUE tells ConvertFromRawBits the source rows are top-first.
FIBITMAP* MakeDib(const Image& img, unsigned bpp) {
  if (bpp == 24) {
    const int pitch = kPitch24(img.w);
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

// Read a pixel addressing the reference top-down row `ry` (DIB is bottom-up).
Rgba ReadPx(FIBITMAP* dib, int x, int ry) {
  RGBQUAD q{0, 0, 0, 0};
  const unsigned h = dib ? FreeImage_GetHeight(dib) : 0;
  FreeImage_GetPixelColor(dib, static_cast<unsigned>(x), h - 1 - static_cast<unsigned>(ry), &q);
  return Rgba{q.rgbRed, q.rgbGreen, q.rgbBlue, q.rgbReserved};
}

// Classic GetPixelColor reports rgbReserved = 0 for a 24bpp dib (PixelAccess.cpp:95-101),
// so alpha is unobservable there and must not count as a difference; 32bpp still compares it.
bool PixelsEq(FIBITMAP* dib, Rgba got, Rgba want) {
  return got.r == want.r && got.g == want.g && got.b == want.b && (FreeImage_GetBPP(dib) == 24 || got.a == want.a);
}

int ChannelError(FIBITMAP* dib, Rgba got, Rgba want) {
  int error = std::max(std::abs(int(got.r) - int(want.r)), std::abs(int(got.g) - int(want.g)));
  error = std::max(error, std::abs(int(got.b) - int(want.b)));
  if (FreeImage_GetBPP(dib) != 24) {
    error = std::max(error, std::abs(int(got.a) - int(want.a)));
  }
  return error;
}

// design §2.8 leaves the separable pass order to the implementation, and classic
// filters horizontally first only when the width does not grow. The shipped
// reference (fic::Lanczos3Reference) is the horizontal-first order; the port below is
// the same §2.8 kernel composed in the other order, which is what a widening geometry
// selects. Golden and implementation therefore agree on the order without the test
// inheriting it from the reference it happens to be written in.
double LanczosSinc(double x) {
  if (x == 0.0) {
    return 1.0;
  }
  const double px = std::numbers::pi * x;
  return std::sin(px) / px;
}

double L3(double x) { return LanczosSinc(x) * LanczosSinc(x / 3.0); }

std::vector<float> Axis(const std::vector<float>& src, int src_n, int dst_n) {
  std::vector<float> dst(static_cast<size_t>(dst_n));
  const double scale = static_cast<double>(dst_n) / src_n;
  const double width = scale < 1.0 ? 3.0 / scale : 3.0;
  const double fscale = scale < 1.0 ? scale : 1.0;
  for (int u = 0; u < dst_n; ++u) {
    const double center = u / scale + 0.5 / scale;
    const int i_left = std::max(0, static_cast<int>(center - width + 0.5));
    const int i_right = std::min(static_cast<int>(center + width + 0.5), src_n);
    double sum = 0.0;
    double norm = 0.0;
    for (int i = i_left; i < i_right; ++i) {
      const double w = fscale * L3(fscale * (i + 0.5 - center));
      sum += w * src[i];
      norm += w;
    }
    if (norm > 0.0 && norm != 1.0) {
      sum /= norm;
    }
    dst[u] = static_cast<float>(sum);
  }
  return dst;
}

double ClampByte(double v) { return v < 0.0 ? 0.0 : (v > 255.0 ? 255.0 : v); }

Image Lanczos3VerticalFirst(const Image& src, int dst_w, int dst_h) {
  auto channel = [&](int ch) {
    std::vector<float> scaled(static_cast<size_t>(src.w) * src.h);
    for (int y = 0; y < src.h; ++y) {
      for (int x = 0; x < src.w; ++x) {
        const Rgba p = src.at(x, y);
        scaled[static_cast<size_t>(y) * src.w + x] = static_cast<float>(
            ch == 0   ? p.r
            : ch == 1 ? p.g
            : ch == 2 ? p.b
                      : p.a);
      }
    }
    // Vertical pass first: src.h -> dst_h, keeping src.w columns.
    std::vector<float> intermediate(static_cast<size_t>(src.w) * dst_h);
    for (int x = 0; x < src.w; ++x) {
      std::vector<float> column(static_cast<size_t>(src.h));
      for (int y = 0; y < src.h; ++y) {
        column[y] = scaled[static_cast<size_t>(y) * src.w + x];
      }
      const std::vector<float> filtered = Axis(column, src.h, dst_h);
      std::copy(filtered.begin(), filtered.end(), intermediate.begin() + static_cast<size_t>(x) * dst_h);
    }
    // Horizontal pass over the intermediate: src.w -> dst_w for each of dst_h rows.
    std::vector<float> out(static_cast<size_t>(dst_w) * dst_h);
    for (int y = 0; y < dst_h; ++y) {
      std::vector<float> row(static_cast<size_t>(src.w));
      for (int x = 0; x < src.w; ++x) {
        row[x] = intermediate[static_cast<size_t>(x) * dst_h + y];
      }
      const std::vector<float> filtered = Axis(row, src.w, dst_w);
      std::copy(filtered.begin(), filtered.end(), out.begin() + static_cast<size_t>(y) * dst_w);
    }
    return out;
  };
  const std::vector<float> r = channel(0);
  const std::vector<float> g = channel(1);
  const std::vector<float> b = channel(2);
  const std::vector<float> a = channel(3);
  Image out;
  out.w = dst_w;
  out.h = dst_h;
  out.px.resize(static_cast<size_t>(dst_w) * dst_h);
  for (size_t i = 0; i < out.px.size(); ++i) {
    out.px[i] = Rgba{
        static_cast<uint8_t>(std::lround(ClampByte(r[i]))),
        static_cast<uint8_t>(std::lround(ClampByte(g[i]))),
        static_cast<uint8_t>(std::lround(ClampByte(b[i]))),
        static_cast<uint8_t>(std::lround(ClampByte(a[i])))};
  }
  return out;
}

}  // namespace

// --- exact destination geometry ---------------------------------------------

TEST_CASE("[resample §2.8] Lanczos3 produces exact dst dimensions incl. 1px edges") {
  Image src = fic::Pattern(16, 10);
  FIBITMAP* dib = MakeDib(src, 24);
  const int dsts[][2] = {{4, 3}, {1, 1}, {1, 10}, {16, 1}, {15, 9}, {32, 20}};
  for (const auto& d : dsts) {
    FIBITMAP* out = FreeImage_Rescale(dib, d[0], d[1], FILTER_LANCZOS3);
    CHECK(out != nullptr);
    if (out) {
      CHECK(FreeImage_GetWidth(out) == static_cast<unsigned>(d[0]));
      CHECK(FreeImage_GetHeight(out) == static_cast<unsigned>(d[1]));
      FreeImage_Unload(out);
    }
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- channel-depth preservation (24->24, 32->32) ----------------------------

TEST_CASE("[resample §2.8] 24bpp scales to 24bpp, 32bpp scales to 32bpp") {
  Image rgb = fic::Pattern(12, 8);
  for (auto& p : rgb.px) {
    p.a = 255;
  }
  FIBITMAP* d24 = MakeDib(rgb, 24);
  FIBITMAP* o24 = FreeImage_Rescale(d24, 6, 4, FILTER_LANCZOS3);
  CHECK(o24 != nullptr);
  if (o24) {
    CHECK(FreeImage_GetBPP(o24) == 24);
    FreeImage_Unload(o24);
  }
  if (d24) {
    FreeImage_Unload(d24);
  }

  Image rgba = fic::Pattern(12, 8);  // carries alpha
  FIBITMAP* d32 = MakeDib(rgba, 32);
  FIBITMAP* o32 = FreeImage_Rescale(d32, 6, 4, FILTER_LANCZOS3);
  CHECK(o32 != nullptr);
  if (o32) {
    CHECK(FreeImage_GetBPP(o32) == 32);
    FreeImage_Unload(o32);
  }
  if (d32) {
    FreeImage_Unload(d32);
  }
}

// --- same-size is an exact copy ---------------------------------------------

TEST_CASE("[resample §2.8] rescale to the same size is a lossless copy") {
  Image img = fic::Pattern(9, 7);
  FIBITMAP* dib = MakeDib(img, 24);
  FIBITMAP* out = FreeImage_Rescale(dib, 9, 7, FILTER_LANCZOS3);
  CHECK(out != nullptr);
  if (out) {
    CHECK(FreeImage_GetBPP(out) == 24);
    bool all = true;
    for (int y = 0; y < img.h && all; ++y) {
      for (int x = 0; x < img.w; ++x) {
        if (!PixelsEq(out, ReadPx(out, x, y), img.at(x, y))) {
          all = false;
          break;
        }
      }
    }
    CHECK(all);
    FreeImage_Unload(out);
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- constant color is exactly preserved ------------------------------------

TEST_CASE("[resample §2.8] a solid color rescales to the same solid color") {
  const Rgba fill{40, 120, 200, 255};
  Image img = fic::Solid(20, 20, fill);
  FIBITMAP* dib = MakeDib(img, 24);
  FIBITMAP* out = FreeImage_Rescale(dib, 7, 5, FILTER_LANCZOS3);
  CHECK(out != nullptr);
  if (out) {
    bool all = true;
    for (int y = 0; y < 5 && all; ++y) {
      for (int x = 0; x < 7; ++x) {
        if (!PixelsEq(out, ReadPx(out, x, y), fill)) {
          all = false;
          break;
        }
      }
    }
    CHECK(all);
    FreeImage_Unload(out);
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- determinism ------------------------------------------------------------

TEST_CASE("[resample §2.8] Lanczos3 is deterministic across calls") {
  Image img = fic::Pattern(24, 16);
  FIBITMAP* dib = MakeDib(img, 32);
  FIBITMAP* a = FreeImage_Rescale(dib, 8, 6, FILTER_LANCZOS3);
  FIBITMAP* b = FreeImage_Rescale(dib, 8, 6, FILTER_LANCZOS3);
  CHECK(a != nullptr);
  CHECK(b != nullptr);
  if (a && b) {
    bool identical = true;
    for (int y = 0; y < 6 && identical; ++y) {
      for (int x = 0; x < 8; ++x) {
        if (!(ReadPx(a, x, y) == ReadPx(b, x, y))) {
          identical = false;
          break;
        }
      }
    }
    CHECK(identical);
    FreeImage_Unload(a);
    FreeImage_Unload(b);
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- numeric agreement with the independent reference (<=2/255) -------------

TEST_CASE("[resample §2.8] output matches an independent Lanczos3 within 2/255") {
  Image src = fic::Pattern(24, 18);
  for (auto& p : src.px) {
    p.a = 255;
  }
  Image ref = fic::Lanczos3Reference(src, 9, 7);
  FIBITMAP* dib = MakeDib(src, 24);
  FIBITMAP* out = FreeImage_Rescale(dib, 9, 7, FILTER_LANCZOS3);
  CHECK(out != nullptr);
  if (out) {
    CHECK(FreeImage_GetWidth(out) == 9u);
    CHECK(FreeImage_GetHeight(out) == 7u);
    int over = 0;
    int max_err = 0;
    for (int y = 0; y < 7; ++y) {
      for (int x = 0; x < 9; ++x) {
        Rgba got = ReadPx(out, x, y);
        Rgba want = ref.at(x, y);
        const int e = ChannelError(out, got, want);
        max_err = std::max(max_err, e);
        if (e > 2) {
          ++over;
        }
      }
    }
    CHECK(max_err <= 2);
    CHECK(over == 0);
    FreeImage_Unload(out);
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- enlargement pins the other pass order (design §2.8) --------------------

// dst_width > src_width, so the implementation's classic "xy versus yx" rule
// (horizontal first only when the width does not grow) selects the vertical-first
// composition; the golden below is that order, so a horizontally-first
// implementation would be compared against the wrong separable order.
TEST_CASE("[resample §2.8] a widening rescale matches the vertical-first reference within 2/255") {
  Image src = fic::Pattern(9, 24);
  for (auto& p : src.px) {
    p.a = 255;
  }
  Image ref = Lanczos3VerticalFirst(src, 24, 9);
  FIBITMAP* dib = MakeDib(src, 24);
  FIBITMAP* out = FreeImage_Rescale(dib, 24, 9, FILTER_LANCZOS3);
  CHECK(out != nullptr);
  if (out) {
    CHECK(FreeImage_GetWidth(out) == 24u);
    CHECK(FreeImage_GetHeight(out) == 9u);
    int over = 0;
    int max_err = 0;
    for (int y = 0; y < 9; ++y) {
      for (int x = 0; x < 24; ++x) {
        const Rgba got = ReadPx(out, x, y);
        const Rgba want = ref.at(x, y);
        const int e = ChannelError(out, got, want);
        max_err = std::max(max_err, e);
        if (e > 2) {
          ++over;
        }
      }
    }
    CHECK(max_err <= 2);
    CHECK(over == 0);
    FreeImage_Unload(out);
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}

// --- alpha is an independent 4th channel ------------------------------------

TEST_CASE("[resample §2.8] alpha is resampled as its own channel") {
  Image src{16, 16, std::vector<Rgba>(16 * 16, Rgba{255, 0, 0, 0})};
  // Left half opaque, right half transparent: alpha gradient must be preserved.
  for (int y = 0; y < 16; ++y) {
    for (int x = 0; x < 16; ++x) {
      src.px[static_cast<size_t>(y) * 16 + x] = Rgba{255, 0, 0, static_cast<uint8_t>(x < 8 ? 255 : 0)};
    }
  }
  FIBITMAP* dib = MakeDib(src, 32);
  FIBITMAP* out = FreeImage_Rescale(dib, 8, 8, FILTER_LANCZOS3);
  CHECK(out != nullptr);
  if (out) {
    // Interior of the opaque half stays fully opaque; interior of the transparent
    // half stays fully transparent; the seam is free to ramp.
    Rgba left = ReadPx(out, 1, 4);
    Rgba right = ReadPx(out, 6, 4);
    CHECK(left.a >= 200);
    CHECK(right.a <= 55);
    CHECK(ReadPx(out, 1, 4).r > 150);
    FreeImage_Unload(out);
  }
  if (dib) {
    FreeImage_Unload(dib);
  }
}
