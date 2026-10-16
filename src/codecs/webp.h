#pragma once

#include "codec.h"

namespace fi {

struct WebpCodec final : Codec {
  std::string_view format_name() const override;
  std::string_view extensions() const override;
  std::string_view description() const override;
  bool validate(FreeImageIO const& io, fi_handle handle) const override;
  std::unique_ptr<Dib> load(FreeImageIO& io, fi_handle handle, int page, int flags) const override;
  bool save(FreeImageIO& io, Dib& dib, fi_handle handle, int flags) const override;
  int page_count(FreeImageIO& io, fi_handle handle) const override;
};

extern WebpCodec const webp_codec;

}  // namespace fi
