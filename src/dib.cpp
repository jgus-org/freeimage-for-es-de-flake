// The dib and its tag model: construction and geometry allocation (§1.1), the
// ordered-greyscale test that drives Rescale's depth choice (§2.8), and the tag
// stores a dib owns and hands along on clone and conversion (§2.10, §2.4). A
// missing tag allocates nothing (§2.10); tags ride the pixels through the Convert
// and Rescale paths on the way to the ES-DE call sites.
#include "dib.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace fi {
namespace {

// A clone gets a fresh handle; the payload is the dib's own (§2.10).
std::unique_ptr<StoredTag> copied_tag(StoredTag const& source) {
  auto stored = std::make_unique<StoredTag>();
  stored->tag = source.tag;
  return stored;
}

}  // namespace

TagSet::TagSet(TagSet const& source) {
  tags_.reserve(source.tags_.size());
  for (auto const& stored : source.tags_) {
    tags_.push_back(copied_tag(*stored));
  }
}

TagSet& TagSet::operator=(TagSet const& source) {
  if (this != &source) {
    *this = TagSet(source);
  }
  return *this;
}

void TagSet::add(std::string_view key, FREE_IMAGE_MDTYPE value_type, std::span<BYTE const> value) {
  auto const existing = std::ranges::find_if(tags_, [key](auto const& stored) { return stored->tag.key == key; });
  if (existing != tags_.end()) {
    Tag& tag = (*existing)->tag;
    tag.type = value_type;
    tag.value.assign(value.begin(), value.end());
    return;
  }
  auto stored = std::make_unique<StoredTag>();
  stored->tag.key = key;
  stored->tag.type = value_type;
  stored->tag.value.assign(value.begin(), value.end());
  tags_.push_back(std::move(stored));
}

FITAG const* TagSet::find(std::string_view key) const {
  auto const found = std::ranges::find_if(tags_, [key](auto const& stored) { return stored->tag.key == key; });
  return found == tags_.end() ? nullptr : &(*found)->handle;
}

Dib::Dib(int width, int height, int bpp, unsigned red, unsigned green, unsigned blue)
    : width_(width),
      height_(height),
      bpp_(bpp),
      red_mask_(red),
      green_mask_(green),
      blue_mask_(blue),
      line_(row_bytes(width, bpp)),
      pitch_(row_bytes(width, bpp)) {
  pixels_.assign(static_cast<std::size_t>(pitch_) * static_cast<std::size_t>(height_), 0);
  if (bpp <= 8) {
    // The palette's length IS the entry count: the greyscale test and every codec
    // read it as the number of entries the image owns.
    palette_.assign(static_cast<std::size_t>(1) << static_cast<unsigned>(bpp), RGBQUAD{});
  }
  transparency_.fill(255);
}

RGBQUAD Dib::color_at(unsigned index) const {
  if (palette_.empty()) {
    return RGBQUAD{};
  }
  return index < palette_.size() ? palette_[index] : palette_.back();
}

std::optional<Ramp> Dib::greyscale_ramp() const {
  if (bpp_ > 8 || palette_.size() < 2) {
    return std::nullopt;
  }
  bool ascending = false;
  bool descending = false;
  for (std::size_t i = 1; i < palette_.size(); ++i) {
    RGBQUAD const& color = palette_[i];
    if (color.rgbRed != color.rgbGreen || color.rgbGreen != color.rgbBlue) {
      return std::nullopt;
    }
    if (color.rgbRed > palette_[i - 1].rgbRed) {
      ascending = true;
    } else if (color.rgbRed < palette_[i - 1].rgbRed) {
      descending = true;
    }
  }
  // A flat ramp is an unordered palette image, not an ordered greyscale mapping.
  if (ascending == descending) {
    return std::nullopt;
  }
  return descending ? Ramp::Inverted : Ramp::Normal;
}

void Dib::add(
    FREE_IMAGE_MDMODEL model,
    std::string_view key,
    FREE_IMAGE_MDTYPE value_type,
    std::span<BYTE const> value) {
  int const slot = model_slot(model);
  if (slot >= 0) {
    tags_[static_cast<std::size_t>(slot)].add(key, value_type, value);
  }
}

FITAG const* Dib::find(FREE_IMAGE_MDMODEL model, std::string_view key) const {
  int const slot = model_slot(model);
  return slot >= 0 ? tags_[static_cast<std::size_t>(slot)].find(key) : nullptr;
}

void Dib::adopt_tags_from(Dib const& source) { tags_ = source.tags_; }

std::unique_ptr<Dib>
Dib::make(int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {
  if (width <= 0 || height <= 0 || bpp <= 0) {
    return nullptr;
  }
  return std::make_unique<Dib>(width, height, bpp, red_mask, green_mask, blue_mask);
}

}  // namespace fi
