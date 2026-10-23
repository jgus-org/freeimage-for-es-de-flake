// The codec seam: the abstract format plugin every codec implements and the
// registry dispatches through. Audience: codec translation units (which implement
// it) and the aggregation layer (which holds Codec const* to it); no function
// pointers and no C-ABI handle types cross this interface - a load hands back an
// owning Dib and the marshaling layer wraps the handle.
//
// All procedures run against a caller-supplied FreeImageIO so the same codec path
// serves files, memory and custom streams (§2.3, §2.9). Codecs are stateless:
// every scratch buffer is call-local, which is what satisfies the
// concurrent-distinct-objects requirement (§2.11).
#pragma once

#include <FreeImage.h>

#include <memory>
#include <string_view>

#include "dib.h"

namespace fi {

class Codec {
 public:
  virtual ~Codec() = default;

  // Identity used by GetFIFFromFilename and the extension table.
  virtual std::string_view format_name() const = 0;  // e.g. "PNG"
  virtual std::string_view extensions() const = 0;   // comma-separated lowercase list, e.g. "png,jng"
  virtual std::string_view description() const = 0;  // e.g. "Portable Network Graphics"

  // Detection: must leave the stream position unchanged (seek back to 0). The probe
  // reads through the table's function pointers, which a const object permits.
  virtual bool validate(FreeImageIO const& io, fi_handle handle) const = 0;
  // Decode a page (page == -1 for a single-frame load), or null on failure.
  virtual std::unique_ptr<Dib> load(FreeImageIO& io, fi_handle handle, int page, int flags) const = 0;
  // Encode a single frame; false for an unsupported dib.
  virtual bool save(FreeImageIO& io, Dib& dib, fi_handle handle, int flags) const = 0;
  // Number of pages (GIF frames); 1 for single-frame formats.
  virtual int page_count(FreeImageIO& io, fi_handle handle) const = 0;
};

}  // namespace fi
