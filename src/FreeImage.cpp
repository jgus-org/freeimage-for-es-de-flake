// The marshal layer: every extern "C" entry point of the public header, grouped and
// ordered exactly as the header declares them. This is the only file that speaks the
// handle shapes - a body validates or marshals through the abi/io seams and then
// delegates to one internal call; the library self-initialises (the registry is a
// compile-time table), so Initialise/DeInitialise are no-ops and the version strings
// and the message sink are the only things answered here directly (§2.1, §2.2).
#include <FreeImage.h>

#include <cstdarg>
#include <cstdio>
#include <string>

#include "abi.h"
#include "convert.h"
#include "dib.h"
#include "io.h"
#include "multipage.h"
#include "registry.h"
#include "resample.h"

namespace fi {
namespace {

constexpr char const* kVersion = "3.18.0";
constexpr char const* kCopyright = "freeimage-for-es-de 3.18.0 - Copyright (c) 2026 Joshua Gustafson - MIT licensed";

// The one piece of mutable global state: classic leaves the sink unlocked, and es-de
// never installs one (§2.11).
FreeImage_OutputMessageFunction g_sink = nullptr;

}  // namespace
}  // namespace fi

// Init / Error routines ------------------------------------------------------

void FreeImage_Initialise(BOOL load_local_plugins_only) {
  (void) load_local_plugins_only;  // no external plugins exist
}

void FreeImage_DeInitialise(void) {}

// Version routines -----------------------------------------------------------

const char* FreeImage_GetVersion(void) { return fi::kVersion; }

const char* FreeImage_GetCopyrightMessage(void) { return fi::kCopyright; }

// Message output functions ----------------------------------------------------

void FreeImage_SetOutputMessageStdCall(FreeImage_OutputMessageFunctionStdCall omf) {
  // On POSIX DLL_CALLCONV is empty, so the two sink types are identical.
  fi::g_sink = reinterpret_cast<FreeImage_OutputMessageFunction>(omf);
}

void FreeImage_SetOutputMessage(FreeImage_OutputMessageFunction omf) { fi::g_sink = omf; }

void FreeImage_OutputMessageProc(int fif, const char* fmt, ...) {
  if (fi::g_sink == nullptr || fmt == nullptr) {
    return;  // classic drops messages when no sink is installed
  }
  char buffer[1024];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buffer, sizeof(buffer), fmt, args);
  va_end(args);
  fi::g_sink(static_cast<FREE_IMAGE_FORMAT>(fif), buffer);
}

// Load / Save routines --------------------------------------------------------

void FreeImage_Unload(FIBITMAP* dib) { fi::dispose_bitmap(dib); }

FIBITMAP* FreeImage_Load(FREE_IMAGE_FORMAT fif, char const* filename, int flags) {
  if (filename == nullptr) {
    return nullptr;
  }
  fi::io::FILE_ptr const file = fi::io::open_file(filename, "rb");
  if (file == nullptr) {
    return nullptr;
  }
  return fi::adopt_bitmap(fi::registry::load(fif, fi::io::file_io(), file.get(), -1, flags)).release();
}

FIBITMAP* FreeImage_LoadU(FREE_IMAGE_FORMAT fif, wchar_t const* filename, int flags) {
  // POSIX narrow-char conversion: sufficient on Linux, never called by ES-DE.
  std::string const path = fi::io::narrow_path(filename);
  fi::io::FILE_ptr const file = fi::io::open_file(path.c_str(), "rb");
  if (file == nullptr) {
    return nullptr;
  }
  return fi::adopt_bitmap(fi::registry::load(fif, fi::io::file_io(), file.get(), -1, flags)).release();
}

FIBITMAP* FreeImage_LoadFromHandle(FREE_IMAGE_FORMAT fif, FreeImageIO* io, fi_handle handle, int flags) {
  if (io == nullptr || handle == nullptr) {
    return nullptr;
  }
  return fi::adopt_bitmap(fi::registry::load(fif, *io, handle, -1, flags)).release();
}

BOOL FreeImage_Save(FREE_IMAGE_FORMAT fif, FIBITMAP* dib, char const* filename, int flags) {
  if (filename == nullptr) {
    return FALSE;
  }
  // Classic opens "w+b" before it resolves the format, so an unregistered format or a
  // rejected dib still leaves a truncated file. ES-DE overwrites in place: no
  // temp-file/rename dance (§4.5).
  fi::io::FILE_ptr const file = fi::io::open_file(filename, "w+b");
  if (file == nullptr) {
    FreeImage_OutputMessageProc(static_cast<int>(fif), "FreeImage_Save: failed to open file %s", filename);
    return FALSE;
  }
  fi::Dib* d = fi::deref(dib);
  if (d == nullptr) {
    return FALSE;
  }
  return fi::registry::save(fif, *d, fi::io::file_io(), file.get(), flags) ? TRUE : FALSE;
}

BOOL FreeImage_SaveU(FREE_IMAGE_FORMAT fif, FIBITMAP* dib, wchar_t const* filename, int flags) {
  std::string const path = fi::io::narrow_path(filename);
  fi::io::FILE_ptr const file = fi::io::open_file(path.c_str(), "w+b");
  if (file == nullptr) {
    FreeImage_OutputMessageProc(static_cast<int>(fif), "FreeImage_Save: failed to open file %s", path.c_str());
    return FALSE;
  }
  fi::Dib* d = fi::deref(dib);
  if (d == nullptr) {
    return FALSE;
  }
  return fi::registry::save(fif, *d, fi::io::file_io(), file.get(), flags) ? TRUE : FALSE;
}

BOOL FreeImage_SaveToHandle(FREE_IMAGE_FORMAT fif, FIBITMAP* dib, FreeImageIO* io, fi_handle handle, int flags) {
  if (io == nullptr || handle == nullptr) {
    return FALSE;
  }
  fi::Dib* d = fi::deref(dib);
  if (d == nullptr) {
    return FALSE;
  }
  return fi::registry::save(fif, *d, *io, handle, flags) ? TRUE : FALSE;
}

// Memory I/O stream routines ---------------------------------------------------

FIMEMORY* FreeImage_OpenMemory(BYTE* data, DWORD size_in_bytes) {
  return fi::io::make_stream(data, size_in_bytes).release();
}

void FreeImage_CloseMemory(FIMEMORY* stream) { fi::io::close_stream(stream); }

FIBITMAP* FreeImage_LoadFromMemory(FREE_IMAGE_FORMAT fif, FIMEMORY* stream, int flags) {
  if (stream == nullptr) {
    return nullptr;
  }
  return fi::adopt_bitmap(fi::registry::load(fif, fi::io::memory_io(), stream, -1, flags)).release();
}

BOOL FreeImage_SaveToMemory(FREE_IMAGE_FORMAT fif, FIBITMAP* dib, FIMEMORY* stream, int flags) {
  if (stream == nullptr) {
    return FALSE;
  }
  fi::Dib* d = fi::deref(dib);
  if (d == nullptr) {
    return FALSE;
  }
  return fi::registry::save(fif, *d, fi::io::memory_io(), stream, flags) ? TRUE : FALSE;
}

long FreeImage_TellMemory(FIMEMORY* stream) { return fi::io::tell(stream); }

BOOL FreeImage_SeekMemory(FIMEMORY* stream, long offset, int origin) {
  return fi::io::seek(stream, offset, origin) == 0 ? TRUE : FALSE;
}

BOOL FreeImage_AcquireMemory(FIMEMORY* stream, BYTE** data, DWORD* size_in_bytes) {
  return fi::io::acquire(stream, data, size_in_bytes) ? TRUE : FALSE;
}

unsigned FreeImage_ReadMemory(void* buffer, unsigned size, unsigned count, FIMEMORY* stream) {
  return fi::io::read(stream, buffer, size, count);
}

unsigned FreeImage_WriteMemory(const void* buffer, unsigned size, unsigned count, FIMEMORY* stream) {
  return fi::io::write(stream, buffer, size, count);
}

// Plugin Interface -------------------------------------------------------------

FREE_IMAGE_FORMAT FreeImage_GetFIFFromFilename(char const* filename) { return fi::registry::from_extension(filename); }

FREE_IMAGE_FORMAT FreeImage_GetFIFFromFilenameU(wchar_t const* filename) {
  return fi::registry::from_extension(fi::io::narrow_path(filename).c_str());
}

BOOL FreeImage_FIFSupportsReading(FREE_IMAGE_FORMAT fif) {
  return fi::registry::registered(fif) != nullptr ? TRUE : FALSE;
}

BOOL FreeImage_FIFSupportsWriting(FREE_IMAGE_FORMAT fif) {
  return fi::registry::registered(fif) != nullptr ? TRUE : FALSE;
}

// Multipaging interface ---------------------------------------------------------

FIMULTIBITMAP* FreeImage_OpenMultiBitmap(
    FREE_IMAGE_FORMAT fif,
    char const* filename,
    BOOL create_new,
    BOOL read_only,
    BOOL keep_cache_in_memory,
    int flags) {
  (void) keep_cache_in_memory;  // no cache exists, so there is nothing to keep in memory
  return fi::multipage::open_from_file(fif, filename, create_new, read_only, flags).release();
}

FIMULTIBITMAP*
FreeImage_OpenMultiBitmapFromHandle(FREE_IMAGE_FORMAT fif, FreeImageIO* io, fi_handle handle, int flags) {
  if (io == nullptr || handle == nullptr) {
    return nullptr;
  }
  return fi::multipage::open_from_handle(fif, *io, handle, flags).release();
}

BOOL FreeImage_CloseMultiBitmap(FIMULTIBITMAP* bitmap, int flags) {
  (void) flags;
  return fi::multipage::close(bitmap) ? TRUE : FALSE;
}

int FreeImage_GetPageCount(FIMULTIBITMAP* bitmap) { return fi::multipage::page_count(bitmap); }

FIBITMAP* FreeImage_LockPage(FIMULTIBITMAP* bitmap, int page) { return fi::multipage::lock_page(bitmap, page); }

void FreeImage_UnlockPage(FIMULTIBITMAP* bitmap, FIBITMAP* data, BOOL changed) {
  (void) changed;  // with no cache, a changed page is discarded exactly like an unchanged one
  fi::multipage::unlock_page(bitmap, data);
}

// File type request routines ------------------------------------------------------

FREE_IMAGE_FORMAT FreeImage_GetFileType(char const* filename, int /*size*/) {
  if (filename == nullptr) {
    return FIF_UNKNOWN;
  }
  fi::io::FILE_ptr const file = fi::io::open_file(filename, "rb");
  if (file == nullptr) {
    return FIF_UNKNOWN;
  }
  return fi::registry::detect(fi::io::file_io(), file.get());
}

FREE_IMAGE_FORMAT FreeImage_GetFileTypeU(wchar_t const* /*filename*/, int /*size*/) {
  // No UTF-16 filesystem path on POSIX; classic returns FIF_UNKNOWN here.
  return FIF_UNKNOWN;
}

FREE_IMAGE_FORMAT FreeImage_GetFileTypeFromMemory(FIMEMORY* stream, int /*size*/) {
  if (stream == nullptr) {
    return FIF_UNKNOWN;
  }
  return fi::registry::detect(fi::io::memory_io(), stream);
}

// Image type request routine --------------------------------------------------------

FREE_IMAGE_TYPE FreeImage_GetImageType(FIBITMAP* dib) {
  fi::Dib const* d = fi::deref(dib);
  return d != nullptr ? d->type() : FIT_UNKNOWN;
}

// Pixel access routines -------------------------------------------------------------

BYTE* FreeImage_GetBits(FIBITMAP* dib) {
  fi::Dib* d = fi::deref(dib);
  return fi::has_pixels(d) ? d->pixels().data() : nullptr;
}

BYTE* FreeImage_GetScanLine(FIBITMAP* dib, int scanline) {
  fi::Dib* d = fi::deref(dib);
  if (!fi::has_pixels(d) || scanline < 0 || scanline >= d->height()) {
    return nullptr;
  }
  return d->row(scanline).data();
}

BOOL FreeImage_GetPixelIndex(FIBITMAP* dib, unsigned x, unsigned y, BYTE* value) {
  fi::Dib const* d = fi::readable_bitmap(dib);
  return d != nullptr && fi::convert::get_pixel_index(*d, x, y, value) ? TRUE : FALSE;
}

BOOL FreeImage_GetPixelColor(FIBITMAP* dib, unsigned x, unsigned y, RGBQUAD* value) {
  fi::Dib const* d = fi::readable_bitmap(dib);
  return d != nullptr && fi::convert::get_pixel_color(*d, x, y, value) ? TRUE : FALSE;
}

BOOL FreeImage_SetPixelIndex(FIBITMAP* dib, unsigned x, unsigned y, BYTE* value) {
  fi::Dib* d = fi::writable_bitmap(dib);
  return d != nullptr && fi::convert::set_pixel_index(*d, x, y, value) ? TRUE : FALSE;
}

BOOL FreeImage_SetPixelColor(FIBITMAP* dib, unsigned x, unsigned y, RGBQUAD* value) {
  fi::Dib* d = fi::writable_bitmap(dib);
  return d != nullptr && fi::convert::set_pixel_color(*d, x, y, value) ? TRUE : FALSE;
}

// DIB info routines --------------------------------------------------------------

unsigned FreeImage_GetBPP(FIBITMAP* dib) {
  fi::Dib const* d = fi::deref(dib);
  return d != nullptr ? static_cast<unsigned>(d->bpp()) : 0;
}

unsigned FreeImage_GetWidth(FIBITMAP* dib) {
  fi::Dib const* d = fi::deref(dib);
  return d != nullptr ? static_cast<unsigned>(d->width()) : 0;
}

unsigned FreeImage_GetHeight(FIBITMAP* dib) {
  fi::Dib const* d = fi::deref(dib);
  return d != nullptr ? static_cast<unsigned>(d->height()) : 0;
}

unsigned FreeImage_GetLine(FIBITMAP* dib) {
  fi::Dib const* d = fi::deref(dib);
  return d != nullptr ? d->line() : 0;
}

unsigned FreeImage_GetPitch(FIBITMAP* dib) {
  fi::Dib const* d = fi::deref(dib);
  return d != nullptr ? d->pitch() : 0;
}

// Smart conversion routines -------------------------------------------------------

FIBITMAP* FreeImage_ConvertTo24Bits(FIBITMAP* dib) {
  fi::Dib const* d = fi::readable_bitmap(dib);
  return d != nullptr ? fi::adopt_bitmap(fi::convert::to_dib(*d, 24)).release() : nullptr;
}

FIBITMAP* FreeImage_ConvertTo32Bits(FIBITMAP* dib) {
  fi::Dib const* d = fi::readable_bitmap(dib);
  return d != nullptr ? fi::adopt_bitmap(fi::convert::to_dib(*d, 32)).release() : nullptr;
}

FIBITMAP* FreeImage_ConvertFromRawBits(
    BYTE* bits,
    int width,
    int height,
    int pitch,
    unsigned bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask,
    BOOL topdown) {
  return fi::adopt_bitmap(
             fi::convert::from_raw_bits(
                 TRUE,
                 bits,
                 FIT_BITMAP,
                 width,
                 height,
                 pitch,
                 bpp,
                 red_mask,
                 green_mask,
                 blue_mask,
                 topdown))
      .release();
}

FIBITMAP* FreeImage_ConvertFromRawBitsEx(
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
    BOOL topdown) {
  return fi::adopt_bitmap(
             fi::convert::from_raw_bits(
                 copySource,
                 bits,
                 type,
                 width,
                 height,
                 pitch,
                 bpp,
                 red_mask,
                 green_mask,
                 blue_mask,
                 topdown))
      .release();
}

void FreeImage_ConvertToRawBits(
    BYTE* bits,
    FIBITMAP* dib,
    int pitch,
    unsigned bpp,
    unsigned red_mask,
    unsigned green_mask,
    unsigned blue_mask,
    BOOL topdown) {
  fi::Dib const* d = fi::deref(dib);
  if (!fi::has_pixels(d)) {
    return;  // §2.6: classic no-ops without a dib or without pixels
  }
  fi::convert::to_raw_bits(*d, bits, pitch, bpp, red_mask, green_mask, blue_mask, topdown);
}

// Image manipulation toolkit --------------------------------------------------------

FIBITMAP* FreeImage_Rescale(FIBITMAP* dib, int dst_width, int dst_height, FREE_IMAGE_FILTER filter) {
  fi::Dib const* d = fi::readable_bitmap(dib);
  return d != nullptr ? fi::adopt_bitmap(fi::rescale(*d, dst_width, dst_height, filter)).release() : nullptr;
}

BOOL FreeImage_PreMultiplyWithAlpha(FIBITMAP* dib) {
  fi::Dib* d = fi::writable_bitmap(dib);
  return d != nullptr && fi::convert::premultiply_with_alpha(*d) ? TRUE : FALSE;
}

// Metadata routines -----------------------------------------------------------------

DWORD FreeImage_GetTagCount(FITAG* tag) {
  fi::Tag const* stored = fi::stored_tag(tag);
  return stored != nullptr ? stored->count() : 0;
}

const void* FreeImage_GetTagValue(FITAG* tag) {
  fi::Tag const* stored = fi::stored_tag(tag);
  if (stored == nullptr || stored->value.empty()) {
    return nullptr;
  }
  return stored->value.data();
}

BOOL FreeImage_GetMetadata(FREE_IMAGE_MDMODEL model, FIBITMAP* dib, char const* key, FITAG** tag) {
  fi::Dib const* d = fi::deref(dib);
  FITAG const* found = d != nullptr && key != nullptr ? d->find(model, key) : nullptr;
  if (found == nullptr) {
    return FALSE;  // §2.10: a missing tag allocates nothing and leaves *tag untouched
  }
  if (tag != nullptr) {
    // The frozen classic signature hands the handle out as FITAG*; the payload stays
    // dib-owned and immutable through it, so the cast restores only the published shape.
    *tag = const_cast<FITAG*>(found);
  }
  return TRUE;
}
