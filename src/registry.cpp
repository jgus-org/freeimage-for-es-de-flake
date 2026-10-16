// Format registry: the aggregation point. This TU includes every codec header, and
// no codec includes it back; Load/Save/detect resolve a classic FIF to its codec and
// run it against the caller's - or a file/memory-backed - FreeImageIO, so one codec
// path serves files, memory and custom streams (§2.3).
#include "registry.h"

#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#include "codecs/bmp.h"
#include "codecs/gif.h"
#include "codecs/jpeg.h"
#include "codecs/png.h"
#include "codecs/webp.h"

namespace fi {
namespace registry {
namespace {

// Sparse table indexed by the classic FIF value; only the five implemented formats are
// registered.
constexpr int kRegistrySize = FIF_WEBP + 1;

constexpr Entry kRegistry[kRegistrySize] = {
    {FIF_BMP, &bmp_codec},
    {FIF_UNKNOWN, nullptr},
    {FIF_JPEG, &jpeg_codec},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_PNG, &png_codec},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_GIF, &gif_codec},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_UNKNOWN, nullptr},
    {FIF_WEBP, &webp_codec},
};

// Detection walks the implemented ids ascending, so the first registered hit wins (§2.3).
constexpr FREE_IMAGE_FORMAT kSupportedFifs[] = {FIF_BMP, FIF_JPEG, FIF_PNG, FIF_GIF, FIF_WEBP};

// Lower-cased substring after the last '.' (or the whole name if no dot).
std::string extension_of(char const* filename) {
  char const* dot = std::strrchr(filename, '.');
  std::string ext = dot != nullptr ? dot + 1 : filename;
  for (char& c : ext) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return ext;
}

bool extension_matches(std::string_view list, std::string_view ext) {
  for (std::size_t pos = 0; pos < list.size();) {
    std::size_t const comma = list.find(',', pos);
    std::size_t const end = comma != std::string_view::npos ? comma : list.size();
    if (list.substr(pos, end - pos) == ext) {
      return true;
    }
    pos = end + 1;
  }
  return false;
}

}  // namespace

Entry const* registered(FREE_IMAGE_FORMAT fif) {
  int const id = static_cast<int>(fif);
  if (id < 0 || id >= kRegistrySize) {
    return nullptr;
  }
  Entry const& entry = kRegistry[id];
  return entry.codec != nullptr ? &entry : nullptr;
}

FREE_IMAGE_FORMAT detect(FreeImageIO const& io, fi_handle handle) {
  for (FREE_IMAGE_FORMAT const fif : kSupportedFifs) {
    Entry const* entry = registered(fif);
    if (entry == nullptr) {
      continue;
    }
    io.seek_proc(handle, 0, SEEK_SET);
    if (entry->codec->validate(io, handle)) {
      io.seek_proc(handle, 0, SEEK_SET);
      return fif;
    }
  }
  io.seek_proc(handle, 0, SEEK_SET);
  return FIF_UNKNOWN;
}

FREE_IMAGE_FORMAT from_extension(char const* filename) {
  if (filename == nullptr) {
    return FIF_UNKNOWN;
  }
  std::string const ext = extension_of(filename);
  for (FREE_IMAGE_FORMAT const fif : kSupportedFifs) {
    Entry const* entry = registered(fif);
    if (entry != nullptr && extension_matches(entry->codec->extensions(), ext)) {
      return fif;
    }
  }
  return FIF_UNKNOWN;
}

std::unique_ptr<Dib> load(FREE_IMAGE_FORMAT fif, FreeImageIO& io, fi_handle handle, int page, int flags) {
  Entry const* entry = registered(fif);
  if (entry == nullptr) {
    return nullptr;
  }
  return entry->codec->load(io, handle, page, flags);
}

bool save(FREE_IMAGE_FORMAT fif, Dib& dib, FreeImageIO& io, fi_handle handle, int flags) {
  Entry const* entry = registered(fif);
  return entry != nullptr && entry->codec->save(io, dib, handle, flags);
}

}  // namespace registry
}  // namespace fi
