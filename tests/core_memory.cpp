// Memory streams, format detection and the registry surface (§2.3, §2.9, §2.1), plus
// the dispatcher's file discipline (§2.3, §4.5).
//
// The registry and the memory stream are already implemented, so most of this file is
// expected to be green against the stub library; the tests that need real decoding go
// red until the codecs land.
#include "helpers.h"

namespace {

constexpr FREE_IMAGE_FORMAT kUnregisteredTiff = static_cast<FREE_IMAGE_FORMAT>(18);
constexpr int kWidth = 3;
constexpr int kHeight = 3;

const char* missing_path() { return "/this/path/does/not/exist/freeimage-for-es-de.png"; }

}  // namespace

TEST_CASE("[memory] OpenMemory wraps the caller buffer without copying per §2.9") {
  fit::Bytes buffer{'B', 'M', 0x01, 0x02, 0x03, 0x04};
  const fit::Memory memory(FreeImage_OpenMemory(buffer.data(), static_cast<DWORD>(buffer.size())));
  REQUIRE_MESSAGE(memory.get() != nullptr, "OpenMemory must return a stream (§2.9)");

  // The frozen surface exposes the wrapped pointer: a copy would be a different address.
  BYTE* data = nullptr;
  DWORD size = 0;
  REQUIRE(FreeImage_AcquireMemory(memory.get(), &data, &size) == TRUE);
  CHECK(data == buffer.data());
  CHECK(size == static_cast<DWORD>(buffer.size()));

  // And reads see later writes to the caller's buffer, which is the same clause
  // observable without any codec.
  buffer[0] = 'X';
  buffer[1] = 'Y';
  BYTE head[2] = {0, 0};
  REQUIRE(FreeImage_ReadMemory(head, 1, 2, memory.get()) == 2u);
  CHECK(head[0] == 'X');
  CHECK(head[1] == 'Y');
  // §2.9: the stream advances as the plugin reads.
  CHECK(FreeImage_TellMemory(memory.get()) == 2);
}

TEST_CASE("[memory] CloseMemory frees only internally-owned buffers per §2.9") {
  fit::Bytes buffer{'G', 'I', 'F', '8', '9', 'a'};
  FIMEMORY* memory = FreeImage_OpenMemory(buffer.data(), static_cast<DWORD>(buffer.size()));
  REQUIRE(memory != nullptr);
  FreeImage_CloseMemory(memory);
  // The wrapped buffer is caller-owned: it stays valid and unchanged after the stream
  // is closed (a use-after-free or double free shows up under a sanitizer build).
  CHECK(buffer == (fit::Bytes{'G', 'I', 'F', '8', '9', 'a'}));
  FreeImage_CloseMemory(nullptr);  // §2.1 null guard: no-op
}

TEST_CASE("[memory] OpenMemory with no data yields the growable internal buffer per §2.9") {
  const fit::Memory memory(FreeImage_OpenMemory());
  REQUIRE_MESSAGE(memory.get() != nullptr, "OpenMemory() must return a stream (§2.9)");

  const fit::Bytes payload{0x01, 0x02, 0x03, 0x04};
  REQUIRE(
      FreeImage_WriteMemory(payload.data(), 1, static_cast<unsigned>(payload.size()), memory.get()) == payload.size());

  BYTE* data = nullptr;
  DWORD size = 0;
  REQUIRE(FreeImage_AcquireMemory(memory.get(), &data, &size) == TRUE);
  CHECK(size == static_cast<DWORD>(payload.size()));
  REQUIRE(data != nullptr);
  CHECK_BYTES_EQ(fit::Bytes(data, data + size), payload);

  FreeImage_SeekMemory(memory.get(), 0, SEEK_SET);
  REQUIRE(FreeImage_TellMemory(memory.get()) == 0);
  fit::Bytes read_back(payload.size(), 0);
  REQUIRE(
      FreeImage_ReadMemory(read_back.data(), 1, static_cast<unsigned>(read_back.size()), memory.get()) ==
      read_back.size());
  CHECK_BYTES_EQ(read_back, payload);
}

TEST_CASE("[format] GetFileType on an absent file is FIF_UNKNOWN in every call shape per §2.3") {
  // ES-DE calls both GetFileType(path) and GetFileType(path, 0); classic takes the size
  // argument and ignores it.
  CHECK(FreeImage_GetFileType(missing_path()) == FIF_UNKNOWN);
  CHECK(FreeImage_GetFileType(missing_path(), 0) == FIF_UNKNOWN);
  CHECK(FreeImage_GetFileType(missing_path(), 8) == FIF_UNKNOWN);
  CHECK(FreeImage_GetFileType(nullptr) == FIF_UNKNOWN);
  // §2.3 / API.md: the U variant has no POSIX path and always reports unknown.
  CHECK(FreeImage_GetFileTypeU(L"image.png") == FIF_UNKNOWN);
}

TEST_CASE("[format] GetFileType ignores the size hint for a real image per §2.3") {
  const fit::TempDir dir;
  const std::filesystem::path path = dir.file("fixture.bmp");
  fit::write_file(path, fit::tiny_bmp());

  // §2.3: "size is accepted but ignored by classic", so the ES-DE (path, 0) shape and an
  // explicit size must agree; BMP is registered, so detection must report it.
  const FREE_IMAGE_FORMAT hinted_zero = FreeImage_GetFileType(path.c_str(), 0);
  CHECK(hinted_zero == FIF_BMP);
  CHECK(FreeImage_GetFileType(path.c_str()) == hinted_zero);
  CHECK(FreeImage_GetFileType(path.c_str(), 2) == hinted_zero);
  CHECK(FreeImage_GetFileType(path.c_str(), 1 << 20) == hinted_zero);
}

TEST_CASE("[format] GetFileTypeFromMemory on garbage is unknown and leaves the stream at 0 per §2.3") {
  fit::Bytes buffer(64, 0x5A);
  const fit::Memory memory(FreeImage_OpenMemory(buffer.data(), static_cast<DWORD>(buffer.size())));
  REQUIRE(memory.get() != nullptr);

  // §2.3: validate must restore the position, because ES-DE loads immediately after
  // detection. Repeated identical calls must therefore be identical and start at 0.
  CHECK(FreeImage_TellMemory(memory.get()) == 0);
  CHECK(FreeImage_GetFileTypeFromMemory(memory.get()) == FIF_UNKNOWN);
  CHECK(FreeImage_TellMemory(memory.get()) == 0);
  CHECK(FreeImage_GetFileTypeFromMemory(memory.get(), 0) == FIF_UNKNOWN);
  CHECK(FreeImage_TellMemory(memory.get()) == 0);
  CHECK(FreeImage_GetFileTypeFromMemory(nullptr) == FIF_UNKNOWN);  // §2.1 null guard
}

TEST_CASE("[format] TIFF is absent: its magic stays undetectable and unreadable per API.md §1") {
  fit::Bytes buffer = fit::tiff_header();
  const fit::Memory memory(FreeImage_OpenMemory(buffer.data(), static_cast<DWORD>(buffer.size())));
  REQUIRE(memory.get() != nullptr);

  CHECK(FreeImage_GetFileTypeFromMemory(memory.get()) == FIF_UNKNOWN);
  CHECK(FreeImage_FIFSupportsReading(kUnregisteredTiff) == FALSE);
  CHECK(FreeImage_FIFSupportsWriting(kUnregisteredTiff) == FALSE);
  CHECK(FreeImage_LoadFromMemory(kUnregisteredTiff, memory.get()) == nullptr);
}

TEST_CASE("[format] GetFIFFromFilename matches only the five formats per §2.3") {
  CHECK(FreeImage_GetFIFFromFilename("artwork.bmp") == FIF_BMP);
  CHECK(FreeImage_GetFIFFromFilename("photo.jpg") == FIF_JPEG);
  CHECK(FreeImage_GetFIFFromFilename("photo.jpeg") == FIF_JPEG);
  CHECK(FreeImage_GetFIFFromFilename("theme.png") == FIF_PNG);
  CHECK(FreeImage_GetFIFFromFilename("anim.gif") == FIF_GIF);
  CHECK(FreeImage_GetFIFFromFilename("thumb.webp") == FIF_WEBP);

  // Case-insensitive match.
  CHECK(FreeImage_GetFIFFromFilename("THEME.PNG") == FIF_PNG);
  CHECK(FreeImage_GetFIFFromFilename("Photo.JpG") == FIF_JPEG);
  CHECK(FreeImage_GetFIFFromFilename("a.BmP") == FIF_BMP);

  // The extension is the text after the last dot; a bare name is matched whole.
  CHECK(FreeImage_GetFIFFromFilename("/tmp/a.bar.png") == FIF_PNG);
  CHECK(FreeImage_GetFIFFromFilename("png") == FIF_PNG);
  CHECK(FreeImage_GetFIFFromFilename("README") == FIF_UNKNOWN);
  CHECK(FreeImage_GetFIFFromFilename("archive.") == FIF_UNKNOWN);

  // Unchanged failure paths: unknown and out-of-scope formats.
  CHECK(FreeImage_GetFIFFromFilename("archive.xyz") == FIF_UNKNOWN);
  CHECK(FreeImage_GetFIFFromFilename("clip.avi") == FIF_UNKNOWN);
  CHECK(FreeImage_GetFIFFromFilename("scan.tiff") == FIF_UNKNOWN);
  CHECK(FreeImage_GetFIFFromFilename("old.tga") == FIF_UNKNOWN);
  CHECK(FreeImage_GetFIFFromFilename("pixmap.pnm") == FIF_UNKNOWN);
  CHECK(FreeImage_GetFIFFromFilename(nullptr) == FIF_UNKNOWN);
}

TEST_CASE("[format] FIFSupportsReading / FIFSupportsWriting cover exactly the five per §2.3") {
  for (const FREE_IMAGE_FORMAT fif : {FIF_BMP, FIF_JPEG, FIF_PNG, FIF_GIF, FIF_WEBP}) {
    INFO("fif " << static_cast<int>(fif));
    CHECK(FreeImage_FIFSupportsReading(fif) == TRUE);
    CHECK(FreeImage_FIFSupportsWriting(fif) == TRUE);
  }

  CHECK(FreeImage_FIFSupportsReading(FIF_UNKNOWN) == FALSE);
  CHECK(FreeImage_FIFSupportsWriting(FIF_UNKNOWN) == FALSE);
  // Classic ids with no plugin in the sparse registry, and ids past the registry.
  for (const int id : {-1, -2, 1, 3, 7, 14, 15, 17, 18, 26, 34, 36, 1000}) {
    const FREE_IMAGE_FORMAT fif = static_cast<FREE_IMAGE_FORMAT>(id);
    INFO("fif " << id);
    CHECK(FreeImage_FIFSupportsReading(fif) == FALSE);
    CHECK(FreeImage_FIFSupportsWriting(fif) == FALSE);
  }
}

TEST_CASE("[format] Load reports failure instead of throwing per §2.3 + §2.1") {
  CHECK(FreeImage_Load(FIF_BMP, missing_path()) == nullptr);
  CHECK(FreeImage_Load(FIF_BMP, nullptr) == nullptr);
  // An unregistered id can never load, whatever the file claims to be.
  CHECK(FreeImage_Load(kUnregisteredTiff, missing_path()) == nullptr);
  CHECK(FreeImage_LoadFromMemory(FIF_PNG, nullptr) == nullptr);
  FreeImage_Unload(nullptr);  // §2.1 null guard: no-op
}

TEST_CASE("[format] Save truncates before encoding even when the plugin rejects the dib per §4.5 + §4.4") {
  const fit::TempDir dir;
  const std::filesystem::path path = dir.file("miximage.gif");
  fit::write_file(path, fit::Bytes(64, 0x77));

  const fit::Bitmap dib = fit::bitmap_from_pattern(kWidth, kHeight, 32);
  REQUIRE_MESSAGE(dib.get() != nullptr, "ConvertFromRawBits must return a dib (§1.3)");

  // §4.4: GIF save of a 32bpp dib is rejected with no automatic quantize; §4.5: the
  // dispatcher opens the file "w+b" first, so the previous contents are already gone.
  // ES-DE overwrites in place and relies on there being no temp-file/rename dance.
  CHECK(FreeImage_Save(FIF_GIF, dib.get(), path.c_str(), GIF_DEFAULT) == FALSE);
  std::uintmax_t size = 0;
  REQUIRE_MESSAGE(fit::file_size(path, size), "the target file must still exist after a rejected save");
  CHECK(size == 0);
}
