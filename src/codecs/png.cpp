// PNG codec (CODECS-owned) over libpng + zlib. The DIB model has no
// 16-bit pixel types, so the accepted input space is classic's FIT_BITMAP
// subset: palette and grey images keep their 1/4/8bpp indexed forms (only the
// 2-bit depth is expanded, as classic does), RGB becomes 24bpp and RGBA - or
// grey+alpha, or an RGB image carrying tRNS - becomes 32bpp. PNG rows are
// stored top-first while the DIB is bottom-up, so file row k is scanline
// height-1-k in both directions (design §1.1).
//
// A gAMA chunk applies classic's png_set_gamma(screen 2.2, file gamma)
// whenever it is present, unless PNG_IGNOREGAMMA is passed (design §3).
//
// libpng reports fatal errors by longjmp'ing to the landing pad installed by
// the frame that called setjmp. That frame is the only place a longjmp can be
// caught, so it owns everything a jump must not orphan - the library handles and
// every scratch buffer alike are released by destructors of objects declared
// there, and the frames the jump skips hold nothing of their own.
// FreeImage.h's PNG_Z_DEFAULT_COMPRESSION is a save-flag value; libpng's is a
// zlib request code. The shipped header must be read first and its macro dropped
// before png.h is; Google style re-sorts includes across blank lines, so the block
// is fenced to keep that order through the formatter.
// clang-format off
#include <algorithm>
#include <array>
#include <csetjmp>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "codecs/png.h"

#undef PNG_Z_DEFAULT_COMPRESSION
#include <png.h>
#include <zlib.h>
// clang-format on

namespace fi {
namespace {

constexpr unsigned kSignatureBytes = 8;
constexpr double kScreenGamma = 2.2;  // classic's fixed assumption about the display

struct IoBridge {
  FreeImageIO* io = nullptr;
  fi_handle handle = nullptr;
};

// Owned by the frame that installs the landing pad: the destructor runs when
// that frame is left either way, so a jump from deep inside a png_* call still
// releases the library handles and the row table.
struct ReadState {
  IoBridge bridge;
  png_structp png = nullptr;
  png_infop info = nullptr;
  std::unique_ptr<Dib> dib;
  std::vector<png_bytep> rows{};

  ~ReadState() {
    if (png != nullptr) {
      png_destroy_read_struct(&png, info != nullptr ? &info : nullptr, nullptr);
    }
  }
};

// Owned by the frame that installs the landing pad, as ReadState is. The palette
// is png_malloc'd, so it goes back through png before the struct that owns the
// pool it came from.
struct WriteState {
  IoBridge bridge;
  png_structp png = nullptr;
  png_infop info = nullptr;
  png_colorp palette = nullptr;
  std::vector<png_byte> packed{};  // 32bpp rows with the alpha byte dropped

  ~WriteState() {
    if (png != nullptr) {
      if (palette != nullptr) {
        png_free(png, palette);
      }
      png_destroy_write_struct(&png, info != nullptr ? &info : nullptr);
    }
  }
};

void Fail(png_structp png, png_const_charp message) {
  FreeImage_OutputMessageProc(FIF_PNG, "%s", message);
  png_longjmp(png, 1);
}

void IgnoreWarning(png_structp /*png*/, png_const_charp /*message*/) {}

// A zero-byte read is the fatal condition; a short read is retried by libpng.
void ReadBytes(png_structp png, png_bytep data, png_size_t length) {
  IoBridge const& bridge = *static_cast<IoBridge*>(png_get_io_ptr(png));
  if (length != 0 && bridge.io->read_proc(data, 1, static_cast<unsigned>(length), bridge.handle) == 0) {
    Fail(png, "PNG: truncated or unreadable stream");
  }
}

void WriteBytes(png_structp png, png_bytep data, png_size_t length) {
  IoBridge const& bridge = *static_cast<IoBridge*>(png_get_io_ptr(png));
  if (bridge.io->write_proc == nullptr ||
      bridge.io->write_proc(data, 1, static_cast<unsigned>(length), bridge.handle) != length) {
    Fail(png, "PNG: the output stream rejected data");
  }
}

void FlushBytes(png_structp /*png*/) {}

bool ReadExact(FreeImageIO const& io, fi_handle handle, void* buffer, std::size_t bytes) {
  return bytes == 0 || io.read_proc(buffer, 1, static_cast<unsigned>(bytes), handle) == bytes;
}

inline BYTE* Scanline(Dib& dib, int y) { return dib.row(y).data(); }

inline BYTE const* Scanline(Dib const& dib, int y) { return dib.row(y).data(); }

// Registers classic's FIT_BITMAP conversions and reports false for the
// combinations classic hands to a 16-bit image type instead.
bool ConfigureDecoder(png_structp png, png_infop info, int flags) {
  int const color_type = png_get_color_type(png, info);
  int const bit_depth = png_get_bit_depth(png, info);
  int const pixel_depth = bit_depth * png_get_channels(png, info);
  bool const has_trns = png_get_valid(png, info, PNG_INFO_tRNS) == PNG_INFO_tRNS;

  switch (color_type) {
    case PNG_COLOR_TYPE_GRAY:
      if (bit_depth > 8) {
        return false;
      }
      if (pixel_depth == 2 || (has_trns && pixel_depth < 8)) {
        png_set_expand_gray_1_2_4_to_8(png);
      }
      break;
    case PNG_COLOR_TYPE_RGB:
      if (bit_depth != 8) {
        return false;
      }
      if (has_trns) {
        png_set_tRNS_to_alpha(png);
      }
      break;
    case PNG_COLOR_TYPE_PALETTE:
      if (bit_depth > 8) {
        return false;
      }
      if (pixel_depth == 2 || (has_trns && pixel_depth < 8)) {
        png_set_packing(png);
      }
      break;
    case PNG_COLOR_TYPE_GRAY_ALPHA:
      if (bit_depth != 8) {
        return false;
      }
      png_set_gray_to_rgb(png);
      break;
    case PNG_COLOR_TYPE_RGB_ALPHA:
      if (bit_depth != 8) {
        return false;
      }
      break;
    default:
      return false;
  }

  if (color_type == PNG_COLOR_TYPE_RGB || color_type == PNG_COLOR_TYPE_RGB_ALPHA) {
    png_set_bgr(png);
  }

  if (png_get_valid(png, info, PNG_INFO_gAMA) == PNG_INFO_gAMA && (flags & PNG_IGNOREGAMMA) != PNG_IGNOREGAMMA) {
    double file_gamma = 0;
    if (png_get_gAMA(png, info, &file_gamma)) {
      png_set_gamma(png, kScreenGamma, file_gamma);
    }
  }

  png_read_update_info(png, info);
  return true;
}

void StorePalette(ReadState const& source, Dib& dib, int color_type, int pixel_depth) {
  std::span<RGBQUAD> const slots = dib.palette();
  if (color_type == PNG_COLOR_TYPE_GRAY) {
    unsigned const entries = 1u << static_cast<unsigned>(pixel_depth);
    for (unsigned i = 0; i < entries && i < slots.size(); ++i) {
      BYTE const level = static_cast<BYTE>((i * 255) / (entries - 1));
      slots[i].rgbRed = level;
      slots[i].rgbGreen = level;
      slots[i].rgbBlue = level;
    }
    return;
  }
  png_colorp entries = nullptr;
  int count = 0;
  png_get_PLTE(source.png, source.info, &entries, &count);
  unsigned const stored = std::min(static_cast<unsigned>(std::max(count, 0)), static_cast<unsigned>(slots.size()));
  for (unsigned i = 0; i < stored; ++i) {
    slots[i].rgbRed = entries[i].red;
    slots[i].rgbGreen = entries[i].green;
    slots[i].rgbBlue = entries[i].blue;
  }
}

void StoreTransparency(ReadState& source, Dib& dib, int color_type, int pixel_depth) {
  if (png_get_valid(source.png, source.info, PNG_INFO_tRNS) != PNG_INFO_tRNS) {
    return;
  }
  png_bytep alpha = nullptr;
  int count = 0;
  png_color_16p color = nullptr;
  png_get_tRNS(source.png, source.info, &alpha, &count, &color);

  if (color_type == PNG_COLOR_TYPE_GRAY && color != nullptr) {
    if (color->gray < kMaxPalette) {
      std::ranges::fill(dib.transparency(), 255);
      dib.transparency()[color->gray] = 0;
      dib.set_transparent();
    } else if (alpha != nullptr && pixel_depth <= 8) {
      count = std::min(count, kMaxPalette);
      for (int i = 0; i < count; ++i) {
        dib.transparency()[i] = alpha[i];
      }
      dib.set_transparent();
    }
    return;
  }
  if (color_type == PNG_COLOR_TYPE_PALETTE && alpha != nullptr) {
    count = std::min(count, kMaxPalette);
    for (int i = 0; i < count; ++i) {
      dib.transparency()[i] = alpha[i];
    }
    dib.set_transparent();
  }
}

bool DecodeImage(ReadState& source, int flags) {
  png_read_info(source.png, source.info);

  if (!ConfigureDecoder(source.png, source.info, flags)) {
    FreeImage_OutputMessageProc(FIF_PNG, "PNG: colour type or bit depth is outside the supported subset");
    return false;
  }

  png_uint_32 width = 0;
  png_uint_32 height = 0;
  int bit_depth = 0;
  int color_type = 0;
  png_get_IHDR(source.png, source.info, &width, &height, &bit_depth, &color_type, nullptr, nullptr, nullptr);
  int const pixel_depth = png_get_bit_depth(source.png, source.info) * png_get_channels(source.png, source.info);
  if ((pixel_depth != 1 && pixel_depth != 4 && pixel_depth != 8 && pixel_depth != 24 && pixel_depth != 32) ||
      width == 0 || height == 0) {
    FreeImage_OutputMessageProc(FIF_PNG, "PNG: unsupported pixel depth %d", pixel_depth);
    return false;
  }

  source.dib = Dib::make(
      static_cast<int>(width),
      static_cast<int>(height),
      pixel_depth,
      FI_RGBA_RED_MASK,
      FI_RGBA_GREEN_MASK,
      FI_RGBA_BLUE_MASK);
  if (source.dib == nullptr) {
    FreeImage_OutputMessageProc(FIF_PNG, "PNG: cannot allocate the bitmap");
    return false;
  }
  Dib& dib = *source.dib;

  if (pixel_depth <= 8) {
    StorePalette(source, dib, color_type, pixel_depth);
    StoreTransparency(source, dib, color_type, pixel_depth);
  }

  source.rows.resize(height);
  for (png_uint_32 k = 0; k < height; ++k) {
    source.rows[height - 1 - k] = Scanline(dib, static_cast<int>(k));
  }

  png_set_benign_errors(source.png, 1);
  png_read_image(source.png, source.rows.data());
  png_read_end(source.png, source.info);
  return true;
}

// classic FreeImage_GetColorType for FIT_BITMAP, restricted to what this DIB
// model can hold: at 32bpp the alpha layer counts as an alpha channel only
// while some pixel is translucent, so a fully opaque 32bpp dib writes color
// type 2 exactly as classic does.
FREE_IMAGE_COLOR_TYPE ColorType(Dib const& dib) {
  switch (dib.bpp()) {
    case 1:
    case 4:
    case 8: {
      std::optional<Ramp> const ramp = dib.greyscale_ramp();
      if (!ramp.has_value()) {
        return FIC_PALETTE;
      }
      return *ramp == Ramp::Inverted ? FIC_MINISWHITE : FIC_MINISBLACK;
    }
    case 16:
    case 24:
      return FIC_RGB;
    case 32:
      for (int y = 0; y < dib.height(); ++y) {
        BYTE const* bits = Scanline(dib, y);
        for (int x = 0; x < dib.width(); ++x) {
          if (bits[x * 4 + FI_RGBA_ALPHA] != 0xFF) {
            return FIC_RGBALPHA;
          }
        }
      }
      return FIC_RGB;
    default:
      return FIC_MINISBLACK;
  }
}

bool EncodeImage(WriteState& target, Dib& dib, int flags) {
  int const interlace = (flags & PNG_INTERLACED) == PNG_INTERLACED ? PNG_INTERLACE_ADAM7 : PNG_INTERLACE_NONE;

  int const zlib_level = flags & 0x0F;
  if (zlib_level >= 1 && zlib_level <= 9) {
    png_set_compression_level(target.png, zlib_level);
  } else if ((flags & PNG_Z_NO_COMPRESSION) == PNG_Z_NO_COMPRESSION) {
    png_set_compression_level(target.png, Z_NO_COMPRESSION);
  }
  if (dib.bpp() >= 16) {
    png_set_compression_strategy(target.png, Z_FILTERED);
    png_set_filter(target.png, 0, PNG_FILTER_NONE | PNG_FILTER_SUB | PNG_FILTER_PAETH);
  } else {
    png_set_compression_strategy(target.png, Z_DEFAULT_STRATEGY);
  }

  int const bit_depth = dib.bpp() > 8 ? 8 : dib.bpp();
  bool const transparent = dib.bpp() <= 8 && dib.is_transparent();
  int color_type = PNG_COLOR_TYPE_RGB;
  bool has_alpha_channel = false;

  switch (ColorType(dib)) {
    case FIC_MINISWHITE:
      if (!transparent) {
        png_set_invert_mono(target.png);
      }
      [[fallthrough]];
    case FIC_MINISBLACK:
      if (!transparent) {
        color_type = PNG_COLOR_TYPE_GRAY;
        break;
      }
      [[fallthrough]];
    case FIC_PALETTE: {
      color_type = PNG_COLOR_TYPE_PALETTE;
      int const entries = 1 << bit_depth;
      target.palette =
          static_cast<png_colorp>(png_malloc(target.png, sizeof(png_color) * static_cast<size_t>(entries)));
      if (target.palette == nullptr) {
        FreeImage_OutputMessageProc(FIF_PNG, "PNG: cannot allocate the colour map");
        return false;
      }
      std::span<RGBQUAD const> const palette = dib.palette();
      for (int i = 0; i < entries; ++i) {
        RGBQUAD const& entry = static_cast<std::size_t>(i) < palette.size() ? palette[i] : palette.back();
        target.palette[i].red = entry.rgbRed;
        target.palette[i].green = entry.rgbGreen;
        target.palette[i].blue = entry.rgbBlue;
      }
      png_set_PLTE(target.png, target.info, target.palette, entries);
      break;
    }
    case FIC_RGBALPHA:
      color_type = PNG_COLOR_TYPE_RGBA;
      has_alpha_channel = true;
      png_set_bgr(target.png);
      break;
    case FIC_RGB:
      color_type = PNG_COLOR_TYPE_RGB;
      png_set_bgr(target.png);
      break;
    case FIC_CMYK:
      return false;
  }

  png_set_IHDR(
      target.png,
      target.info,
      static_cast<png_uint_32>(dib.width()),
      static_cast<png_uint_32>(dib.height()),
      bit_depth,
      color_type,
      interlace,
      PNG_COMPRESSION_TYPE_BASE,
      PNG_FILTER_TYPE_BASE);

  if (transparent) {
    png_set_tRNS(target.png, target.info, dib.transparency().data(), kMaxPalette, nullptr);
  }

  png_write_info(target.png, target.info);

  int const passes = interlace == PNG_INTERLACE_ADAM7 ? png_set_interlace_handling(target.png) : 1;
  bool const strip_alpha = dib.bpp() == 32 && !has_alpha_channel;
  if (strip_alpha) {
    target.packed.resize(static_cast<std::size_t>(dib.width()) * 3);
  }
  for (int pass = 0; pass < passes; ++pass) {
    for (int k = 0; k < dib.height(); ++k) {
      BYTE const* bits = Scanline(dib, dib.height() - k - 1);
      if (strip_alpha) {
        // classic's ConvertLine32To24 copies the triple and lets the
        // registered bgr swap reorder it for the file.
        for (int x = 0; x < dib.width(); ++x) {
          target.packed[x * 3] = bits[x * 4];
          target.packed[x * 3 + 1] = bits[x * 4 + 1];
          target.packed[x * 3 + 2] = bits[x * 4 + 2];
        }
      }
      png_write_row(target.png, strip_alpha ? target.packed.data() : bits);
    }
  }

  png_write_end(target.png, target.info);
  return true;
}

}  // namespace

std::string_view PngCodec::format_name() const { return "PNG"; }

std::string_view PngCodec::extensions() const { return "png"; }

std::string_view PngCodec::description() const { return "Portable Network Graphics"; }

bool PngCodec::validate(FreeImageIO const& io, fi_handle handle) const {
  static constexpr std::array<png_byte, kSignatureBytes> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
  std::array<png_byte, kSignatureBytes> signature{};
  if (!ReadExact(io, handle, signature.data(), signature.size())) {
    return false;
  }
  return png_sig_cmp(signature.data(), 0, kSignatureBytes) == 0;
}

std::unique_ptr<Dib> PngCodec::load(FreeImageIO& io, fi_handle handle, int /*page*/, int flags) const {
  if (handle == nullptr) {
    return nullptr;
  }
  std::array<png_byte, kSignatureBytes> signature{};
  if (!ReadExact(io, handle, signature.data(), signature.size()) ||
      png_sig_cmp(signature.data(), 0, kSignatureBytes) != 0) {
    FreeImage_OutputMessageProc(FIF_PNG, "PNG: the signature is missing");
    return nullptr;
  }

  ReadState source{{&io, handle}};
  source.png = png_create_read_struct(PNG_LIBPNG_VER_STRING, nullptr, &Fail, &IgnoreWarning);
  if (source.png == nullptr) {
    return nullptr;
  }
  source.info = png_create_info_struct(source.png);
  if (source.info == nullptr) {
    return nullptr;
  }
  png_set_read_fn(source.png, &source.bridge, &ReadBytes);
  png_set_sig_bytes(source.png, kSignatureBytes);

  if (setjmp(png_jmpbuf(source.png)) != 0) {
    return nullptr;
  }
  if (!DecodeImage(source, flags)) {
    return nullptr;
  }
  std::unique_ptr<Dib> result = std::move(source.dib);
  return result;
}

bool PngCodec::save(FreeImageIO& io, Dib& dib, fi_handle handle, int flags) const {
  if (!dib.has_pixels() || !dib.is_bitmap() || handle == nullptr) {
    return false;
  }
  if (dib.bpp() != 1 && dib.bpp() != 4 && dib.bpp() != 8 && dib.bpp() != 24 && dib.bpp() != 32) {
    FreeImage_OutputMessageProc(FIF_PNG, "PNG: a %d bpp image is not savable as PNG", dib.bpp());
    return false;
  }

  WriteState target{{&io, handle}};
  target.png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, &Fail, &IgnoreWarning);
  if (target.png == nullptr) {
    return false;
  }
  target.info = png_create_info_struct(target.png);
  if (target.info == nullptr) {
    return false;
  }
  png_set_write_fn(target.png, &target.bridge, &WriteBytes, &FlushBytes);

  if (setjmp(png_jmpbuf(target.png)) != 0) {
    return false;
  }
  return EncodeImage(target, dib, flags);
}

int PngCodec::page_count(FreeImageIO& /*io*/, fi_handle /*handle*/) const { return 1; }

PngCodec const png_codec;

}  // namespace fi
