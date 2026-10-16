// Conversion, raw-bits and pixel access. The algorithms reproduce classic FreeImage
// 3.18 semantics as pinned by the contract: §1.1 (bottom-up DIB, 4-byte rows, BGR
// byte order), §1.3 (raw-bits import/export), §2.4 (conversions, premultiply), §2.5
// (pixel access), §2.6 (raw-bits guards). Everything below is Dib-level; the
// handle-shape validation already happened in the marshal TU.
#include "convert.h"

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>

namespace fi {
namespace convert {
namespace {

// The only two 16-bit layouts classic ever writes into a raw buffer (§1.1). A 555
// destination is what an all-zero-or-555 mask set asks for; anything else is 565.
constexpr WORD pack565(BYTE blue, BYTE green, BYTE red) {
  return static_cast<WORD>(
      ((blue >> 3) << FI16_565_BLUE_SHIFT) | ((green >> 2) << FI16_565_GREEN_SHIFT) |
      ((red >> 3) << FI16_565_RED_SHIFT));
}

constexpr WORD pack555(BYTE blue, BYTE green, BYTE red) {
  return static_cast<WORD>(
      ((blue >> 3) << FI16_555_BLUE_SHIFT) | ((green >> 3) << FI16_555_GREEN_SHIFT) |
      ((red >> 3) << FI16_555_RED_SHIFT));
}

constexpr bool masks555(unsigned red, unsigned green, unsigned blue) {
  return red == FI16_555_RED_MASK && green == FI16_555_GREEN_MASK && blue == FI16_555_BLUE_MASK;
}

// Rec.709 luma, rounded to nearest: the value a 24/32bpp source degrades to at 8bpp
// (§1.3).
BYTE luma(BYTE red, BYTE green, BYTE blue) {
  return static_cast<BYTE>(
      0.2126F * static_cast<float>(red) + 0.7152F * static_cast<float>(green) + 0.0722F * static_cast<float>(blue) +
      0.5F);
}

// Indexed -> 32bpp: palette RGB, and alpha from the transparency table only when the
// dib carries transparency - otherwise 0xFF (§2.4).
void indexed_to32(std::span<BYTE> target, std::span<BYTE const> source, Dib const& src) {
  for (int x = 0; x < src.width(); ++x) {
    unsigned const index = palette_index(source, static_cast<unsigned>(x), src.bpp());
    RGBQUAD const color = src.color_at(index);
    target[x * 4 + FI_RGBA_BLUE] = color.rgbBlue;
    target[x * 4 + FI_RGBA_GREEN] = color.rgbGreen;
    target[x * 4 + FI_RGBA_RED] = color.rgbRed;
    target[x * 4 + FI_RGBA_ALPHA] = src.is_transparent() ? src.transparency()[index] : 0xFF;
  }
}

void indexed_to24(std::span<BYTE> target, std::span<BYTE const> source, Dib const& src) {
  for (int x = 0; x < src.width(); ++x) {
    RGBQUAD const color = src.color_at(palette_index(source, static_cast<unsigned>(x), src.bpp()));
    target[x * 3 + FI_RGBA_BLUE] = color.rgbBlue;
    target[x * 3 + FI_RGBA_GREEN] = color.rgbGreen;
    target[x * 3 + FI_RGBA_RED] = color.rgbRed;
  }
}

void indexed_to16(std::span<BYTE> target, std::span<BYTE const> source, Dib const& src, bool rgb565) {
  for (int x = 0; x < src.width(); ++x) {
    RGBQUAD const color = src.color_at(palette_index(source, static_cast<unsigned>(x), src.bpp()));
    store16(
        target.data() + 2 * x,
        rgb565 ? pack565(color.rgbBlue, color.rgbGreen, color.rgbRed)
               : pack555(color.rgbBlue, color.rgbGreen, color.rgbRed));
  }
}

// 16 -> 16/24/32; the destination layout is decided by its masks, as classic's
// CONVERT16TO* lines do (§1.3).
void expand16_row(
    std::span<BYTE> target,
    std::span<BYTE const> source,
    Dib const& src,
    bool target565,
    int target_bpp) {
  for (int x = 0; x < src.width(); ++x) {
    Channels const rgb = expand16(load16(source.data() + 2 * x), src.is_rgb565());
    if (target_bpp == 16) {
      store16(
          target.data() + 2 * x,
          target565 ? pack565(rgb.blue, rgb.green, rgb.red) : pack555(rgb.blue, rgb.green, rgb.red));
    } else if (target_bpp == 24) {
      target[x * 3 + FI_RGBA_BLUE] = rgb.blue;
      target[x * 3 + FI_RGBA_GREEN] = rgb.green;
      target[x * 3 + FI_RGBA_RED] = rgb.red;
    } else {
      target[x * 4 + FI_RGBA_BLUE] = rgb.blue;
      target[x * 4 + FI_RGBA_GREEN] = rgb.green;
      target[x * 4 + FI_RGBA_RED] = rgb.red;
      target[x * 4 + FI_RGBA_ALPHA] = 0xFF;
    }
  }
}

// One row of FreeImage_ConvertToRawBits (§1.3). The equal-bpp case is a straight copy
// of the dib's line bytes and never consults the mask arguments; combinations
// classic's switch has no case for leave the destination row untouched, which is
// reproduced here.
void raw_row(
    std::span<BYTE> target,
    std::span<BYTE const> source,
    Dib const& src,
    int target_bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask) {
  if (src.bpp() == target_bpp) {
    std::memcpy(target.data(), source.data(), target.size());
    return;
  }
  if (src.bpp() == 16 && target_bpp == 16) {
    expand16_row(target, source, src, !masks555(red_mask, green_mask, blue_mask), 16);
    return;
  }
  if (src.is_indexed()) {
    switch (target_bpp) {
      case 8:
        for (int x = 0; x < src.width(); ++x) {
          target[x] = static_cast<BYTE>(palette_index(source, static_cast<unsigned>(x), src.bpp()));
        }
        return;
      case 16:
        indexed_to16(target, source, src, !masks555(red_mask, green_mask, blue_mask));
        return;
      case 24:
        indexed_to24(target, source, src);
        return;
      case 32:
        indexed_to32(target, source, src);
        return;
      default:
        return;
    }
  }
  if (src.bpp() != 24 && src.bpp() != 32) {
    return;
  }
  int const step = src.bpp() / 8;
  switch (target_bpp) {
    case 8:
      for (int x = 0; x < src.width(); ++x) {
        target[x] =
            luma(source[x * step + FI_RGBA_RED], source[x * step + FI_RGBA_GREEN], source[x * step + FI_RGBA_BLUE]);
      }
      return;
    case 16: {
      bool const rgb565 = !masks555(red_mask, green_mask, blue_mask);
      for (int x = 0; x < src.width(); ++x) {
        BYTE const blue = source[x * step + FI_RGBA_BLUE];
        BYTE const green = source[x * step + FI_RGBA_GREEN];
        BYTE const red = source[x * step + FI_RGBA_RED];
        store16(target.data() + 2 * x, rgb565 ? pack565(blue, green, red) : pack555(blue, green, red));
      }
      return;
    }
    case 24:
      for (int x = 0; x < src.width(); ++x) {
        target[x * 3 + FI_RGBA_BLUE] = source[x * step + FI_RGBA_BLUE];
        target[x * 3 + FI_RGBA_GREEN] = source[x * step + FI_RGBA_GREEN];
        target[x * 3 + FI_RGBA_RED] = source[x * step + FI_RGBA_RED];
      }
      return;
    case 32:
      for (int x = 0; x < src.width(); ++x) {
        target[x * 4 + FI_RGBA_BLUE] = source[x * step + FI_RGBA_BLUE];
        target[x * 4 + FI_RGBA_GREEN] = source[x * step + FI_RGBA_GREEN];
        target[x * 4 + FI_RGBA_RED] = source[x * step + FI_RGBA_RED];
        target[x * 4 + FI_RGBA_ALPHA] = 0xFF;
      }
      return;
    default:
      return;
  }
}

// DIB -> DIB conversion rows (§2.4). Same arithmetic as the raw-bits converter plus
// the 16-bit source rows, which the raw-bits matrix has no case for but
// ConvertTo24/32Bits do handle.
void dib_row(std::span<BYTE> target, std::span<BYTE const> source, Dib const& src, int target_bpp) {
  if (src.is_indexed()) {
    if (target_bpp == 32) {
      indexed_to32(target, source, src);
    } else {
      indexed_to24(target, source, src);
    }
    return;
  }
  if (src.bpp() == 16) {
    expand16_row(target, source, src, true, target_bpp);
    return;
  }
  raw_row(target, source, src, target_bpp, 0, 0, 0);
}

}  // namespace

std::unique_ptr<Dib> to_dib(Dib const& source, int target_bpp) {
  if (source.bpp() == target_bpp) {
    return std::make_unique<Dib>(source);
  }
  std::unique_ptr<Dib> target =
      Dib::make(source.width(), source.height(), target_bpp, FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK, FI_RGBA_BLUE_MASK);
  if (target == nullptr) {
    return nullptr;
  }
  target->adopt_tags_from(source);
  for (int y = 0; y < source.height(); ++y) {
    dib_row(target->row(y), source.row(y), source, target_bpp);
  }
  return target;
}

std::unique_ptr<Dib> from_raw_bits(
    BOOL copySource,
    BYTE* bits,
    FREE_IMAGE_TYPE type,
    int width,
    int height,
    int pitch,
    unsigned bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask,
    BOOL topdown) {
  if (bits == nullptr || width <= 0 || height <= 0 || pitch <= 0 || type != FIT_BITMAP) {
    return nullptr;
  }
  if (bpp != 1 && bpp != 4 && bpp != 8 && bpp != 16 && bpp != 24 && bpp != 32) {
    return nullptr;
  }
  // Classic splits copySource TRUE (a copied dib, the ES-DE call shape) from a
  // zero-copy wrapper around the caller's buffer. This DIB model always owns its
  // pixels, so both modes copy and the dib is always independent of `bits` - which is
  // what §1.3 requires of the path ES-DE actually calls.
  (void) copySource;
  std::unique_ptr<Dib> dib = Dib::make(width, height, static_cast<int>(bpp), red_mask, green_mask, blue_mask);
  if (dib == nullptr) {
    return nullptr;
  }
  // §1.3: rows are copied into scanlines 0..h-1 in buffer order, then the dib is
  // flipped iff topdown == TRUE; copying straight into the flipped order is the same
  // result. Only the row's own bytes are read, so a pitch larger than the line never
  // reaches the dib.
  bool const flip = topdown == TRUE;
  for (int y = 0; y < height; ++y) {
    int const scanline = flip ? height - 1 - y : y;
    std::memcpy(dib->row(scanline).data(), bits + static_cast<std::ptrdiff_t>(pitch) * y, dib->line());
  }
  return dib;
}

void to_raw_bits(
    Dib const& source,
    BYTE* bits,
    int pitch,
    unsigned bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask,
    BOOL topdown) {
  if (!source.has_pixels() || bits == nullptr || pitch <= 0 || bpp == 0) {
    return;  // §2.6: classic no-ops without pixels or without a destination
  }
  std::size_t const target_line = Dib::row_bytes(source.width(), static_cast<int>(bpp));
  for (int i = 0; i < source.height(); ++i) {
    // §1.3: topdown reorders rows only; the destination advances by the caller's pitch
    // and only the row's own bytes are written, so a larger pitch keeps its trailing
    // bytes untouched.
    int const scanline = topdown == TRUE ? source.height() - i - 1 : i;
    raw_row(
        std::span<BYTE>(bits + static_cast<std::ptrdiff_t>(pitch) * i, target_line),
        source.row(scanline),
        source,
        static_cast<int>(bpp),
        red_mask,
        green_mask,
        blue_mask);
  }
}

bool premultiply_with_alpha(Dib& dib) {
  if (dib.bpp() != 32) {
    return false;
  }
  for (int y = 0; y < dib.height(); ++y) {
    std::span<BYTE> const row = dib.row(y);
    for (int x = 0; x < dib.width(); ++x) {
      std::span<BYTE> const pixel = row.subspan(static_cast<std::size_t>(x) * 4, 4);
      BYTE const alpha = pixel[FI_RGBA_ALPHA];
      if (alpha == 0x00) {
        pixel[FI_RGBA_BLUE] = 0x00;
        pixel[FI_RGBA_GREEN] = 0x00;
        pixel[FI_RGBA_RED] = 0x00;
      } else if (alpha != 0xFF) {
        // §2.4: integer premultiply with +127 rounding.
        pixel[FI_RGBA_BLUE] = static_cast<BYTE>((alpha * static_cast<unsigned>(pixel[FI_RGBA_BLUE]) + 127) / 255);
        pixel[FI_RGBA_GREEN] = static_cast<BYTE>((alpha * static_cast<unsigned>(pixel[FI_RGBA_GREEN]) + 127) / 255);
        pixel[FI_RGBA_RED] = static_cast<BYTE>((alpha * static_cast<unsigned>(pixel[FI_RGBA_RED]) + 127) / 255);
      }
    }
  }
  return true;
}

bool get_pixel_index(Dib const& dib, unsigned x, unsigned y, BYTE* value) {
  if (value == nullptr || !dib.is_indexed()) {
    return false;
  }
  if (x >= static_cast<unsigned>(dib.width()) || y >= static_cast<unsigned>(dib.height())) {
    return false;
  }
  *value = static_cast<BYTE>(palette_index(dib.row(static_cast<int>(y)), x, dib.bpp()));
  return true;
}

bool set_pixel_index(Dib& dib, unsigned x, unsigned y, BYTE const* value) {
  if (value == nullptr || !dib.is_indexed()) {
    return false;
  }
  if (x >= static_cast<unsigned>(dib.width()) || y >= static_cast<unsigned>(dib.height())) {
    return false;
  }
  std::span<BYTE> const row = dib.row(static_cast<int>(y));
  switch (dib.bpp()) {
    case 1:
      if ((*value) & 1) {
        row[x >> 3] = static_cast<BYTE>(row[x >> 3] | (0x80 >> (x & 7)));
      } else {
        row[x >> 3] = static_cast<BYTE>(row[x >> 3] & ~static_cast<BYTE>(0x80 >> (x & 7)));
      }
      return true;
    case 4: {
      BYTE const shift = static_cast<BYTE>((1 - x % 2) << 2);
      row[x >> 1] = static_cast<BYTE>(
          (row[x >> 1] & ~static_cast<BYTE>(0x0F << shift)) | static_cast<BYTE>((*value & 0x0F) << shift));
      return true;
    }
    default:
      row[x] = *value;
      return true;
  }
}

bool get_pixel_color(Dib const& dib, unsigned x, unsigned y, RGBQUAD* value) {
  if (value == nullptr) {
    return false;
  }
  if (x >= static_cast<unsigned>(dib.width()) || y >= static_cast<unsigned>(dib.height())) {
    return false;
  }
  std::span<BYTE const> const row = dib.row(static_cast<int>(y));
  switch (dib.bpp()) {
    case 16: {
      Channels const rgb = expand16(load16(row.data() + 2 * x), dib.is_rgb565());
      value->rgbBlue = rgb.blue;
      value->rgbGreen = rgb.green;
      value->rgbRed = rgb.red;
      value->rgbReserved = 0;
      return true;
    }
    case 24:
      value->rgbBlue = row[x * 3 + FI_RGBA_BLUE];
      value->rgbGreen = row[x * 3 + FI_RGBA_GREEN];
      value->rgbRed = row[x * 3 + FI_RGBA_RED];
      value->rgbReserved = 0;
      return true;
    case 32:
      value->rgbBlue = row[x * 4 + FI_RGBA_BLUE];
      value->rgbGreen = row[x * 4 + FI_RGBA_GREEN];
      value->rgbRed = row[x * 4 + FI_RGBA_RED];
      value->rgbReserved = row[x * 4 + FI_RGBA_ALPHA];
      return true;
    default:
      return false;
  }
}

bool set_pixel_color(Dib& dib, unsigned x, unsigned y, RGBQUAD const* value) {
  if (value == nullptr) {
    return false;
  }
  if (x >= static_cast<unsigned>(dib.width()) || y >= static_cast<unsigned>(dib.height())) {
    return false;
  }
  std::span<BYTE> const row = dib.row(static_cast<int>(y));
  switch (dib.bpp()) {
    case 16: {
      bool const rgb565 = dib.is_rgb565();
      store16(
          row.data() + 2 * x,
          rgb565 ? pack565(value->rgbBlue, value->rgbGreen, value->rgbRed)
                 : pack555(value->rgbBlue, value->rgbGreen, value->rgbRed));
      return true;
    }
    case 24:
      row[x * 3 + FI_RGBA_BLUE] = value->rgbBlue;
      row[x * 3 + FI_RGBA_GREEN] = value->rgbGreen;
      row[x * 3 + FI_RGBA_RED] = value->rgbRed;
      return true;
    case 32:
      row[x * 4 + FI_RGBA_BLUE] = value->rgbBlue;
      row[x * 4 + FI_RGBA_GREEN] = value->rgbGreen;
      row[x * 4 + FI_RGBA_RED] = value->rgbRed;
      row[x * 4 + FI_RGBA_ALPHA] = value->rgbReserved;
      return true;
    default:
      return false;
  }
}

}  // namespace convert
}  // namespace fi
