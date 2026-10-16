// Stream plumbing under the C ABI: the stdio bridge behind the filename entry
// points and the FIMEMORY-backed stream behind the *FromMemory paths and the
// FIMEMORY wrappers themselves. Audience: the marshal TU and the multi-page
// container, which owns the FILE* of a filename-based open; never a codec, which
// only ever sees a caller-supplied FreeImageIO.
//
// The stream objects themselves stay in io.cpp; what is published here is the
// handle-level operation set, so each public stream entry point is one internal
// call away from its behavior (§2.7, §2.9).
#pragma once

#include <FreeImage.h>

#include <cstdio>
#include <memory>
#include <string>

namespace fi {
namespace io {

// The stdio bridge behind the filename entry points; the FILE* stays caller-owned
// on the handle paths, and only a filename open owns its stream (§2.7).
FreeImageIO& file_io();

struct FileCloser {
  void operator()(FILE* file) const noexcept {
    if (file != nullptr) {
      std::fclose(file);
    }
  }
};
using FILE_ptr = std::unique_ptr<FILE, FileCloser>;

// The stdio open the filename entry points share, owned from the moment it exists.
FILE_ptr open_file(char const* path, char const* mode);

// Best-effort UTF-16 (wchar_t) -> narrow path conversion for the classic U entry
// points. Only meaningful on POSIX for the never-called LoadU/SaveU.
std::string narrow_path(wchar_t const* wide);

// The FIMEMORY handle seam: the stream behind the handle owns either a wrapped
// caller buffer (read-only) or a growable buffer, exactly as classic publishes
// OpenMemory/CloseMemory (§2.9). The read/write/seek/tell operations carry the
// whole-item and 0/-1 semantics that the FreeImage_*_Memory entry points publish.
struct MemoryDeleter {
  void operator()(FIMEMORY* handle) const noexcept;
};
using FIMEMORY_ptr = std::unique_ptr<FIMEMORY, MemoryDeleter>;

// Builds a FIMEMORY stream. A null data means "nothing to wrap": the stream owns a
// growable buffer, which is what makes WriteMemory - and therefore SaveToMemory -
// able to fill it.
FIMEMORY_ptr make_stream(BYTE* data, DWORD size_in_bytes);

// Re-adopts a released stream so its single delete runs; the wrapped caller buffer
// was never ours, so only internally owned storage is freed (§2.9).
inline void close_stream(FIMEMORY* stream) { FIMEMORY_ptr const ownership(stream); }

unsigned read(FIMEMORY* stream, void* buffer, unsigned size, unsigned count);
unsigned write(FIMEMORY* stream, void const* buffer, unsigned size, unsigned count);
int seek(FIMEMORY* stream, long offset, int origin);
long tell(FIMEMORY* stream);
// Publishes the stream's data pointer and used size, each written iff its out
// parameter is non-null; false for a null stream.
bool acquire(FIMEMORY* stream, BYTE** data, DWORD* size_in_bytes);

// The stream whose fi_handle is a FIMEMORY* (whose data is that stream's object),
// used by the *FromMemory entry points and by the FIMEMORY wrappers themselves.
FreeImageIO& memory_io();

}  // namespace io
}  // namespace fi
