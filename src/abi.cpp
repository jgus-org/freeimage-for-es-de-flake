// The handle seam: wrapping a dib into the published FIBITMAP shape and back.
// Definitions in declaration order; the casts and validation-ladder helpers stay
// inline in abi.h because every marshal entry point runs them per call.
#include "abi.h"

#include <memory>

namespace fi {

void DibDeleter::operator()(FIBITMAP* handle) const noexcept {
  if (handle == nullptr) {
    return;
  }
  delete static_cast<Dib*>(handle->data);
  delete handle;
}

FIBITMAP_ptr adopt_bitmap(std::unique_ptr<Dib> dib) {
  if (dib == nullptr) {
    return nullptr;
  }
  FIBITMAP_ptr wrapper(new FIBITMAP);
  wrapper->data = dib.release();
  return wrapper;
}

FIBITMAP_ptr make_bitmap(int width, int height, int bpp, unsigned red_mask, unsigned green_mask, unsigned blue_mask) {
  return adopt_bitmap(Dib::make(width, height, bpp, red_mask, green_mask, blue_mask));
}

FIBITMAP_ptr make_bitmap_copy(FIBITMAP const* handle) {
  Dib const* source = deref(handle);
  if (source == nullptr) {
    return nullptr;
  }
  return adopt_bitmap(std::make_unique<Dib>(*source));
}

}  // namespace fi
