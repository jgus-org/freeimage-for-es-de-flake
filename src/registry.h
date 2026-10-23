// The registry: the aggregation layer above the codecs. Audience: the marshal TU
// that resolves a classic FIF to a codec (load/save dispatch, format queries) and
// the multi-page container, which keeps a codec for an open stream's lifetime. The
// registry table itself lives in registry.cpp, which includes the codec headers -
// dependency direction is strict: codecs depend on dib.h and codec.h only, never
// on this header or on each other.
#pragma once

#include <FreeImage.h>

#include <memory>

#include "codec.h"

namespace fi {
namespace registry {

// Sparse registry entry: the classic FIF value is the index, so ids keep their
// numeric identity and detection iterates ascending by id (§2.3).
struct Entry {
  FREE_IMAGE_FORMAT fif;
  Codec const* codec;
};

// The entry for a registered fif, or nullptr when fif is not one of the five
// implemented formats.
Entry const* registered(FREE_IMAGE_FORMAT fif);
// Iterates the registry in ascending fif order, invoking each codec's validate.
// Leaves the stream positioned at 0. Returns FIF_UNKNOWN when nothing matches.
FREE_IMAGE_FORMAT detect(FreeImageIO const& io, fi_handle handle);
// Extension table lookup over the five registered codecs.
FREE_IMAGE_FORMAT from_extension(char const* filename);
// Decodes one page with the fif's codec (page == -1 for the usual single-frame
// load); null for an unregistered format or a codec failure.
std::unique_ptr<Dib> load(FREE_IMAGE_FORMAT fif, FreeImageIO& io, fi_handle handle, int page, int flags);
// Encodes one frame with the fif's codec; false for an unregistered format.
bool save(FREE_IMAGE_FORMAT fif, Dib& dib, FreeImageIO& io, fi_handle handle, int flags);

}  // namespace registry
}  // namespace fi
