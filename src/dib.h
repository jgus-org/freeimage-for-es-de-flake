// The core type layer: the device-independent bitmap and its tag model. Audience:
// every layer below the C ABI - codecs, the codec interface, the registry and the
// marshaling TUs all build on these types; nothing here knows a FIBITMAP handle or
// a FreeImageIO stream exists.
//
// The public header couples channel order to host endianness, and the contract pins
// this build to the little-endian BGR layout: a 32bpp pixel is B G R A in memory
// (§1.1). On-disk field access (a codec's header reads) is written for that native
// order.
#pragma once

#include <FreeImage.h>

#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace fi {

static_assert(
    std::endian::native == std::endian::little,
    "the BGR channel order this API publishes assumes a little-endian host");

inline constexpr int kMaxPalette = 256;

// The direction of an ordered greyscale ramp: the MINISWHITE distinction that drives
// Rescale's destination depth choice and the mono inverters (§2.8).
enum class Ramp {
  Normal,
  Inverted,
};

// Metadata ------------------------------------------------------------

// One tag store per metadata model: FIMD_NODATA(-1) .. FIMD_EXIF_RAW(11).
inline constexpr int kFirstModel = -1;
inline constexpr int kLastModel = 11;
inline constexpr int kModelCount = kLastModel - kFirstModel + 1;

// Storage slot for a model, or -1 when the model is outside the supported range.
constexpr int model_slot(FREE_IMAGE_MDMODEL model) {
  int const id = static_cast<int>(model);
  return (id >= kFirstModel && id <= kLastModel) ? id - kFirstModel : -1;
}

// The Exif element width of a tag type; 0 for FIDT_NOTYPE and for any value outside
// the FIDT range.
constexpr std::size_t element_size(FREE_IMAGE_MDTYPE value_type) {
  constexpr std::array<std::size_t, 19> kSizes{0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4, 4, 0, 8, 8, 8};
  std::size_t const id = static_cast<std::size_t>(value_type);
  return id < kSizes.size() ? kSizes[id] : 0;
}

struct Tag {
  // Invariant: value.size() is always an integral multiple of element_size(type), so
  // the element count is derived from the payload and never stored.
  std::string key;
  FREE_IMAGE_MDTYPE type = FIDT_NOTYPE;
  std::vector<BYTE> value;

  DWORD count() const {
    std::size_t const size = element_size(type);
    return size != 0 ? static_cast<DWORD>(value.size() / size) : 0;
  }
};

// The bytes of one scalar as tag payload, in the span<BYTE const> shape a tag add
// takes; std::as_bytes yields span<std::byte const>, which is not that shape.
template<typename T> std::span<BYTE const> scalar_bytes(T const& value) {
  return std::span<BYTE const>(reinterpret_cast<BYTE const*>(&value), sizeof(T));
}

// A tag plus the FITAG the public API hands out for it. The handle addresses the
// payload, so it stays valid for the owning dib's lifetime and the caller never
// frees it (§2.10).
struct StoredTag {
  Tag tag;
  FITAG handle;

  StoredTag() { handle.data = &tag; }
};

// Tag handles point at the payload the dib owns.
inline Tag const* stored_tag(FITAG const* tag) {
  if (tag == nullptr || tag->data == nullptr) {
    return nullptr;
  }
  return static_cast<Tag const*>(tag->data);
}

// The tags of one model, keyed by name. Tags are individually heap-allocated
// because their handles are public output: appending must not move an existing one.
class TagSet {
 public:
  TagSet() = default;
  TagSet(TagSet const& source);
  TagSet& operator=(TagSet const& source);
  TagSet(TagSet&&) noexcept = default;
  TagSet& operator=(TagSet&&) noexcept = default;

  // Adds a tag, or gives an existing key the new payload and keeps its handle.
  void add(std::string_view key, FREE_IMAGE_MDTYPE value_type, std::span<BYTE const> value);
  // The handle is the dib's own; handing it out is not a mutation of the payload.
  FITAG const* find(std::string_view key) const;

 private:
  std::vector<std::unique_ptr<StoredTag>> tags_;
};

// Bitmap ---------------------------------------------------------------

// One pixel's channels as the 16-bit DIB layouts expand them (§1.1). The 6-bit
// green of 565 scales by 0xFF/0x3F, every 5-bit component by 0xFF/0x1F.
struct Channels {
  BYTE blue;
  BYTE green;
  BYTE red;
};

constexpr Channels expand16(WORD pixel, bool rgb565) {
  if (rgb565) {
    return Channels{
        static_cast<BYTE>(((pixel & FI16_565_BLUE_MASK) >> FI16_565_BLUE_SHIFT) * 0xFF / 0x1F),
        static_cast<BYTE>(((pixel & FI16_565_GREEN_MASK) >> FI16_565_GREEN_SHIFT) * 0xFF / 0x3F),
        static_cast<BYTE>(((pixel & FI16_565_RED_MASK) >> FI16_565_RED_SHIFT) * 0xFF / 0x1F)};
  }
  return Channels{
      static_cast<BYTE>(((pixel & FI16_555_BLUE_MASK) >> FI16_555_BLUE_SHIFT) * 0xFF / 0x1F),
      static_cast<BYTE>(((pixel & FI16_555_GREEN_MASK) >> FI16_555_GREEN_SHIFT) * 0xFF / 0x1F),
      static_cast<BYTE>(((pixel & FI16_555_RED_MASK) >> FI16_555_RED_SHIFT) * 0xFF / 0x1F)};
}

// DIB words are stored in host byte order, unlike a codec's on-disk fields; the memcpy
// keeps the access free of the alignment and aliasing UB of a cast.
inline WORD load16(BYTE const* bits) {
  WORD value = 0;
  std::memcpy(&value, bits, sizeof(WORD));
  return value;
}

inline void store16(BYTE* bits, WORD value) { std::memcpy(bits, &value, sizeof(WORD)); }

// Palette index of pixel x in a 1/4/8bpp row: the high nibble and the high bit
// come first, independent of host byte order (§1.1).
constexpr unsigned palette_index(std::span<BYTE const> row, unsigned x, int bpp) {
  switch (bpp) {
    case 1:
      return (row[x >> 3] & (0x80 >> (x & 0x07))) != 0 ? 1u : 0u;
    case 4: {
      unsigned const shift = (1 - x % 2) << 2;
      return (row[x >> 1] & (0x0Fu << shift)) >> shift;
    }
    default:
      return row[x];
  }
}

// The device-independent bitmap below a FIBITMAP handle. Bottom-up: scanline 0 is
// the visual bottom row (§1.1). The state is private; the accessors below are the
// interface every layer reads it through.
class Dib {
 public:
  // Precondition: supported geometry, i.e. positive dimensions and bpp in
  // {1, 4, 8, 16, 24, 32}; Dib::make() is what enforces it.
  Dib(int width, int height, int bpp, unsigned red = 0, unsigned green = 0, unsigned blue = 0);

  // A clone carries the pixels, palette, transparency table and tags of its source
  // (§2.4: ConvertTo*/Rescale hand the tags along with the pixels).
  Dib(Dib const&) = default;
  Dib& operator=(Dib const&) = default;
  Dib(Dib&&) noexcept = default;
  Dib& operator=(Dib&&) noexcept = default;

  // Allocates a dib, or returns null for unsupported geometry or bit depth.
  static std::unique_ptr<Dib>
  make(int width, int height, int bpp, unsigned red_mask = 0, unsigned green_mask = 0, unsigned blue_mask = 0);

  // Classic CalculateLine: visible bytes per row, 4-byte aligned (§1.1).
  static constexpr unsigned row_bytes(int width, int bpp) { return (((width * bpp + 7) >> 3) + 3) & ~3u; }

  FREE_IMAGE_TYPE type() const { return type_; }
  int width() const { return width_; }
  int height() const { return height_; }
  int bpp() const { return bpp_; }
  unsigned red_mask() const { return red_mask_; }
  unsigned green_mask() const { return green_mask_; }
  unsigned blue_mask() const { return blue_mask_; }
  unsigned line() const { return line_; }
  unsigned pitch() const { return pitch_; }

  std::span<BYTE> pixels() { return pixels_; }
  std::span<BYTE const> pixels() const { return pixels_; }

  // The mutable overloads are the <=8bpp loaders' fill surface: a loader populates
  // the palette and transparency table the constructor already sized.
  std::span<RGBQUAD> palette() { return palette_; }
  std::span<RGBQUAD const> palette() const { return palette_; }
  std::span<BYTE> transparency() { return transparency_; }
  std::span<BYTE const> transparency() const { return transparency_; }

  bool is_transparent() const { return transparent_; }
  void set_transparent() { transparent_ = true; }

  bool has_pixels() const { return !pixels_.empty(); }
  bool is_bitmap() const { return type_ == FIT_BITMAP; }
  bool is_indexed() const { return bpp_ == 1 || bpp_ == 4 || bpp_ == 8; }
  bool is_rgb565() const {
    return red_mask_ == FI16_565_RED_MASK && green_mask_ == FI16_565_GREEN_MASK && blue_mask_ == FI16_565_BLUE_MASK;
  }
  bool is_rgb555() const {
    return red_mask_ == FI16_555_RED_MASK && green_mask_ == FI16_555_GREEN_MASK && blue_mask_ == FI16_555_BLUE_MASK;
  }

  // Precondition: has_pixels() and 0 <= y < height. The span is the visible row;
  // the bytes between line and pitch are padding and belong to no pixel.
  std::span<BYTE> row(int y) {
    return {pixels_.data() + static_cast<std::size_t>(pitch_) * static_cast<std::size_t>(y), line_};
  }
  std::span<BYTE const> row(int y) const {
    return {pixels_.data() + static_cast<std::size_t>(pitch_) * static_cast<std::size_t>(y), line_};
  }

  // Clamped so a codec that filled fewer entries than bpp implies cannot read past
  // the stored palette.
  RGBQUAD color_at(unsigned index) const;
  // An ordered greyscale ramp and its direction, or nullopt when the palette is not
  // one - a flat ramp is an unordered palette image (§2.8).
  std::optional<Ramp> greyscale_ramp() const;

  void add(FREE_IMAGE_MDMODEL model, std::string_view key, FREE_IMAGE_MDTYPE value_type, std::span<BYTE const> value);
  // The tag the dib keeps for its own lifetime, or null. The handle crosses the C ABI
  // only through the public marshaling function, which restores the classic shape.
  FITAG const* find(FREE_IMAGE_MDMODEL model, std::string_view key) const;
  void adopt_tags_from(Dib const& source);

 private:
  FREE_IMAGE_TYPE type_ = FIT_BITMAP;
  int width_ = 0;
  int height_ = 0;
  int bpp_ = 0;
  unsigned red_mask_ = 0;
  unsigned green_mask_ = 0;
  unsigned blue_mask_ = 0;
  unsigned line_ = 0;
  unsigned pitch_ = 0;
  std::vector<BYTE> pixels_;                      // pitch * height
  std::vector<RGBQUAD> palette_;                  // bpp <= 8; length is the entry count
  std::array<BYTE, kMaxPalette> transparency_{};  // per-index alpha, 255 == opaque
  bool transparent_ = false;
  std::array<TagSet, kModelCount> tags_;
};

}  // namespace fi
