// BMP codec: an uncompressed DIB passthrough, so no external dependency (§3).
// BMP stores its rows bottom-up when biHeight is positive, which is exactly the Dib
// orientation - file row i is scanline i (§1.1) - and a negative biHeight marks a
// top-down file, whose row i is scanline height-1-i.
//
// Supported subset, per the contract's BI_RGB scope: uncompressed 1/4/8/16/24/32 bpp
// with a BITMAPINFOHEADER-sized DIB header (larger V2..V5 headers are accepted and their
// extra bytes skipped), plus BI_BITFIELDS at 16 bpp, where the file masks decide 565
// versus 555. OS/2 bitmap headers, RLE4/RLE8, JPEG/PNG-in-BMP, and bit fields at other
// depths are rejected the way a codec rejects an unsupported file: FreeImage_Load
// returns NULL and the message sink reports why.
#include "codecs/bmp.h"

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>

namespace fi {
namespace {

constexpr int kFileHeaderSize = 14;
constexpr int kInfoHeaderSize = 40;
constexpr DWORD kBitFields = 3;
constexpr DWORD kBitFieldsHeaderOffset = 40;  // first mask field inside a V2..V5 header

// The header fields are little-endian on disk, which this build's native order is
// (§1.1); the memcpy keeps access free of the alignment and aliasing UB of a cast.
DWORD le32(std::span<BYTE const, 4> field) {
  DWORD value = 0;
  std::memcpy(&value, field.data(), sizeof(value));
  return value;
}

WORD le16(std::span<BYTE const, 2> field) {
  WORD value = 0;
  std::memcpy(&value, field.data(), sizeof(value));
  return value;
}

void put32(std::span<BYTE, 4> field, DWORD value) { std::memcpy(field.data(), &value, sizeof(value)); }

void put16(std::span<BYTE, 2> field, WORD value) { std::memcpy(field.data(), &value, sizeof(value)); }

bool read_all(FreeImageIO const& io, fi_handle handle, std::span<std::byte> bytes) {
  return bytes.empty() || io.read_proc(bytes.data(), 1, static_cast<unsigned>(bytes.size()), handle) == bytes.size();
}

// FreeImageIO::write_proc takes a void* for stdio compatibility; it never writes through
// it, so the buffer stays const on this side.
bool write_all(FreeImageIO const& io, fi_handle handle, std::span<std::byte const> bytes) {
  if (io.write_proc == nullptr) {
    return false;
  }
  return bytes.empty() ||
         io.write_proc(const_cast<std::byte*>(bytes.data()), 1, static_cast<unsigned>(bytes.size()), handle) ==
             bytes.size();
}

bool supported_bpp(int bpp) { return bpp == 1 || bpp == 4 || bpp == 8 || bpp == 16 || bpp == 24 || bpp == 32; }

}  // namespace

std::string_view BmpCodec::format_name() const { return "BMP"; }

std::string_view BmpCodec::extensions() const { return "bmp,dib,rle"; }

std::string_view BmpCodec::description() const { return "Windows or OS/2 Bitmap"; }

// Classic ValidateBMP checks the two magic bytes only, and GetFileType relies on that
// when a buffer carries several magics (§2.3: detection iterates ascending and the first
// validate hit wins).
bool BmpCodec::validate(FreeImageIO const& io, fi_handle handle) const {
  std::array<BYTE, 2> magic{};
  if (!read_all(io, handle, std::as_writable_bytes(std::span(magic)))) {
    return false;
  }
  return magic[0] == 'B' && magic[1] == 'M';
}

std::unique_ptr<Dib> BmpCodec::load(FreeImageIO& io, fi_handle handle, int /*page*/, int /*flags*/) const {
  std::array<BYTE, kFileHeaderSize> file_header{};
  if (!read_all(io, handle, std::as_writable_bytes(std::span(file_header))) || file_header[0] != 'B' ||
      file_header[1] != 'M') {
    FreeImage_OutputMessageProc(FIF_BMP, "BMP: not a Windows bitmap");
    return nullptr;
  }
  DWORD const pixel_offset = le32(std::span(file_header).subspan<10, 4>());

  std::array<BYTE, kInfoHeaderSize> header{};
  if (!read_all(io, handle, std::as_writable_bytes(std::span(header)))) {
    FreeImage_OutputMessageProc(FIF_BMP, "BMP: truncated DIB header");
    return nullptr;
  }
  DWORD const header_size = le32(std::span(header).subspan<0, 4>());
  if (header_size < static_cast<DWORD>(kInfoHeaderSize)) {
    FreeImage_OutputMessageProc(FIF_BMP, "BMP: OS/2 bitmap headers are not supported");
    return nullptr;
  }
  int const width = static_cast<int>(le32(std::span(header).subspan<4, 4>()));
  int const height = static_cast<int>(le32(std::span(header).subspan<8, 4>()));
  int const bpp = static_cast<int>(le16(std::span(header).subspan<14, 2>()));
  DWORD const compression = le32(std::span(header).subspan<16, 4>());
  DWORD const colors_used = le32(std::span(header).subspan<32, 4>());
  if (width <= 0 || height == 0 || !supported_bpp(bpp)) {
    FreeImage_OutputMessageProc(FIF_BMP, "BMP: unsupported dimensions or bit depth");
    return nullptr;
  }

  unsigned red_mask = 0;
  unsigned green_mask = 0;
  unsigned blue_mask = 0;
  if (compression == kBitFields) {
    if (bpp != 16) {
      FreeImage_OutputMessageProc(FIF_BMP, "BMP: bit fields are only supported at 16 bpp");
      return nullptr;
    }
    // A V2..V5 header carries its masks inside the header; a plain BITMAPINFOHEADER puts
    // the three DWORDs immediately after it.
    bool const masks_inside = header_size >= kBitFieldsHeaderOffset + 3 * sizeof(DWORD);
    if (masks_inside &&
        io.seek_proc(handle, kFileHeaderSize + static_cast<long>(kBitFieldsHeaderOffset), SEEK_SET) != 0) {
      FreeImage_OutputMessageProc(FIF_BMP, "BMP: truncated bit field masks");
      return nullptr;
    }
    std::array<BYTE, 3 * sizeof(DWORD)> masks{};
    if (!read_all(io, handle, std::as_writable_bytes(std::span(masks)))) {
      FreeImage_OutputMessageProc(FIF_BMP, "BMP: truncated bit field masks");
      return nullptr;
    }
    red_mask = le32(std::span(masks).subspan<0, 4>());
    green_mask = le32(std::span(masks).subspan<4, 4>());
    blue_mask = le32(std::span(masks).subspan<8, 4>());
  } else if (compression != 0) {
    FreeImage_OutputMessageProc(FIF_BMP, "BMP: compressed BMP data is not supported");
    return nullptr;
  } else if (bpp == 16) {
    // A 16bpp BI_RGB file carries no masks, so the depth decides the layout, and classic
    // resolves that default to 555.
    red_mask = FI16_555_RED_MASK;
    green_mask = FI16_555_GREEN_MASK;
    blue_mask = FI16_555_BLUE_MASK;
  }

  int const rows = height > 0 ? height : -height;
  bool const top_down = height < 0;
  std::unique_ptr<Dib> dib = Dib::make(width, rows, bpp, red_mask, green_mask, blue_mask);
  if (dib == nullptr) {
    return nullptr;
  }

  unsigned palette_entries = 0;
  if (bpp <= 8) {
    unsigned const implied = 1u << static_cast<unsigned>(bpp);
    palette_entries = colors_used != 0 && colors_used <= implied ? colors_used : implied;
    if (palette_entries > kMaxPalette) {
      palette_entries = kMaxPalette;
    }
  }

  std::size_t const minimum_offset =
      static_cast<std::size_t>(kFileHeaderSize) + header_size + palette_entries * sizeof(RGBQUAD);
  DWORD pixel_data_offset = pixel_offset;
  if (pixel_data_offset < minimum_offset) {
    pixel_data_offset = static_cast<DWORD>(minimum_offset);  // a bad offset is clamped up, as classic does
  }

  if (palette_entries != 0) {
    if (io.seek_proc(handle, kFileHeaderSize + static_cast<long>(header_size), SEEK_SET) != 0) {
      return nullptr;
    }
    // palette_entries never exceeds the 1 << bpp palette the dib already holds.
    std::span<RGBQUAD> entries = dib->palette().first(palette_entries);
    if (!read_all(io, handle, std::as_writable_bytes(entries))) {
      FreeImage_OutputMessageProc(FIF_BMP, "BMP: truncated palette");
      return nullptr;
    }
  }

  if (io.seek_proc(handle, static_cast<long>(pixel_data_offset), SEEK_SET) != 0) {
    return nullptr;
  }
  for (int y = 0; y < rows; ++y) {
    // A positive-height file is bottom-up, so file row y is scanline y; the
    // negative-height form stores its first row at the visual top.
    int const scanline = top_down ? rows - 1 - y : y;
    if (!read_all(io, handle, std::as_writable_bytes(dib->row(scanline)))) {
      FreeImage_OutputMessageProc(FIF_BMP, "BMP: truncated pixel data");
      return nullptr;
    }
  }
  return dib;
}

bool BmpCodec::save(FreeImageIO& io, Dib& dib, fi_handle handle, int flags) const {
  if (!dib.has_pixels() || !dib.is_bitmap()) {
    return false;
  }
  if ((flags & BMP_SAVE_RLE) == BMP_SAVE_RLE) {
    FreeImage_OutputMessageProc(FIF_BMP, "BMP: RLE encoding is not supported");
    return false;
  }
  if (!supported_bpp(dib.bpp())) {
    FreeImage_OutputMessageProc(FIF_BMP, "BMP: cannot save a %d bpp image", dib.bpp());
    return false;
  }
  bool const bit_fields = dib.bpp() == 16;  // classic writes 16 bpp as BI_BITFIELDS
  unsigned const palette_entries = dib.bpp() <= 8 ? (1u << static_cast<unsigned>(dib.bpp())) : 0u;

  DWORD const pixel_offset = static_cast<DWORD>(
      kFileHeaderSize + kInfoHeaderSize + bit_fields * 3 * sizeof(DWORD) + palette_entries * sizeof(RGBQUAD));
  DWORD const pixel_bytes = dib.pitch() * static_cast<DWORD>(dib.height());

  std::array<BYTE, kFileHeaderSize> file_header{'B', 'M'};
  put32(std::span(file_header).subspan<2, 4>(), pixel_offset + pixel_bytes);
  put32(std::span(file_header).subspan<6, 4>(), 0);
  put32(std::span(file_header).subspan<10, 4>(), pixel_offset);
  if (!write_all(io, handle, std::as_bytes(std::span(file_header)))) {
    return false;
  }

  std::array<BYTE, kInfoHeaderSize> header{};
  put32(std::span(header).subspan<0, 4>(), kInfoHeaderSize);
  put32(std::span(header).subspan<4, 4>(), static_cast<DWORD>(dib.width()));
  // Positive height: bottom-up, which is the Dib order (§1.1).
  put32(std::span(header).subspan<8, 4>(), static_cast<DWORD>(dib.height()));
  put16(std::span(header).subspan<12, 2>(), 1);
  put16(std::span(header).subspan<14, 2>(), static_cast<WORD>(dib.bpp()));
  put32(std::span(header).subspan<16, 4>(), bit_fields ? kBitFields : 0);
  put32(std::span(header).subspan<20, 4>(), pixel_bytes);
  put32(std::span(header).subspan<32, 4>(), palette_entries);
  if (!write_all(io, handle, std::as_bytes(std::span(header)))) {
    return false;
  }

  if (bit_fields) {
    std::array<BYTE, 3 * sizeof(DWORD)> masks{};
    put32(std::span(masks).subspan<0, 4>(), dib.red_mask());
    put32(std::span(masks).subspan<4, 4>(), dib.green_mask());
    put32(std::span(masks).subspan<8, 4>(), dib.blue_mask());
    if (!write_all(io, handle, std::as_bytes(std::span(masks)))) {
      return false;
    }
  }

  if (palette_entries != 0) {
    std::span<RGBQUAD const> const palette = dib.palette();
    std::array<RGBQUAD, kMaxPalette> entries{};
    for (unsigned i = 0; i < palette_entries; ++i) {
      entries[i] = i < palette.size() ? palette[i] : RGBQUAD{};
    }
    std::span<RGBQUAD const> written(entries.data(), palette_entries);
    if (!write_all(io, handle, std::as_bytes(written))) {
      return false;
    }
  }

  for (int y = 0; y < dib.height(); ++y) {  // scanline 0 is the visual bottom, which is BMP row 0
    if (!write_all(io, handle, std::as_bytes(dib.row(y)))) {
      return false;
    }
  }
  return true;
}

int BmpCodec::page_count(FreeImageIO& /*io*/, fi_handle /*handle*/) const { return 1; }

BmpCodec const bmp_codec;

}  // namespace fi
