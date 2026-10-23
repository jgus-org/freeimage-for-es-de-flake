// JPEG codec (CODECS-owned) over libjpeg-turbo's public API. The
// library is driven by source and destination managers bound to the caller's
// FreeImageIO, so file, memory and custom streams share one path (design §2.9)
// and nothing here touches stdio. The library walks an image top-down, so its
// row k lands on scanline height-1-k of the bottom-up DIB, and its R,G,B sample
// order is exchanged with this build's B,G,R on both sides (design §1.1).
//
// Load produces 24bpp for a colour image and 8bpp with a greyscale palette for
// a single-component one; a CMYK source is converted to RGB by the library, so
// JPEG_CMYK is not implemented (design §3). Save accepts 24bpp, 32bpp with the
// alpha byte dropped, and 8bpp (grey ramp, inverted grey or palette) (design
// §3). The low seven flag bits are a direct quality value; the JPEG_QUALITY_*
// constants name the classic presets, and quality 75 applies when the low bits
// are zero (design §4.3).
//
// libjpeg reports fatal errors by calling error_exit, which longjmps to the pad
// installed by the frame that owns the error manager. That frame is the only
// place the jump can be caught, so it owns the library object and the scratch
// row through destructors that run when it is left either way; error_exit itself
// reports and jumps, never deallocates.
// jpeglib.h uses size_t and FILE without including <stddef.h> or <stdio.h>, so the
// standard headers must stay ahead of it; Google style re-sorts includes across blank
// lines, so the block is fenced to keep that order through the formatter.
// clang-format off
#include <csetjmp>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <jerror.h>
#include <jpeglib.h>

#include "codecs/jpeg.h"
// clang-format on

namespace fi {
namespace {

constexpr unsigned kIoBufferSize = 4096;

struct ErrorManager {
  jpeg_error_mgr pub;
  jmp_buf landing;
};

struct SourceManager {
  jpeg_source_mgr pub;
  FreeImageIO* io;
  fi_handle handle;
  JOCTET* buffer;
  boolean start_of_file;
};

struct DestinationManager {
  jpeg_destination_mgr pub;
  FreeImageIO* io;
  fi_handle handle;
  JOCTET* buffer;
};

// Owned by the frame that installs the landing pad, so the library object and
// the decoded dib are released whether DecodeImage returns or a fatal error
// jumps back to that frame.
struct ReadState {
  jpeg_decompress_struct cinfo;
  ErrorManager errors;
  std::unique_ptr<Dib> dib;
  bool created = false;

  ~ReadState() {
    if (created) {
      jpeg_destroy_decompress(&cinfo);
    }
  }
};

// Owned by the frame that installs the landing pad, as ReadState is.
struct WriteState {
  jpeg_compress_struct cinfo;
  ErrorManager errors;
  std::vector<BYTE> scratch{};  // one row in the library's form, reused for every row
  bool created = false;

  ~WriteState() {
    if (created) {
      jpeg_destroy_compress(&cinfo);
    }
  }
};

// The library hands back the manager base it embeds, always as the first
// member of its owner, so these casts are the container-of identity; naming
// each one once keeps reinterpret_cast out of the call sites.
ErrorManager* ErrorsOf(j_common_ptr cinfo) { return reinterpret_cast<ErrorManager*>(cinfo->err); }

void ReportMessage(j_common_ptr cinfo) {
  char message[JMSG_LENGTH_MAX];
  ErrorsOf(cinfo)->pub.format_message(cinfo, message);
  FreeImage_OutputMessageProc(FIF_JPEG, "JPEG: %s", message);
}

[[noreturn]] void Fail(j_common_ptr cinfo) {
  ErrorManager* errors = ErrorsOf(cinfo);
  ReportMessage(cinfo);
  longjmp(errors->landing, 1);
}

void Warn(j_common_ptr cinfo, int code) {
  ErrorManager* errors = ErrorsOf(cinfo);
  errors->pub.msg_code = code;
  errors->pub.emit_message(cinfo, -1);
}

// ----------------------------------------------------------
//   Stream bridges
// ----------------------------------------------------------

// The library's structs all start with their shared fields or with the manager
// they embed, so classic's C casts are the container-of identity here; naming
// them once keeps the reinterpret_cast in one place per shape.
j_common_ptr Common(j_decompress_ptr cinfo) { return reinterpret_cast<j_common_ptr>(cinfo); }

j_common_ptr Common(j_compress_ptr cinfo) { return reinterpret_cast<j_common_ptr>(cinfo); }

SourceManager* ManagerOf(j_decompress_ptr cinfo) { return reinterpret_cast<SourceManager*>(cinfo->src); }

DestinationManager* ManagerOf(j_compress_ptr cinfo) { return reinterpret_cast<DestinationManager*>(cinfo->dest); }

// The managers and their buffers live in the library's permanent pool, which
// releases them together with the (de)compress object.
void* AllocatePermanent(j_common_ptr cinfo, std::size_t bytes) {
  return (*cinfo->mem->alloc_small)(cinfo, JPOOL_PERMANENT, bytes);
}

void InitSource(j_decompress_ptr cinfo) { ManagerOf(cinfo)->start_of_file = TRUE; }

boolean FillSource(j_decompress_ptr cinfo) {
  SourceManager* source = ManagerOf(cinfo);
  unsigned const received = source->io->read_proc(source->buffer, 1, kIoBufferSize, source->handle);
  if (received == 0) {
    if (source->start_of_file) {
      Fail(Common(cinfo));
    }
    Warn(Common(cinfo), JWRN_JPEG_EOF);
    // A synthetic EOI lets the decompressor finish the rows it already decoded.
    source->buffer[0] = static_cast<JOCTET>(0xFF);
    source->buffer[1] = static_cast<JOCTET>(JPEG_EOI);
    source->pub.bytes_in_buffer = 2;
  } else {
    source->pub.bytes_in_buffer = received;
  }
  source->pub.next_input_byte = source->buffer;
  source->start_of_file = FALSE;
  return TRUE;
}

void SkipSource(j_decompress_ptr cinfo, long num_bytes) {
  SourceManager* source = ManagerOf(cinfo);
  while (num_bytes > 0) {
    while (num_bytes > static_cast<long>(source->pub.bytes_in_buffer)) {
      num_bytes -= static_cast<long>(source->pub.bytes_in_buffer);
      FillSource(cinfo);
    }
    source->pub.next_input_byte += static_cast<size_t>(num_bytes);
    source->pub.bytes_in_buffer -= static_cast<size_t>(num_bytes);
    num_bytes = 0;
  }
}

void TermSource(j_decompress_ptr /*cinfo*/) {}

void BindSource(j_decompress_ptr cinfo, FreeImageIO& io, fi_handle handle) {
  SourceManager* source = static_cast<SourceManager*>(AllocatePermanent(Common(cinfo), sizeof(SourceManager)));
  source->buffer = static_cast<JOCTET*>(AllocatePermanent(Common(cinfo), kIoBufferSize));
  source->pub.init_source = &InitSource;
  source->pub.fill_input_buffer = &FillSource;
  source->pub.skip_input_data = &SkipSource;
  source->pub.resync_to_restart = jpeg_resync_to_restart;
  source->pub.term_source = &TermSource;
  source->io = &io;
  source->handle = handle;
  source->pub.bytes_in_buffer = 0;
  source->pub.next_input_byte = nullptr;
  cinfo->src = &source->pub;
}

void InitDestination(j_compress_ptr cinfo) {
  DestinationManager* destination = ManagerOf(cinfo);
  destination->pub.next_output_byte = destination->buffer;
  destination->pub.free_in_buffer = kIoBufferSize;
}

boolean EmptyDestination(j_compress_ptr cinfo) {
  DestinationManager* destination = ManagerOf(cinfo);
  if (destination->io->write_proc == nullptr ||
      destination->io->write_proc(destination->buffer, 1, kIoBufferSize, destination->handle) != kIoBufferSize) {
    Fail(Common(cinfo));
  }
  destination->pub.next_output_byte = destination->buffer;
  destination->pub.free_in_buffer = kIoBufferSize;
  return TRUE;
}

void TermDestination(j_compress_ptr cinfo) {
  DestinationManager* destination = ManagerOf(cinfo);
  std::size_t const pending = kIoBufferSize - destination->pub.free_in_buffer;
  if (pending > 0 &&
      (destination->io->write_proc == nullptr ||
       destination->io->write_proc(destination->buffer, 1, static_cast<unsigned>(pending), destination->handle) !=
           pending)) {
    Fail(Common(cinfo));
  }
}

void BindDestination(j_compress_ptr cinfo, FreeImageIO& io, fi_handle handle) {
  DestinationManager* destination =
      static_cast<DestinationManager*>(AllocatePermanent(Common(cinfo), sizeof(DestinationManager)));
  destination->buffer = static_cast<JOCTET*>(AllocatePermanent(Common(cinfo), kIoBufferSize));
  destination->pub.init_destination = &InitDestination;
  destination->pub.empty_output_buffer = &EmptyDestination;
  destination->pub.term_destination = &TermDestination;
  destination->io = &io;
  destination->handle = handle;
  cinfo->dest = &destination->pub;
}

// ----------------------------------------------------------
//   Load
// ----------------------------------------------------------

inline BYTE* Scanline(Dib& dib, int y) { return dib.row(y).data(); }

inline BYTE const* Scanline(Dib const& dib, int y) { return dib.row(y).data(); }

bool DecodeImage(ReadState& state, int flags) {
  jpeg_decompress_struct& cinfo = state.cinfo;
  jpeg_read_header(&cinfo, TRUE);

  // The library can halve, quarter or eighth the image while decoding; the
  // requested size arrives in the top flag bits, as classic expects.
  unsigned scale_denom = 1;
  int const requested_size = flags >> 16;
  if (requested_size > 0) {
    unsigned const longest = cinfo.image_width > cinfo.image_height ? cinfo.image_width : cinfo.image_height;
    double const scale = static_cast<double>(longest) / static_cast<double>(requested_size);
    if (scale >= 8) {
      scale_denom = 8;
    } else if (scale >= 4) {
      scale_denom = 4;
    } else if (scale >= 2) {
      scale_denom = 2;
    }
  }
  cinfo.scale_num = 1;
  cinfo.scale_denom = scale_denom;

  if ((flags & JPEG_ACCURATE) != JPEG_ACCURATE) {
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
  }
  if ((flags & JPEG_GREYSCALE) == JPEG_GREYSCALE) {
    cinfo.out_color_space = JCS_GRAYSCALE;
  } else if (cinfo.out_color_space == JCS_CMYK || cinfo.out_color_space == JCS_YCCK) {
    cinfo.out_color_space = JCS_RGB;
  }

  jpeg_start_decompress(&cinfo);

  int const bpp = 8 * static_cast<int>(cinfo.output_components);
  if (bpp != 8 && bpp != 24) {
    FreeImage_OutputMessageProc(FIF_JPEG, "JPEG: %d output components are not supported", cinfo.output_components);
    return false;
  }

  state.dib = Dib::make(
      static_cast<int>(cinfo.output_width),
      static_cast<int>(cinfo.output_height),
      bpp,
      FI_RGBA_RED_MASK,
      FI_RGBA_GREEN_MASK,
      FI_RGBA_BLUE_MASK);
  if (state.dib == nullptr) {
    FreeImage_OutputMessageProc(FIF_JPEG, "JPEG: cannot allocate the bitmap");
    return false;
  }
  Dib& dib = *state.dib;

  if (bpp == 8) {
    for (int i = 0; i < kMaxPalette; ++i) {
      dib.palette()[i].rgbRed = static_cast<BYTE>(i);
      dib.palette()[i].rgbGreen = static_cast<BYTE>(i);
      dib.palette()[i].rgbBlue = static_cast<BYTE>(i);
    }
  }

  while (cinfo.output_scanline < cinfo.output_height) {
    // The library emits the visual top row first, so row n lands on scanline
    // height-1-n; the sample order is exchanged per pixel below it.
    BYTE* target = Scanline(dib, static_cast<int>(cinfo.output_height - cinfo.output_scanline - 1));
    jpeg_read_scanlines(&cinfo, &target, 1);
    if (bpp == 24) {
      for (int x = 0; x < dib.width(); ++x) {
        std::swap(target[x * 3], target[x * 3 + 2]);
      }
    }
  }

  jpeg_finish_decompress(&cinfo);
  return true;
}

// ----------------------------------------------------------
//   Save
// ----------------------------------------------------------

int SaveQuality(int flags) {
  if ((flags & JPEG_QUALITYBAD) == JPEG_QUALITYBAD) {
    return 10;
  }
  if ((flags & JPEG_QUALITYAVERAGE) == JPEG_QUALITYAVERAGE) {
    return 25;
  }
  if ((flags & JPEG_QUALITYNORMAL) == JPEG_QUALITYNORMAL) {
    return 50;
  }
  if ((flags & JPEG_QUALITYGOOD) == JPEG_QUALITYGOOD) {
    return 75;
  }
  if ((flags & JPEG_QUALITYSUPERB) == JPEG_QUALITYSUPERB) {
    return 100;
  }
  int const direct = flags & 0x7F;
  return direct == 0 ? 75 : direct;
}

void ApplySubsampling(jpeg_compress_struct& cinfo, int flags) {
  if (cinfo.in_color_space != JCS_RGB) {
    return;
  }
  if ((flags & JPEG_SUBSAMPLING_411) == JPEG_SUBSAMPLING_411) {
    cinfo.comp_info[0].h_samp_factor = 4;
    cinfo.comp_info[0].v_samp_factor = 1;
  } else if ((flags & JPEG_SUBSAMPLING_420) == JPEG_SUBSAMPLING_420) {
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 2;
  } else if ((flags & JPEG_SUBSAMPLING_422) == JPEG_SUBSAMPLING_422) {
    cinfo.comp_info[0].h_samp_factor = 2;
    cinfo.comp_info[0].v_samp_factor = 1;
  } else if ((flags & JPEG_SUBSAMPLING_444) == JPEG_SUBSAMPLING_444) {
    cinfo.comp_info[0].h_samp_factor = 1;
    cinfo.comp_info[0].v_samp_factor = 1;
  } else {
    return;
  }
  cinfo.comp_info[1].h_samp_factor = 1;
  cinfo.comp_info[1].v_samp_factor = 1;
  cinfo.comp_info[2].h_samp_factor = 1;
  cinfo.comp_info[2].v_samp_factor = 1;
}

// Expands scanline y of the DIB into the library's row form: one grey byte per
// pixel, otherwise R,G,B with any alpha byte dropped.
void PrepareRow(Dib const& dib, int y, bool greyscale, bool inverted, BYTE* target) {
  BYTE const* bits = Scanline(dib, y);
  if (greyscale) {
    if (inverted) {
      for (int x = 0; x < dib.width(); ++x) {
        target[x] = static_cast<BYTE>(255 - bits[x]);
      }
    }
    return;
  }
  int const step = dib.bpp() / 8;
  for (int x = 0; x < dib.width(); ++x) {
    if (dib.bpp() <= 8) {
      unsigned const index = bits[x] < dib.palette().size() ? bits[x] : static_cast<unsigned>(dib.palette().size()) - 1;
      RGBQUAD const& color = dib.palette()[index];
      target[x * 3] = color.rgbRed;
      target[x * 3 + 1] = color.rgbGreen;
      target[x * 3 + 2] = color.rgbBlue;
    } else {
      target[x * 3] = bits[x * step + FI_RGBA_RED];
      target[x * 3 + 1] = bits[x * step + FI_RGBA_GREEN];
      target[x * 3 + 2] = bits[x * step + FI_RGBA_BLUE];
    }
  }
}

bool EncodeImage(WriteState& state, Dib& dib, int flags) {
  jpeg_compress_struct& cinfo = state.cinfo;

  std::optional<Ramp> const ramp = dib.greyscale_ramp();
  bool const greyscale = ramp.has_value();
  bool const inverted = ramp == Ramp::Inverted;
  if (dib.bpp() != 8 && dib.bpp() != 24 && dib.bpp() != 32) {
    FreeImage_OutputMessageProc(FIF_JPEG, "JPEG: a %d bpp image is not savable as JPEG", dib.bpp());
    return false;
  }

  cinfo.image_width = static_cast<JDIMENSION>(dib.width());
  cinfo.image_height = static_cast<JDIMENSION>(dib.height());
  cinfo.input_components = greyscale ? 1 : 3;
  cinfo.in_color_space = greyscale ? JCS_GRAYSCALE : JCS_RGB;
  jpeg_set_defaults(&cinfo);

  if ((flags & JPEG_PROGRESSIVE) == JPEG_PROGRESSIVE) {
    jpeg_simple_progression(&cinfo);
  }
  if ((flags & JPEG_OPTIMIZE) == JPEG_OPTIMIZE) {
    cinfo.optimize_coding = TRUE;
  }
  if ((flags & JPEG_BASELINE) == JPEG_BASELINE) {
    cinfo.write_JFIF_header = FALSE;
    cinfo.write_Adobe_marker = FALSE;
  }
  ApplySubsampling(cinfo, flags);
  jpeg_set_quality(&cinfo, SaveQuality(flags), TRUE);

  jpeg_start_compress(&cinfo, TRUE);

  // A plain grey ramp is already in the library's byte order and needs no copy;
  // every other shape is expanded into one reusable row.
  bool const row_passthrough = greyscale && !inverted;
  if (!row_passthrough) {
    state.scratch.resize(static_cast<std::size_t>(dib.width()) * 3);
  }

  while (cinfo.next_scanline < cinfo.image_height) {
    int const y = dib.height() - static_cast<int>(cinfo.next_scanline) - 1;
    BYTE* row = row_passthrough ? Scanline(dib, y) : state.scratch.data();
    PrepareRow(dib, y, greyscale, inverted, row);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }

  jpeg_finish_compress(&cinfo);
  return true;
}

}  // namespace

std::string_view JpegCodec::format_name() const { return "JPEG"; }

std::string_view JpegCodec::extensions() const { return "jpg,jpe,jpeg"; }

std::string_view JpegCodec::description() const { return "JFIF Huffman Encoding"; }

bool JpegCodec::validate(FreeImageIO const& io, fi_handle handle) const {
  BYTE signature[2] = {0, 0};
  if (io.read_proc(signature, 1, sizeof(signature), handle) != sizeof(signature)) {
    return false;
  }
  return signature[0] == 0xFF && signature[1] == 0xD8;
}

std::unique_ptr<Dib> JpegCodec::load(FreeImageIO& io, fi_handle handle, int /*page*/, int flags) const {
  if (handle == nullptr) {
    return nullptr;
  }
  ReadState state;
  state.cinfo.mem = nullptr;
  state.cinfo.err = jpeg_std_error(&state.errors.pub);
  state.errors.pub.error_exit = &Fail;
  state.errors.pub.output_message = &ReportMessage;

  if (setjmp(state.errors.landing) != 0) {
    return nullptr;
  }
  jpeg_create_decompress(&state.cinfo);
  state.created = true;
  BindSource(&state.cinfo, io, handle);

  if (!DecodeImage(state, flags)) {
    return nullptr;
  }
  std::unique_ptr<Dib> result = std::move(state.dib);
  return result;
}

bool JpegCodec::save(FreeImageIO& io, Dib& dib, fi_handle handle, int flags) const {
  if (!dib.has_pixels() || !dib.is_bitmap() || handle == nullptr || io.write_proc == nullptr) {
    return false;
  }

  WriteState state;
  state.cinfo.mem = nullptr;
  state.cinfo.err = jpeg_std_error(&state.errors.pub);
  state.errors.pub.error_exit = &Fail;
  state.errors.pub.output_message = &ReportMessage;

  if (setjmp(state.errors.landing) != 0) {
    return false;
  }
  jpeg_create_compress(&state.cinfo);
  state.created = true;
  BindDestination(&state.cinfo, io, handle);

  return EncodeImage(state, dib, flags);
}

int JpegCodec::page_count(FreeImageIO& /*io*/, fi_handle /*handle*/) const { return 1; }

JpegCodec const jpeg_codec;

}  // namespace fi
