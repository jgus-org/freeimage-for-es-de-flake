// Stream plumbing: the stdio bridge behind the filename entry points and the
// stream behind a FIMEMORY handle (§2.9). A wrapped buffer stays caller-owned and
// read-only; an owned stream grows its buffer as a plugin writes. The memory
// FreeImageIO uses the FIMEMORY* itself as its fi_handle, matching classic, and
// the stream operations below it are the same ones the FreeImage_*_Memory entry
// points publish through the marshal TU.
#include "io.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace fi {
namespace io {
namespace {

// The stream behind a FIMEMORY handle (§2.9).
class MemoryStream {
 public:
  // A null buffer means "nothing to wrap": the stream owns a growable buffer, which is
  // what makes WriteMemory - and therefore SaveToMemory - able to fill it.
  explicit MemoryStream(BYTE* wrapped, DWORD wrapped_size)
      : external(wrapped),
        size(wrapped != nullptr ? wrapped_size : 0),
        owns_buffer(wrapped == nullptr) {}

  // An owned stream reports no data until it has allocated some, so AcquireMemory hands
  // out the same null a caller buffer never does.
  BYTE* data() {
    if (!owns_buffer) {
      return external;
    }
    return buffer.empty() ? nullptr : buffer.data();
  }

  DWORD used() const { return size; }

  std::size_t read(void* target, std::size_t wanted) {
    if (wanted == 0 || pos >= size) {
      return 0;
    }
    std::size_t const bytes = std::min(wanted, static_cast<std::size_t>(size - pos));
    if (bytes == 0) {
      return 0;  // a writer-opened stream has no bytes at all, and nothing to read from
    }
    std::memcpy(target, data() + pos, bytes);
    pos += static_cast<DWORD>(bytes);
    return bytes;
  }

  bool write(void const* source, std::size_t bytes) {
    if (!owns_buffer) {
      return false;  // a read-only wrapped buffer is never written through
    }
    std::size_t const needed = static_cast<std::size_t>(pos) + bytes;
    if (needed > buffer.size()) {
      std::size_t grown = buffer.size() != 0 ? buffer.size() : kInitialCapacity;
      while (grown < needed) {
        grown *= 2;
      }
      try {
        buffer.resize(grown);
      } catch (std::bad_alloc const&) {
        return false;  // a failed allocation is reported as a failed write, not a throw
      }
    }
    std::memcpy(buffer.data() + pos, source, bytes);
    pos += static_cast<DWORD>(bytes);
    size = std::max(size, pos);
    return true;
  }

  bool seek(long offset, int origin) {
    long target;
    switch (origin) {
      case SEEK_SET:
        target = offset;
        break;
      case SEEK_CUR:
        target = static_cast<long>(pos) + offset;
        break;
      case SEEK_END:
        target = static_cast<long>(size) + offset;
        break;
      default:
        return false;
    }
    if (target < 0) {
      return false;
    }
    pos = static_cast<DWORD>(target);
    return true;
  }

  long tell() const { return static_cast<long>(pos); }

 private:
  // First allocation of an owned buffer; later growth doubles.
  static constexpr std::size_t kInitialCapacity = 4096;

  BYTE* external = nullptr;
  std::vector<BYTE> buffer;
  DWORD size = 0;
  DWORD pos = 0;
  bool owns_buffer = false;
};

unsigned FileRead(void* buffer, unsigned size, unsigned count, fi_handle handle) {
  return std::fread(buffer, size, count, static_cast<FILE*>(handle));
}

unsigned FileWrite(void* buffer, unsigned size, unsigned count, fi_handle handle) {
  return std::fwrite(buffer, size, count, static_cast<FILE*>(handle));
}

int FileSeek(fi_handle handle, long offset, int origin) {
  return std::fseek(static_cast<FILE*>(handle), offset, origin);
}

long FileTell(fi_handle handle) { return std::ftell(static_cast<FILE*>(handle)); }

MemoryStream* stream_of(FIMEMORY* memory) {
  return memory != nullptr ? static_cast<MemoryStream*>(memory->data) : nullptr;
}

}  // namespace

FreeImageIO& file_io() {
  static FreeImageIO io = {&FileRead, &FileWrite, &FileSeek, &FileTell};
  return io;
}

FILE_ptr open_file(char const* path, char const* mode) { return FILE_ptr(std::fopen(path, mode)); }

std::string narrow_path(wchar_t const* wide) {
  std::string out;
  if (wide == nullptr) {
    return out;
  }
  for (wchar_t const* p = wide; *p != L'\0'; ++p) {
    wchar_t const c = *p;
    out.push_back(c >= 0x20 && c < 0x7f ? static_cast<char>(c) : '?');
  }
  return out;
}

void MemoryDeleter::operator()(FIMEMORY* handle) const noexcept {
  if (handle == nullptr) {
    return;
  }
  delete static_cast<MemoryStream*>(handle->data);
  delete handle;
}

FIMEMORY_ptr make_stream(BYTE* data, DWORD size_in_bytes) {
  FIMEMORY_ptr wrapper(new FIMEMORY);
  wrapper->data = std::make_unique<MemoryStream>(data, size_in_bytes).release();
  return wrapper;
}

unsigned read(FIMEMORY* stream, void* buffer, unsigned size, unsigned count) {
  MemoryStream* memory = stream_of(stream);
  if (memory == nullptr || size == 0 || count == 0) {
    return 0;
  }
  return static_cast<unsigned>(memory->read(buffer, static_cast<std::size_t>(size) * count) / size);
}

unsigned write(FIMEMORY* stream, void const* buffer, unsigned size, unsigned count) {
  MemoryStream* memory = stream_of(stream);
  if (memory == nullptr || size == 0 || count == 0) {
    return 0;
  }
  return memory->write(buffer, static_cast<std::size_t>(size) * count) ? count : 0;
}

int seek(FIMEMORY* stream, long offset, int origin) {
  MemoryStream* memory = stream_of(stream);
  return memory != nullptr && memory->seek(offset, origin) ? 0 : -1;
}

long tell(FIMEMORY* stream) {
  MemoryStream* memory = stream_of(stream);
  return memory != nullptr ? memory->tell() : -1;
}

bool acquire(FIMEMORY* stream, BYTE** data, DWORD* size_in_bytes) {
  MemoryStream* memory = stream_of(stream);
  if (memory == nullptr) {
    return false;
  }
  if (data != nullptr) {
    *data = memory->data();
  }
  if (size_in_bytes != nullptr) {
    *size_in_bytes = memory->used();
  }
  return true;
}

namespace {

// The FreeImageIO adapter table: the callbacks carry the published fi_handle shape
// and hand off to the stream operations above, which the entry points reach directly.
unsigned MemoryRead(void* buffer, unsigned size, unsigned count, fi_handle handle) {
  return read(static_cast<FIMEMORY*>(handle), buffer, size, count);
}

unsigned MemoryWrite(void* buffer, unsigned size, unsigned count, fi_handle handle) {
  return write(static_cast<FIMEMORY*>(handle), buffer, size, count);
}

int MemorySeek(fi_handle handle, long offset, int origin) {
  return seek(static_cast<FIMEMORY*>(handle), offset, origin);
}

long MemoryTell(fi_handle handle) { return tell(static_cast<FIMEMORY*>(handle)); }

}  // namespace

FreeImageIO& memory_io() {
  static FreeImageIO io = {&MemoryRead, &MemoryWrite, &MemorySeek, &MemoryTell};
  return io;
}

}  // namespace io
}  // namespace fi
