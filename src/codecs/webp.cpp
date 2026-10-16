// WebP codec (CODECS-owned) over libwebp's public decode/encode API. The
// library works on whole images, so a load reads the stream into memory first,
// exactly as classic does, and a save encodes through a writer bound to the
// caller's FreeImageIO (design §2.9). Decoded output is MODE_BGR at 24bpp or
// MODE_BGRA at 32bpp when the bitstream carries alpha (design §3), and the
// library's top-down rows land on scanline height-1-y of the bottom-up DIB.
//
// Saving mirrors classic's flag mapping: WEBP_LOSSLESS selects lossless,
// otherwise the low seven bits are the quality (clamped to 100) with 0 meaning
// the library default of 75, and method is always 6 (design §3, §4.2).
//
// classic feeds the encoder by flipping the caller's dib in place and flipping
// it back afterwards, which leaks a mutated dib whenever the encoder fails.
// Copying the rows in reverse instead keeps the caller's image untouched on
// every path and avoids writing to caller-owned memory at all.
#include "codecs/webp.h"

#include <webp/decode.h>
#include <webp/encode.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

namespace fi {
namespace {

inline BYTE* Scanline(Dib& dib, int y) { return dib.row(y).data(); }

inline BYTE const* Scanline(Dib const& dib, int y) { return dib.row(y).data(); }

// Reads the rest of the stream into memory, which is what libwebp's
// whole-image API needs. Empty means there was nothing to read or it could not
// be read.
std::vector<uint8_t> Slurp(FreeImageIO const& io, fi_handle handle) {
  long const start = io.tell_proc(handle);
  if (start < 0 || io.seek_proc(handle, 0, SEEK_END) != 0) {
    return {};
  }
  long const end = io.tell_proc(handle);
  if (io.seek_proc(handle, start, SEEK_SET) != 0) {
    return {};
  }
  std::size_t const bytes = static_cast<std::size_t>(end - start);
  if (bytes == 0) {
    return {};
  }
  std::vector<uint8_t> buffer(bytes);
  if (io.read_proc(buffer.data(), 1, static_cast<unsigned>(bytes), handle) != bytes) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: truncated bitstream");
    return {};
  }
  return buffer;
}

struct IoBridge {
  FreeImageIO* io;
  fi_handle handle;
};

int WriteEncoded(uint8_t const* data, size_t data_size, WebPPicture const* picture) {
  IoBridge const& bridge = *static_cast<IoBridge const*>(picture->custom_ptr);
  if (data_size == 0 || bridge.io->write_proc == nullptr) {
    return 0;
  }
  return bridge.io->write_proc(const_cast<uint8_t*>(data), 1, static_cast<unsigned>(data_size), bridge.handle) ==
                 data_size
             ? 1
             : 0;
}

// Copies the DIB into a top-down BGR(A) buffer for the encoder.
std::vector<uint8_t> PackTopDown(Dib const& dib, std::size_t sample_size) {
  std::size_t const bytes = static_cast<std::size_t>(dib.width()) * sample_size;
  std::vector<uint8_t> packed(bytes * static_cast<std::size_t>(dib.height()));
  for (int y = 0; y < dib.height(); ++y) {
    // Encoder row 0 is the visual top, which is the DIB's last scanline.
    std::memcpy(packed.data() + static_cast<std::size_t>(y) * bytes, Scanline(dib, dib.height() - y - 1), bytes);
  }
  return packed;
}

// Releases the planes libwebp allocates inside a WebPPicture once it has been
// initialised.
struct PictureFree {
  void operator()(WebPPicture* picture) const { WebPPictureFree(picture); }
};

bool EncodeImage(FreeImageIO& io, fi_handle handle, Dib& dib, int flags) {
  if (dib.bpp() != 24 && dib.bpp() != 32) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: a %d bpp image is not savable as WebP", dib.bpp());
    return false;
  }
  if (dib.width() > WEBP_MAX_DIMENSION || dib.height() > WEBP_MAX_DIMENSION) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: %dx%d exceeds the format limit", dib.width(), dib.height());
    return false;
  }

  WebPConfig config;
  if (WebPConfigInit(&config) == 0) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: library version mismatch");
    return false;
  }
  config.method = 6;
  if ((flags & WEBP_LOSSLESS) == WEBP_LOSSLESS) {
    config.lossless = 1;
  } else {
    int const quality = flags & 0x7F;
    if (quality > 0) {
      config.quality = quality > 100 ? 100.0F : static_cast<float>(quality);
    }
  }
  if (WebPValidateConfig(&config) == 0) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: the encoder rejected the requested settings");
    return false;
  }

  std::size_t const sample_size = dib.bpp() / 8;
  std::vector<uint8_t> const packed = PackTopDown(dib, sample_size);

  IoBridge const bridge = {&io, handle};
  WebPPicture picture;
  if (WebPPictureInit(&picture) == 0) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: cannot initialise the input picture");
    return false;
  }
  std::unique_ptr<WebPPicture, PictureFree> const owned_picture(&picture);
  picture.width = dib.width();
  picture.height = dib.height();
  picture.use_argb = config.lossless != 0;
  picture.writer = &WriteEncoded;
  picture.custom_ptr = const_cast<IoBridge*>(&bridge);
  if ((sample_size == 3 ? WebPPictureImportBGR(&picture, packed.data(), dib.width() * 3)
                        : WebPPictureImportBGRA(&picture, packed.data(), dib.width() * 4)) == 0) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: cannot import the image");
    return false;
  }
  if (WebPEncode(&config, &picture) == 0) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: encoding failed with error code %d", picture.error_code);
    return false;
  }
  return true;
}

// Releases the buffer WebPDecode* hands back; every exit from a decode uses it,
// including one whose dimensions disagree with the requested bitmap.
struct WebPFreeBuffer {
  void operator()(uint8_t* data) const { WebPFree(data); }
};

}  // namespace

std::string_view WebpCodec::format_name() const { return "WEBP"; }

std::string_view WebpCodec::extensions() const { return "webp"; }

std::string_view WebpCodec::description() const { return "WebP Image"; }

bool WebpCodec::validate(FreeImageIO const& io, fi_handle handle) const {
  BYTE signature[12] = {0};
  if (io.read_proc(signature, 1, sizeof(signature), handle) != sizeof(signature)) {
    return false;
  }
  bool const riff = signature[0] == 'R' && signature[1] == 'I' && signature[2] == 'F' && signature[3] == 'F';
  bool const webp = signature[8] == 'W' && signature[9] == 'E' && signature[10] == 'B' && signature[11] == 'P';
  return riff && webp;
}

std::unique_ptr<Dib> WebpCodec::load(FreeImageIO& io, fi_handle handle, int /*page*/, int /*flags*/) const {
  if (handle == nullptr) {
    return nullptr;
  }
  std::vector<uint8_t> const bitstream = Slurp(io, handle);
  if (bitstream.empty()) {
    return nullptr;
  }

  WebPBitstreamFeatures features;
  if (WebPGetFeatures(bitstream.data(), bitstream.size(), &features) != VP8_STATUS_OK) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: the bitstream could not be parsed");
    return nullptr;
  }

  int const bpp = features.has_alpha ? 32 : 24;
  std::unique_ptr<Dib> dib =
      Dib::make(features.width, features.height, bpp, FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK, FI_RGBA_BLUE_MASK);
  if (dib == nullptr) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: cannot allocate the bitmap");
    return nullptr;
  }
  Dib& d = *dib;

  int width = 0;
  int height = 0;
  std::unique_ptr<uint8_t, WebPFreeBuffer> const decoded(
      features.has_alpha ? WebPDecodeBGRA(bitstream.data(), bitstream.size(), &width, &height)
                         : WebPDecodeBGR(bitstream.data(), bitstream.size(), &width, &height));
  if (decoded == nullptr || width != d.width() || height != d.height()) {
    FreeImage_OutputMessageProc(FIF_WEBP, "WebP: decoding failed");
    return nullptr;
  }

  std::size_t const line = static_cast<std::size_t>(width) * (bpp / 8);
  for (int y = 0; y < height; ++y) {
    // The decoded buffer is top-down; scanline height-1-y is its DIB home.
    std::memcpy(d.row(height - y - 1).data(), decoded.get() + static_cast<std::size_t>(y) * line, line);
  }
  return dib;
}

bool WebpCodec::save(FreeImageIO& io, Dib& dib, fi_handle handle, int flags) const {
  if (!dib.has_pixels() || !dib.is_bitmap() || handle == nullptr) {
    return false;
  }
  return EncodeImage(io, handle, dib, flags);
}

int WebpCodec::page_count(FreeImageIO& /*io*/, fi_handle /*handle*/) const { return 1; }

WebpCodec const webp_codec;

}  // namespace fi
