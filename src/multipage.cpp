// Multi-page interface: the animated-GIF surface ES-DE drives through
// OpenMultiBitmapFromHandle (§2.7). The header caches the caller's FreeImageIO by value
// plus the open-time load flags, never closes a caller-supplied handle, and keeps no
// page cache: every LockPage re-decodes, which is what ES-DE's re-open-per-render-frame
// pattern depends on. All state is per FIMULTIBITMAP, so distinct objects are safe to
// use from distinct threads (§2.11).
//
// Not implemented, by design: the page cache. Classic stores an UnlockPage
// (changed == TRUE) page in memory so a later LockPage of that page and a SaveMultiBitmap
// see the modification. SaveMultiBitmap functions are dropped from the shipped header and
// ES-DE always passes changed == FALSE, so the cache has no consumer here: a changed page
// is discarded and the modification is lost, and re-locking a page always re-decodes it.
#include "multipage.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "abi.h"
#include "io.h"
#include "registry.h"

namespace fi {
namespace multipage {
namespace {

// The state behind a FIMULTIBITMAP handle (§2.7).
struct Header {
  Codec const* codec = nullptr;
  FreeImageIO io{};
  fi_handle handle = nullptr;
  int load_flags = 0;
  int page_count = 0;
  // A filename-based open owns its FILE*; a handle-based open never closes the caller's
  // stream (§2.7).
  io::FILE_ptr owned_file;
  // Locked pages stay owned here, so a second LockPage of a page is refused and
  // CloseMultiBitmap discards whatever is still locked.
  std::vector<std::pair<FIBITMAP_ptr, int>> locked;
};

Header* header_of(FIMULTIBITMAP* bitmap) { return bitmap != nullptr ? static_cast<Header*>(bitmap->data) : nullptr; }

// Shared tail of both open paths: read the page count at the start of the stream, then
// hand out the wrapper.
FIMULTIBITMAP_ptr open_multi(Header&& header) {
  header.io.seek_proc(header.handle, 0, SEEK_SET);
  header.page_count = header.codec->page_count(header.io, header.handle);
  if (header.page_count < 0) {
    header.page_count = 0;
  }
  FIMULTIBITMAP_ptr wrapper(new FIMULTIBITMAP);
  wrapper->data = std::make_unique<Header>(std::move(header)).release();
  return wrapper;
}

// The checks both open paths share, or nullptr when the format cannot load pages.
Codec const* page_codec(FREE_IMAGE_FORMAT fif) {
  registry::Entry const* entry = registry::registered(fif);
  return entry != nullptr ? entry->codec : nullptr;
}

}  // namespace

void MultiDeleter::operator()(FIMULTIBITMAP* handle) const noexcept {
  if (handle == nullptr) {
    return;
  }
  delete static_cast<Header*>(handle->data);
  delete handle;
}

FIMULTIBITMAP_ptr open_from_handle(FREE_IMAGE_FORMAT fif, FreeImageIO& io, fi_handle handle, int flags) {
  if (handle == nullptr) {
    return nullptr;
  }
  Header header;
  header.codec = page_codec(fif);
  if (header.codec == nullptr) {
    return nullptr;
  }
  header.io = io;  // cached by value, as classic does
  header.handle = handle;
  header.load_flags = flags;  // every LockPage load runs with these flags
  return open_multi(std::move(header));
}

FIMULTIBITMAP_ptr
open_from_file(FREE_IMAGE_FORMAT fif, char const* filename, BOOL create_new, BOOL read_only, int flags) {
  if (filename == nullptr || create_new == TRUE) {
    return nullptr;  // writing a multi-page file is out of scope for this subset
  }
  Codec const* codec = page_codec(fif);
  if (codec == nullptr) {
    return nullptr;
  }
  io::FILE_ptr file = io::open_file(filename, read_only == TRUE ? "rb" : "r+b");
  if (file == nullptr) {
    return nullptr;
  }
  Header header;
  header.codec = codec;
  header.io = io::file_io();
  header.handle = file.get();
  header.load_flags = flags;
  header.owned_file = std::move(file);
  return open_multi(std::move(header));
}

int page_count(FIMULTIBITMAP* bitmap) {
  Header const* header = header_of(bitmap);
  return header != nullptr ? header->page_count : 0;
}

FIBITMAP* lock_page(FIMULTIBITMAP* bitmap, int page) {
  Header* header = header_of(bitmap);
  if (header == nullptr || page < 0) {
    return nullptr;
  }
  auto const already_locked =
      std::ranges::find_if(header->locked, [page](auto const& locked) { return locked.second == page; });
  if (already_locked != header->locked.end()) {
    return nullptr;  // a page already locked cannot be locked twice
  }
  header->io.seek_proc(header->handle, 0, SEEK_SET);
  // Fresh decode on every lock; an out-of-range page is rejected by the codec.
  FIBITMAP_ptr page_dib = adopt_bitmap(header->codec->load(header->io, header->handle, page, header->load_flags));
  if (page_dib == nullptr) {
    return nullptr;
  }
  header->locked.emplace_back(std::move(page_dib), page);
  return header->locked.back().first.get();
}

void unlock_page(FIMULTIBITMAP* bitmap, FIBITMAP* data) {
  Header* header = header_of(bitmap);
  if (header == nullptr || data == nullptr) {
    return;
  }
  auto const locked =
      std::ranges::find_if(header->locked, [data](auto const& entry) { return entry.first.get() == data; });
  if (locked != header->locked.end()) {
    header->locked.erase(locked);  // dropping the owning pointer discards the page
  }
  // Unlocking a pointer this bitmap never locked is ignored (§2.7).
}

bool close(FIMULTIBITMAP* bitmap) {
  if (header_of(bitmap) == nullptr) {
    return false;
  }
  // Re-adopting the handle discards the locked pages, closes the stream only if this
  // open owned it, and frees the header (§2.7).
  FIMULTIBITMAP_ptr const ownership(bitmap);
  return true;
}

}  // namespace multipage
}  // namespace fi
