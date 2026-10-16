// The multi-page container: the state behind a FIMULTIBITMAP handle. Audience: the
// marshal TU; the container itself owns the open stream (when a filename open
// created it) and the locked pages' bitmap handles, so those handle types appear
// here by the container's nature, not as an API-shape leak.
#pragma once

#include <FreeImage.h>

#include <memory>

namespace fi {
namespace multipage {

struct MultiDeleter {
  void operator()(FIMULTIBITMAP* handle) const noexcept;
};
using FIMULTIBITMAP_ptr = std::unique_ptr<FIMULTIBITMAP, MultiDeleter>;

// Opens read-only over a caller stream (never closed here, §2.7); null for a null
// handle or a format that cannot load pages. The marshal entry point checks the
// public FreeImageIO pointer for null before handing the stream down as a reference.
FIMULTIBITMAP_ptr open_from_handle(FREE_IMAGE_FORMAT fif, FreeImageIO& io, fi_handle handle, int flags);
// Opens over a file this container owns; null for a null name, a create request
// (writing multi-page files is out of scope for this subset), a format that cannot
// load pages, or a failed open.
FIMULTIBITMAP_ptr
open_from_file(FREE_IMAGE_FORMAT fif, char const* filename, BOOL create_new, BOOL read_only, int flags);
int page_count(FIMULTIBITMAP* bitmap);
// Decodes the page and holds it until unlock; the returned handle is borrowed from
// the container. Null for an out-of-range or already-locked page (§2.7).
FIBITMAP* lock_page(FIMULTIBITMAP* bitmap, int page);
// Drops the container's hold on a page it locked; unlocking a pointer this
// container never locked is ignored.
void unlock_page(FIMULTIBITMAP* bitmap, FIBITMAP* data);
// Re-adopts the handle, discarding locked pages and closing an owned stream; false
// for a null handle (§2.7).
bool close(FIMULTIBITMAP* bitmap);

}  // namespace multipage
}  // namespace fi
