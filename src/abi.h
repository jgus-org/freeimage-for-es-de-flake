// The C ABI handle seam: the boundary between a FIBITMAP handle as the public API
// publishes it and the Dib the library actually owns. Audience: the marshal TU -
// FreeImage.cpp, the only file that answers the extern "C" surface - and the
// multi-page container, which holds the locked pages' handles for the classic
// lock/unlock lifetime; never a codec.
//
// Ownership: below the C ABI a payload is held by a unique_ptr, and an extern "C"
// entry point is the only place that crosses the boundary. A function that returns
// a handle builds an owning pointer and release()s it at the return; a free
// function re-adopts the handle so the single delete runs there. Nothing else
// allocates or frees a handle.
#pragma once

#include <FreeImage.h>

#include <memory>

#include "dib.h"

namespace fi {

// Owns a FIBITMAP and the Dib it carries. The deleter is the only place a dib's
// storage is released, so every internal path - including an early return or a
// plugin that fails halfway - is leak-free without a matching free call.
struct DibDeleter {
  void operator()(FIBITMAP* handle) const noexcept;
};
using FIBITMAP_ptr = std::unique_ptr<FIBITMAP, DibDeleter>;

// Wraps a dib already built below the ABI into the handle shape; null in, null out.
// An ownership transfer rather than a create factory, so no make_* spelling.
FIBITMAP_ptr adopt_bitmap(std::unique_ptr<Dib> dib);
// Allocates a dib, or returns null for unsupported geometry or bit depth.
FIBITMAP_ptr
make_bitmap(int width, int height, int bpp, unsigned red_mask = 0, unsigned green_mask = 0, unsigned blue_mask = 0);
// Deep copy of a handle's dib (§2.4); null for a null handle.
FIBITMAP_ptr make_bitmap_copy(FIBITMAP const* handle);

// The one cast from a caller handle to its payload; null handle -> null payload.
inline Dib* deref(FIBITMAP* handle) noexcept { return handle != nullptr ? static_cast<Dib*>(handle->data) : nullptr; }
inline Dib const* deref(FIBITMAP const* handle) noexcept {
  return handle != nullptr ? static_cast<Dib const*>(handle->data) : nullptr;
}

// Classic FreeImage_HasPixels (§2.1 null guards): an empty pixel array means the
// dib carries no image, which is what FIF_LOAD_NOPIXELS leaves behind.
inline bool has_pixels(Dib const* dib) noexcept { return dib != nullptr && dib->has_pixels(); }

// Entry-point validation ladder: these are the only checks the public surface makes
// on a handle beyond null (§2.1). A null result means "no image to work on", and the
// entry point checks for it before handing a Dib reference to any internal callee.
inline Dib const* readable_bitmap(FIBITMAP const* handle) noexcept {
  Dib const* dib = deref(handle);
  return has_pixels(dib) && dib->is_bitmap() ? dib : nullptr;
}
inline Dib* writable_bitmap(FIBITMAP* handle) noexcept {
  Dib* dib = deref(handle);
  return has_pixels(dib) && dib->is_bitmap() ? dib : nullptr;
}

// Re-adopts a handle released to a caller so its single delete runs (§2.1).
inline void dispose_bitmap(FIBITMAP* handle) { FIBITMAP_ptr const ownership(handle); }

}  // namespace fi
