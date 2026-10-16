// Conversion, raw-bits and pixel-access internals: everything that reshapes or
// reads a dib's pixels below the C ABI. Audience: the marshal TU and resampling,
// which delegates its same-size depth change; each function takes a reference to
// the already-marshaled Dib the entry-point validation ladder accepted (a rejected
// handle never reaches this layer) and reproduces classic FreeImage 3.18 semantics
// as pinned by the contract: §1.1 (bottom-up DIB, 4-byte rows, BGR byte order),
// §1.3 (raw-bits import/export), §2.4 (conversions, premultiply), §2.5 (pixel
// access), §2.6 (raw-bits guards).
#pragma once

#include <memory>

#include "dib.h"

namespace fi {
namespace convert {

// Shared body of ConvertTo24Bits / ConvertTo32Bits: copy the source when it already
// has the requested depth, else allocate the target with classic's masks, carry the
// tags along and convert row by row (§2.4).
std::unique_ptr<Dib> to_dib(Dib const& source, int target_bpp);

// Shared body of the ConvertFromRawBits pair (§1.3), or null for the input classic
// rejects: a null buffer, non-positive geometry or pitch, a non-bitmap type, or a
// bit depth outside the supported set.
std::unique_ptr<Dib> from_raw_bits(
    BOOL copySource,
    BYTE* bits,
    FREE_IMAGE_TYPE type,
    int width,
    int height,
    int pitch,
    unsigned bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask,
    BOOL topdown);

// ConvertToRawBits: classic no-ops without pixels or without a destination, and
// combinations its switch has no case for leave the destination row untouched (§2.6).
void to_raw_bits(
    Dib const& source,
    BYTE* bits,
    int pitch,
    unsigned bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask,
    BOOL topdown);

// Integer premultiply with +127 rounding; false unless the dib is 32bpp (§2.4).
bool premultiply_with_alpha(Dib& dib);

// Pixel access (§2.5): bounds- and depth-checked per classic; false for a null value
// pointer or an out-of-range pixel. The value pointer is an out parameter whose null
// rejection is itself published classic behavior.
bool get_pixel_index(Dib const& dib, unsigned x, unsigned y, BYTE* value);
bool set_pixel_index(Dib& dib, unsigned x, unsigned y, BYTE const* value);
bool get_pixel_color(Dib const& dib, unsigned x, unsigned y, RGBQUAD* value);
bool set_pixel_color(Dib& dib, unsigned x, unsigned y, RGBQUAD const* value);

}  // namespace convert
}  // namespace fi
