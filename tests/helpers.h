// Shared support for the freeimage-for-es-de unit suites.
//
// The doctest entry point is compiled exactly once, in helpers.cpp
// (DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN); every other test translation unit includes
// only this header, so adding a test file needs no CMake edit. Codec-suite authors:
// reuse the builders, the golden oracles and the CHECK_BYTES_EQ / REQUIRE_BYTES_EQ
// macros below instead of redefining them.
//
// Everything here is derived from the behavioral contract (design.md), never from
// the implementation: the oracles are restatements of the contract's arithmetic, so
// a test cannot agree with a wrong implementation by sharing its code.
#pragma once

#include <FreeImage.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fit {

using Bytes = std::vector<BYTE>;

// Destination pre-fill. An implementation that writes nothing must not accidentally
// match a golden buffer built from zeroes.
constexpr BYTE kPoison = 0xA5;

// §1.1 CalculateLine: (((width * bpp + 7) >> 3) + 3) & ~3, restated from the
// contract so geometry expectations stay independent of the library's arithmetic.
constexpr unsigned calc_line(int width, int bpp) {
  const int bytes_per_row = (width * bpp + 7) >> 3;
  return static_cast<unsigned>((bytes_per_row + 3) & ~3);
}

// §2.4 PreMultiplyWithAlpha channel arithmetic: c = (a * c + 127) / 255 in integer
// math with +127 rounding. The formula alone reproduces both documented special
// cases: a == 0 yields 0 and a == 255 is the identity.
constexpr BYTE premultiplied(BYTE channel, BYTE alpha) {
  return static_cast<BYTE>((alpha * static_cast<unsigned>(channel) + 127) / 255);
}

// One pixel in the contract's little-endian BGR(A) byte order (§1.1, §1.4).
struct Pixel {
  BYTE blue;
  BYTE green;
  BYTE red;
  BYTE alpha;

  void store(BYTE* p, unsigned bpp) const {
    p[FI_RGBA_BLUE] = blue;
    p[FI_RGBA_GREEN] = green;
    p[FI_RGBA_RED] = red;
    if (bpp == 32) {
      p[FI_RGBA_ALPHA] = alpha;
    }
  }
};

// Row- and column-identifiable fixture pixel: the blue channel carries the buffer
// row in its high nibble and the column in its low nibble, and every other channel
// is a distinct function of both. Requires row < 16 and column < 16.
constexpr Pixel pattern(int row, int column) {
  const BYTE code = static_cast<BYTE>(row * 16 + column);
  return Pixel{code, static_cast<BYTE>(0xFF - code), static_cast<BYTE>(code ^ 0x80), static_cast<BYTE>(0xF0 - row)};
}

// A caller-owned raw buffer: width * height pixels of pattern() laid out row-major
// at `pitch`, with pitch-padding bytes left zero.
inline Bytes make_buffer(int width, int height, unsigned bpp, unsigned pitch) {
  Bytes buffer(static_cast<std::size_t>(pitch) * static_cast<unsigned>(height), 0);
  for (int row = 0; row < height; ++row) {
    BYTE* begin = buffer.data() + static_cast<std::size_t>(pitch) * static_cast<unsigned>(row);
    for (int column = 0; column < width; ++column) {
      pattern(row, column).store(begin + static_cast<std::size_t>(column) * (bpp / 8), bpp);
    }
  }
  return buffer;
}

// FIBITMAP owner. The API has no Allocate/Clone, so every dib in these tests comes
// from ConvertFromRawBits (or, later, from a codec load).
class Bitmap {
 public:
  explicit Bitmap(FIBITMAP* dib = nullptr) : dib_(dib) {}
  ~Bitmap() { FreeImage_Unload(dib_); }
  Bitmap(const Bitmap&) = delete;
  Bitmap& operator=(const Bitmap&) = delete;
  Bitmap(Bitmap&& other) noexcept : dib_(std::exchange(other.dib_, nullptr)) {}

  FIBITMAP* get() const { return dib_; }

 private:
  FIBITMAP* dib_;
};

// FIMEMORY owner.
class Memory {
 public:
  explicit Memory(FIMEMORY* stream = nullptr) : stream_(stream) {}
  ~Memory() { FreeImage_CloseMemory(stream_); }
  Memory(const Memory&) = delete;
  Memory& operator=(const Memory&) = delete;
  Memory(Memory&& other) noexcept : stream_(std::exchange(other.stream_, nullptr)) {}

  FIMEMORY* get() const { return stream_; }

 private:
  FIMEMORY* stream_;
};

// Buffers handed to the API by the convenience builders below stay alive for the
// whole run. ConvertFromRawBits is contractually a copy (§1.3); if an implementation
// broke that clause and kept the pointer, the suite reads back defined memory rather
// than a dangling stack buffer. The reference is to the heap allocation the arena
// owns, never to the argument, so it stays valid (and at a fixed address) for every
// later mutation of the caller's own copy.
inline const Bytes& keep_alive(Bytes buffer) {
  static std::deque<Bytes*>* arena = new std::deque<Bytes*>{};
  Bytes* stored = new Bytes(std::move(buffer));
  arena->push_back(stored);
  return *stored;
}

// ConvertFromRawBits in the ES-DE MiximageGenerator call shape (masks ignored for
// equal bpp per §1.3, so the mask arguments carry no meaning here). The caller keeps
// `buffer` alive and may mutate it afterwards to observe the copy clause.
inline Bitmap
bitmap_from_buffer(const Bytes& buffer, int width, int height, unsigned pitch, unsigned bpp, BOOL topdown = FALSE) {
  Bytes& mutable_buffer = const_cast<Bytes&>(buffer);  // the API takes BYTE* but writes nothing
  return Bitmap(FreeImage_ConvertFromRawBits(
      mutable_buffer.data(),
      width,
      height,
      static_cast<int>(pitch),
      bpp,
      FI_RGBA_RED_MASK,
      FI_RGBA_GREEN_MASK,
      FI_RGBA_BLUE_MASK,
      topdown));
}

// A dib whose scanlines are pattern(row, column), built at the classic line pitch.
// `topdown == FALSE` means the fixture buffer is bottom-up, so scanline y holds
// pattern(y, x); `topdown == TRUE` declares it top-down, so scanline y holds
// pattern(height - 1 - y, x) (§1.3).
inline Bitmap bitmap_from_pattern(int width, int height, unsigned bpp, BOOL topdown = FALSE) {
  const Bytes& buffer = keep_alive(make_buffer(width, height, bpp, calc_line(width, static_cast<int>(bpp))));
  return bitmap_from_buffer(buffer, width, height, calc_line(width, static_cast<int>(bpp)), bpp, topdown);
}

// ConvertToRawBits into a poison-filled destination, mirroring how ES-DE passes a
// sized vector together with the dib's own pitch.
inline Bytes to_raw_bits(
    const Bitmap& dib,
    unsigned pitch,
    unsigned bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask,
    BOOL topdown) {
  const unsigned height = FreeImage_GetHeight(dib.get());
  Bytes out(static_cast<std::size_t>(pitch) * height, kPoison);
  if (out.empty()) {
    return out;
  }
  FreeImage_ConvertToRawBits(
      out.data(),
      dib.get(),
      static_cast<int>(pitch),
      bpp,
      red_mask,
      green_mask,
      blue_mask,
      topdown);
  return out;
}

// The ES-DE GIFAnimComponent / MiximageGenerator argument order (§1.3 call shapes).
inline Bytes export_bgr(const Bitmap& dib, unsigned pitch, unsigned bpp, BOOL topdown) {
  return to_raw_bits(dib, pitch, bpp, FI_RGBA_BLUE, FI_RGBA_GREEN, FI_RGBA_RED, topdown);
}

// The contradictory GIFAnimComponent::render order (§1.3).
inline Bytes export_rgb(const Bitmap& dib, unsigned pitch, unsigned bpp, BOOL topdown) {
  return to_raw_bits(dib, pitch, bpp, FI_RGBA_RED, FI_RGBA_GREEN, FI_RGBA_BLUE, topdown);
}

// One buffer row as bytes; empty when the request is out of range, so a mistake
// fails a comparison instead of reading past the end.
inline Bytes row_of(const Bytes& buffer, unsigned pitch, int row) {
  const std::size_t begin = static_cast<std::size_t>(pitch) * static_cast<unsigned>(row);
  if (row < 0 || begin >= buffer.size()) {
    return Bytes{};
  }
  const std::size_t end = std::min(buffer.size(), begin + static_cast<std::size_t>(pitch));
  return Bytes(buffer.begin() + static_cast<std::ptrdiff_t>(begin), buffer.begin() + static_cast<std::ptrdiff_t>(end));
}

// One scanline of a dib (GetLine bytes); empty when the guard returns NULL or the
// dib has no geometry, so comparisons fail rather than crash.
inline Bytes scanline_of(const Bitmap& dib, int y) {
  BYTE* p = FreeImage_GetScanLine(dib.get(), y);
  const unsigned line = FreeImage_GetLine(dib.get());
  if (p == nullptr || line == 0) {
    return Bytes{};
  }
  return Bytes(p, p + line);
}

// Every scanline concatenated, scanline 0 first (the linear array order of §1.1).
inline Bytes all_scanlines(const Bitmap& dib) {
  const unsigned height = FreeImage_GetHeight(dib.get());
  const unsigned line = FreeImage_GetLine(dib.get());
  Bytes out;
  out.reserve(static_cast<std::size_t>(line) * height);
  for (unsigned y = 0; y < height; ++y) {
    const Bytes row = scanline_of(dib, static_cast<int>(y));
    out.insert(out.end(), row.begin(), row.end());
  }
  return out;
}

// Golden destination for ConvertToRawBits of a dib built from `buffer` with
// topdown == FALSE: dst row r receives scanline (topdown ? height-1-r : r), and a
// scanline is that fixture buffer row verbatim (§1.3).
inline Bytes golden_raw_bits(
    const Bytes& buffer,
    int width,
    int height,
    unsigned src_pitch,
    unsigned dst_pitch,
    unsigned bpp,
    BOOL topdown) {
  const unsigned line = calc_line(width, static_cast<int>(bpp));
  Bytes out(static_cast<std::size_t>(dst_pitch) * static_cast<unsigned>(height), kPoison);
  for (int row = 0; row < height; ++row) {
    const int source_row = topdown == TRUE ? height - row - 1 : row;
    const Bytes source = row_of(buffer, src_pitch, source_row);
    if (source.size() < line) {
      return Bytes{};
    }
    const std::size_t offset = static_cast<std::size_t>(dst_pitch) * static_cast<unsigned>(row);
    std::copy(
        source.begin(),
        source.begin() + static_cast<std::ptrdiff_t>(line),
        out.begin() + static_cast<std::ptrdiff_t>(offset));
  }
  return out;
}

// Golden dib content (scanline 0 first) for a dib built from `buffer` with the given
// topdown flag (§1.3: rows are copied into scanlines 0..h-1 in order, then flipped
// iff topdown == TRUE).
inline Bytes
golden_scanlines(const Bytes& buffer, int width, int height, unsigned src_pitch, unsigned bpp, BOOL topdown) {
  const unsigned line = calc_line(width, static_cast<int>(bpp));
  Bytes out;
  out.reserve(static_cast<std::size_t>(line) * static_cast<unsigned>(height));
  for (int y = 0; y < height; ++y) {
    const int source_row = topdown == TRUE ? height - y - 1 : y;
    const Bytes source = row_of(buffer, src_pitch, source_row);
    out.insert(out.end(), source.begin(), source.begin() + static_cast<std::ptrdiff_t>(line));
  }
  return out;
}

inline std::string hex(const Bytes& bytes) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 3);
  for (const BYTE b : bytes) {
    out.push_back(digits[b >> 4]);
    out.push_back(digits[b & 0x0F]);
    out.push_back(' ');
  }
  return out;
}

// Byte-exact comparison with a readable dump on failure. Both arguments are
// evaluated twice: pass variables, not calls with side effects.
#define REQUIRE_BYTES_EQ(actual, expected) \
  REQUIRE_MESSAGE((actual) == (expected), "\n  actual " << fit::hex(actual) << "\n  expected " << fit::hex(expected))
#define CHECK_BYTES_EQ(actual, expected) \
  CHECK_MESSAGE((actual) == (expected), "\n  actual " << fit::hex(actual) << "\n  expected " << fit::hex(expected))

// A fresh directory under $TMPDIR for the few tests that need real files, removed on
// scope exit. Each test owns its own directory, so order never matters.
class TempDir {
 public:
  TempDir() {
    std::string tmpl = (std::filesystem::temp_directory_path() / "freeimage-for-es-de-tests-XXXXXX").string();
    std::vector<char> buffer(tmpl.begin(), tmpl.end());
    buffer.push_back('\0');
    const char* made = ::mkdtemp(buffer.data());
    if (made == nullptr) {
      throw std::runtime_error("mkdtemp failed for " + tmpl);
    }
    path_ = made;
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  std::filesystem::path file(const std::string& name) const { return path_ / name; }

 private:
  std::filesystem::path path_;
};

inline void write_file(const std::filesystem::path& path, const Bytes& data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  out.close();
  if (!out) {
    throw std::runtime_error("failed to write " + path.string());
  }
}

// Returns false and leaves size untouched when the path does not exist.
inline bool file_size(const std::filesystem::path& path, std::uintmax_t& size) {
  std::error_code ec;
  const std::filesystem::file_status status = std::filesystem::status(path, ec);
  if (ec || !std::filesystem::is_regular_file(status)) {
    return false;
  }
  size = std::filesystem::file_size(path, ec);
  return !ec;
}

inline void append_le32(Bytes& out, std::uint32_t value) {
  out.push_back(static_cast<BYTE>(value & 0xFF));
  out.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
  out.push_back(static_cast<BYTE>((value >> 16) & 0xFF));
  out.push_back(static_cast<BYTE>((value >> 24) & 0xFF));
}

inline void append_le16(Bytes& out, std::uint16_t value) {
  out.push_back(static_cast<BYTE>(value & 0xFF));
  out.push_back(static_cast<BYTE>((value >> 8) & 0xFF));
}

// A structurally valid 2x2 24bpp BMP (54-byte header, bottom-up rows padded to 4
// bytes). Written by hand so file-type detection can be exercised without any codec.
inline Bytes tiny_bmp() {
  static constexpr unsigned kPixelOffset = 54;
  static constexpr unsigned kRowBytes = 8;  // two 24bpp pixels padded up from 6
  Bytes out;
  out.push_back('B');
  out.push_back('M');
  append_le32(out, kPixelOffset + 2 * kRowBytes);
  append_le32(out, 0);  // reserved
  append_le32(out, kPixelOffset);
  append_le32(out, 40);  // BITMAPINFOHEADER size
  append_le32(out, 2);   // width
  append_le32(out, 2);   // height, positive == bottom-up
  append_le16(out, 1);   // planes
  append_le16(out, 24);  // bpp
  append_le32(out, 0);   // BI_RGB
  append_le32(out, 2 * kRowBytes);
  append_le32(out, 0);
  append_le32(out, 0);
  append_le32(out, 0);
  append_le32(out, 0);
  for (int row = 0; row < 2; ++row) {
    out.resize(out.size() + kRowBytes, 0);
    BYTE* pixels = out.data() + out.size() - kRowBytes;
    for (int column = 0; column < 2; ++column) {
      pattern(row, column).store(pixels + column * 3, 24);
    }
  }
  return out;
}

// The classic little-endian TIFF magic plus a first-IFD offset. TIFF has no plugin
// in this library, so this buffer must stay undetectable and unreadable.
inline Bytes tiff_header() {
  Bytes out{'I', 'I', 0x2A, 0x00};
  append_le32(out, 8);
  return out;
}

}  // namespace fit
