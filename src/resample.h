// Resampling: the separable filter pass behind FreeImage_Rescale, written to the
// contract's sampling spec (§2.8). Audience: the marshal TU; the source is the
// already-marshaled Dib the entry-point validation ladder accepted.
#pragma once

#include <memory>

#include "dib.h"

namespace fi {

// Resamples to the destination geometry, or null for non-positive dimensions or an
// unknown filter. The destination depth rules, the same-size copy path and the
// metadata carry-over are the function's, per §2.8.
std::unique_ptr<Dib> rescale(Dib const& src, int dst_width, int dst_height, FREE_IMAGE_FILTER filter);

}  // namespace fi
