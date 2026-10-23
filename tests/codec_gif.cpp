// Animated-GIF handling through the ES-DE surface: a custom FreeImageIO bound to
// a fopen("r+b") FILE*, FreeImage_OpenMultiBitmapFromHandle(..., GIF_PLAYBACK),
// FreeImage_LockPage, and the per-frame re-open-over-the-SAME-FILE* loop, all
// reproduced with the exact call shapes from tests/esde_surface.cpp. The §2.7
// compositing expectation is produced by an INDEPENDENT reference compositor in
// this file (not the library), so the golden is an oracle rather than a hand
// guess. Fixtures are built by helpers_codecs' GIF89a byte builder. In the red
// phase every multipage entry point returns its failure value, so the
// structural "produced an image" assertions are deterministic failures and all
// pixel comparisons are guarded (no crashes).
#include <FreeImage.h>
#include <doctest/doctest.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include "helpers.h"
#include "helpers_codecs.h"

namespace {

using fic::GifFrame;
using fic::GifPalette;
using fic::Image;
using fic::Rgba;

// --- ES-DE GIFAnimComponent IO callbacks (verbatim POSIX branch) ------------

unsigned readProc(void* buffer, unsigned size, unsigned count, fi_handle handle) {
  return static_cast<unsigned>(fread(buffer, size, count, reinterpret_cast<FILE*>(handle)));
}
unsigned writeProc(void* buffer, unsigned size, unsigned count, fi_handle handle) {
  return static_cast<unsigned>(fwrite(buffer, size, count, reinterpret_cast<FILE*>(handle)));
}
int seekProc(fi_handle handle, long offset, int origin) {
  return fseek(reinterpret_cast<FILE*>(handle), offset, origin);
}
long tellProc(fi_handle handle) { return ftell(reinterpret_cast<FILE*>(handle)); }

FreeImageIO MakeIO() {
  FreeImageIO io;
  io.read_proc = readProc;
  io.write_proc = writeProc;
  io.seek_proc = seekProc;
  io.tell_proc = tellProc;
  return io;
}

// --- fixture file plumbing --------------------------------------------------

bool WriteBytes(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
  FILE* f = fopen(path.c_str(), "wb");
  if (!f) {
    return false;
  }
  const bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
  fclose(f);
  return ok;
}

// The ES-DE call shapes keep a FILE* alive across the multibitmap's whole life, so
// the tests own one too; closing it in the destructor means a REQUIRE that throws
// out of the case cannot leak the handle.
class FileHandle {
 public:
  FileHandle(const std::filesystem::path& path, const char* mode) : handle_(fopen(path.c_str(), mode)) {}
  ~FileHandle() {
    if (handle_ != nullptr) {
      fclose(handle_);
    }
  }
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  FILE* get() const { return handle_; }

 private:
  FILE* handle_;
};

// --- independent §2.7 reference compositor ----------------------------------
//
// canvas = logical WxH 32bpp, fully initialized to global_palette[bg] with
// alpha 0. Replay frames 0..P; each frame's pixels are blitted at (left,top)
// skipping the transparent index (writing palette RGB with alpha 255). A
// snapshot is taken before each frame so disposal PREVIOUS(3) can restore it;
// BACKGROUND(2) fills the frame rect with the background for the NEXT frame.
// The frame being displayed (k == P) is shown regardless of its own disposal.
Image Compose(const std::vector<Rgba>& pal, int bg, int W, int H, const std::vector<GifFrame>& fr, int P) {
  const Rgba bgc{pal[bg].r, pal[bg].g, pal[bg].b, 0};
  Image canvas{W, H, std::vector<Rgba>(static_cast<size_t>(W) * H, bgc)};
  for (int k = 0; k <= P; ++k) {
    const GifFrame& f = fr[k];
    Image snap = canvas;
    const std::vector<Rgba>& table = f.local.empty() ? pal : f.local;
    for (int y = 0; y < f.h; ++y) {
      for (int x = 0; x < f.w; ++x) {
        const int lx = f.left + x;
        const int ly = f.top + y;
        if (lx < 0 || ly < 0 || lx >= W || ly >= H) {
          continue;
        }
        const uint8_t idx = f.idx[static_cast<size_t>(y) * f.w + x];
        if (f.transparent && static_cast<int>(idx) == f.trans_index) {
          continue;
        }
        Rgba c = table[idx];
        c.a = 255;
        canvas.px[static_cast<size_t>(ly) * W + lx] = c;
      }
    }
    if (k < P) {
      if (f.disposal == 2) {
        for (int y = 0; y < f.h; ++y) {
          for (int x = 0; x < f.w; ++x) {
            const int lx = f.left + x;
            const int ly = f.top + y;
            if (lx >= 0 && ly >= 0 && lx < W && ly < H) {
              canvas.px[static_cast<size_t>(ly) * W + lx] = bgc;
            }
          }
        }
      } else if (f.disposal == 3) {
        canvas = snap;
      }
    }
  }
  return canvas;
}

// Read a canvas pixel at LOGICAL top coordinates (x, yt); the DIB is bottom-up.
Rgba CanvasPx(FIBITMAP* dib, int x, int yt) {
  RGBQUAD q{0, 0, 0, 0};
  const unsigned h = dib ? FreeImage_GetHeight(dib) : 0;
  FreeImage_GetPixelColor(dib, static_cast<unsigned>(x), h - 1 - static_cast<unsigned>(yt), &q);
  return Rgba{q.rgbRed, q.rgbGreen, q.rgbBlue, q.rgbReserved};
}

bool CanvasEq(FIBITMAP* dib, const Image& ref) {
  if (!dib) {
    return false;
  }
  if (FreeImage_GetWidth(dib) != static_cast<unsigned>(ref.w) ||
      FreeImage_GetHeight(dib) != static_cast<unsigned>(ref.h)) {
    return false;
  }
  if (FreeImage_GetBPP(dib) != 32) {
    return false;
  }
  for (int y = 0; y < ref.h; ++y) {
    for (int x = 0; x < ref.w; ++x) {
      if (!(CanvasPx(dib, x, y) == ref.at(x, y))) {
        return false;
      }
    }
  }
  return true;
}

// --- fixtures ---------------------------------------------------------------

std::vector<Rgba> AnimPal() {
  std::vector<Rgba> p(256, Rgba{0, 0, 0, 255});
  p[0] = Rgba{10, 20, 30, 255};  // background
  p[1] = Rgba{200, 0, 0, 255};   // red
  p[2] = Rgba{0, 200, 0, 255};   // green
  p[3] = Rgba{0, 0, 200, 255};   // blue
  return p;
}

GifPalette PalOf(const std::vector<Rgba>& p) {
  GifPalette gp;
  gp.entries = p;
  return gp;
}

std::vector<uint8_t> Fill(int n, uint8_t v) { return std::vector<uint8_t>(n, v); }

// 8x8 logical. 6 frames exercising every disposal mode + a transparent
// full-canvas frame + sub-canvas offsets.
std::vector<GifFrame> AnimFrames() {
  std::vector<GifFrame> f(6);
  f[0].left = f[0].top = 0;
  f[0].w = f[0].h = 4;
  f[0].idx = Fill(16, 2);
  f[0].disposal = 0;
  f[0].delay_cs = 10;

  f[1].left = f[1].top = 4;
  f[1].w = f[1].h = 4;
  f[1].idx = Fill(16, 1);
  f[1].disposal = 0;
  f[1].delay_cs = 20;

  f[2].left = f[2].top = 0;
  f[2].w = f[2].h = 8;
  f[2].idx = Fill(64, 3);
  f[2].idx[0] = f[2].idx[1] = f[2].idx[8] = f[2].idx[9] = 0;  // top-left 2x2 transparent
  f[2].disposal = 2;
  f[2].transparent = true;
  f[2].trans_index = 0;
  f[2].delay_cs = 30;

  f[3].left = f[3].top = 2;
  f[3].w = f[3].h = 4;
  f[3].idx = Fill(16, 1);
  f[3].disposal = 0;
  f[3].delay_cs = 40;

  f[4].left = 0;
  f[4].top = 6;
  f[4].w = f[4].h = 2;
  f[4].idx = Fill(4, 2);
  f[4].disposal = 3;
  f[4].delay_cs = 50;

  f[5].left = 6;
  f[5].top = 2;
  f[5].w = f[5].h = 2;
  f[5].idx = Fill(4, 3);
  f[5].disposal = 0;
  f[5].delay_cs = 60;
  return f;
}

// 8x8 logical. Exercises BACKGROUND(2) clearing its own rect mid-replay and the
// interaction with a following frame.
std::vector<GifFrame> BgDisFrames() {
  std::vector<GifFrame> f(3);
  f[0].w = f[0].h = 8;
  f[0].idx = Fill(64, 1);
  f[0].disposal = 0;
  f[0].delay_cs = 10;

  f[1].left = f[1].top = 2;
  f[1].w = f[1].h = 3;
  f[1].idx = Fill(9, 2);
  f[1].disposal = 2;
  f[1].delay_cs = 20;

  f[2].w = f[2].h = 2;
  f[2].idx = Fill(4, 2);
  f[2].disposal = 0;
  f[2].delay_cs = 30;
  return f;
}

// 4x4, single frame, one transparent index: the raw (non-playback) load path.
std::vector<GifFrame> RawFrames() {
  std::vector<GifFrame> f(1);
  f[0].w = f[0].h = 4;
  f[0].idx = Fill(16, 2);
  f[0].idx[0] = 1;  // (0,0) is the transparent index
  f[0].transparent = true;
  f[0].trans_index = 1;
  f[0].delay_cs = 15;
  return f;
}

}  // namespace

// --- §2.7 playback compositing ----------------------------------------------

TEST_CASE("[gif §2.7] GIF_PLAYBACK LockPage composites each frame on the logical canvas") {
  const fit::TempDir dir;
  const std::vector<Rgba> pal = AnimPal();
  const std::vector<GifFrame> frames = AnimFrames();
  const std::filesystem::path path = dir.file("anim.gif");
  REQUIRE(WriteBytes(path, fic::BuildGif(8, 8, PalOf(pal), 0, frames)));

  const FileHandle file(path, "r+b");
  REQUIRE(file.get() != nullptr);
  FreeImageIO io = MakeIO();

  FIMULTIBITMAP* mm =
      FreeImage_OpenMultiBitmapFromHandle(FIF_GIF, &io, static_cast<fi_handle>(file.get()), GIF_PLAYBACK);
  CHECK(mm != nullptr);
  CHECK(FreeImage_GetPageCount(mm) == 6);

  for (int p = 0; p < 6; ++p) {
    FIBITMAP* page = FreeImage_LockPage(mm, p);
    CHECK(page != nullptr);
    const Image golden = Compose(pal, 0, 8, 8, frames, p);
    CHECK(CanvasEq(page, golden));  // full-canvas 32bpp == reference composite
    if (page) {
      FreeImage_UnlockPage(mm, page, FALSE);
    }
  }

  CHECK(FreeImage_CloseMultiBitmap(mm, 0) == TRUE);
  // FreeImage must NOT close the caller's FILE* (ES-DE keeps mAnimFile open).
  CHECK(file.get() != nullptr);
}

TEST_CASE("[gif §2.7] LockPage returns the full logical canvas (8x8) at 32bpp") {
  const fit::TempDir dir;
  const std::vector<Rgba> pal = AnimPal();
  const std::vector<GifFrame> frames = AnimFrames();
  const std::filesystem::path path = dir.file("canvas.gif");
  REQUIRE(WriteBytes(path, fic::BuildGif(8, 8, PalOf(pal), 0, frames)));
  const FileHandle file(path, "r+b");
  REQUIRE(file.get() != nullptr);
  FreeImageIO io = MakeIO();
  FIMULTIBITMAP* mm =
      FreeImage_OpenMultiBitmapFromHandle(FIF_GIF, &io, static_cast<fi_handle>(file.get()), GIF_PLAYBACK);
  FIBITMAP* page = FreeImage_LockPage(mm, 0);
  CHECK(page != nullptr);
  if (page) {
    CHECK(FreeImage_GetBPP(page) == 32);
    CHECK(FreeImage_GetWidth(page) == 8u);   // logical width, not frame width
    CHECK(FreeImage_GetHeight(page) == 8u);  // logical height
    FreeImage_UnlockPage(mm, page, FALSE);
  }
  FreeImage_CloseMultiBitmap(mm, 0);
}

TEST_CASE("[gif §2.7] BACKGROUND-disposal fixture composites to the reference (rect clears mid-replay)") {
  const fit::TempDir dir;
  const std::vector<Rgba> pal = AnimPal();
  const std::vector<GifFrame> frames = BgDisFrames();
  const std::filesystem::path path = dir.file("bgdis.gif");
  REQUIRE(WriteBytes(path, fic::BuildGif(8, 8, PalOf(pal), 0, frames)));
  const FileHandle file(path, "r+b");
  REQUIRE(file.get() != nullptr);
  FreeImageIO io = MakeIO();
  FIMULTIBITMAP* mm =
      FreeImage_OpenMultiBitmapFromHandle(FIF_GIF, &io, static_cast<fi_handle>(file.get()), GIF_PLAYBACK);
  for (int p = 0; p < 3; ++p) {
    FIBITMAP* page = FreeImage_LockPage(mm, p);
    CHECK(page != nullptr);
    CHECK(CanvasEq(page, Compose(pal, 0, 8, 8, frames, p)));
    if (page) {
      FreeImage_UnlockPage(mm, page, FALSE);
    }
  }
  FreeImage_CloseMultiBitmap(mm, 0);
}

// --- §2.7 FrameTime metadata (stored as delay_cs * 10 milliseconds) ---------
//
// NOTE (contract conflict): design §2.7 prose describes the tag as 1/100 s, but
// the classic GIF plugin stores delay_cs * 10, i.e. milliseconds, which is what
// ES-DE consumes (it gates on 20..1000 ms). The pinned value follows the
// implemented/classic behavior: FrameTime == delay_cs * 10.

TEST_CASE("[gif §2.7] FIMD_ANIMATION 'FrameTime' equals the end page's delay_cs*10 (ms)") {
  const fit::TempDir dir;
  const std::vector<Rgba> pal = AnimPal();
  const std::vector<GifFrame> frames = AnimFrames();
  const std::filesystem::path path = dir.file("ftime.gif");
  REQUIRE(WriteBytes(path, fic::BuildGif(8, 8, PalOf(pal), 0, frames)));
  const FileHandle file(path, "r+b");
  REQUIRE(file.get() != nullptr);
  FreeImageIO io = MakeIO();
  FIMULTIBITMAP* mm =
      FreeImage_OpenMultiBitmapFromHandle(FIF_GIF, &io, static_cast<fi_handle>(file.get()), GIF_PLAYBACK);

  for (int p = 0; p < 6; ++p) {
    FIBITMAP* page = FreeImage_LockPage(mm, p);
    if (!page) {
      CHECK(page != nullptr);
      continue;
    }
    FITAG* tag = nullptr;
    CHECK(FreeImage_GetMetadata(FIMD_ANIMATION, page, "FrameTime", &tag) == TRUE);
    CHECK(tag != nullptr);
    if (tag) {
      CHECK(FreeImage_GetTagCount(tag) == 1u);
      const uint32_t ms = *static_cast<const uint32_t*>(FreeImage_GetTagValue(tag));
      CHECK(ms == static_cast<uint32_t>(frames[p].delay_cs) * 10u);
    }
    FreeImage_UnlockPage(mm, page, FALSE);
  }

  FreeImage_CloseMultiBitmap(mm, 0);
}

// --- ES-DE per-frame re-open-over-the-SAME-FILE* loop -----------------------

TEST_CASE("[gif §2.7] ES-DE render loop re-opens the same FILE* handle for every frame") {
  const fit::TempDir dir;
  const std::vector<Rgba> pal = AnimPal();
  const std::vector<GifFrame> frames = AnimFrames();
  const std::filesystem::path path = dir.file("reopen.gif");
  REQUIRE(WriteBytes(path, fic::BuildGif(8, 8, PalOf(pal), 0, frames)));
  const FileHandle file(path, "r+b");
  REQUIRE(file.get() != nullptr);
  FreeImageIO io = MakeIO();

  // Mirrors GIFAnimComponent::render(): open/close the multibitmap per frame,
  // always on the SAME mAnimFile handle.
  for (int frameNum = 0; frameNum < 6; ++frameNum) {
    FIMULTIBITMAP* mm =
        FreeImage_OpenMultiBitmapFromHandle(FIF_GIF, &io, static_cast<fi_handle>(file.get()), GIF_PLAYBACK);
    CHECK(mm != nullptr);
    FIBITMAP* page = FreeImage_LockPage(mm, frameNum);
    CHECK(page != nullptr);
    CHECK(CanvasEq(page, Compose(pal, 0, 8, 8, frames, frameNum)));
    if (page) {
      FreeImage_UnlockPage(mm, page, false);
    }
    CHECK(FreeImage_CloseMultiBitmap(mm, 0) == TRUE);
  }

  // The file handle stayed valid across every open/close cycle.
  CHECK(ftell(file.get()) >= 0);
}

// --- multibitmap edge contracts ---------------------------------------------

TEST_CASE("[gif §2.7] LockPage of an already-locked page returns NULL; out-of-range NULL; unlock ignored") {
  const fit::TempDir dir;
  const std::vector<Rgba> pal = AnimPal();
  const std::vector<GifFrame> frames = AnimFrames();
  const std::filesystem::path path = dir.file("edges.gif");
  REQUIRE(WriteBytes(path, fic::BuildGif(8, 8, PalOf(pal), 0, frames)));
  const FileHandle file(path, "r+b");
  REQUIRE(file.get() != nullptr);
  FreeImageIO io = MakeIO();
  FIMULTIBITMAP* mm =
      FreeImage_OpenMultiBitmapFromHandle(FIF_GIF, &io, static_cast<fi_handle>(file.get()), GIF_PLAYBACK);
  CHECK(mm != nullptr);

  FIBITMAP* first = FreeImage_LockPage(mm, 0);
  CHECK(first != nullptr);
  // The same page is now locked; a second lock must fail with NULL.
  FIBITMAP* again = FreeImage_LockPage(mm, 0);
  CHECK(again == nullptr);

  // Out-of-range pages yield NULL (before and after the valid range).
  CHECK(FreeImage_LockPage(mm, -1) == nullptr);
  CHECK(FreeImage_LockPage(mm, FreeImage_GetPageCount(mm)) == nullptr);

  // Unlocking a pointer that is not locked must be ignored, not crash.
  FreeImage_UnlockPage(mm, first, FALSE);
  FreeImage_UnlockPage(mm, nullptr, FALSE);

  CHECK(FreeImage_CloseMultiBitmap(mm, 0) == TRUE);
}

// --- raw (non-playback) load path -------------------------------------------

TEST_CASE("[gif §3/§2.7] raw GIF load is 8bpp with a palette; ConvertTo32 honors transparency") {
  const fit::TempDir dir;
  const std::vector<Rgba> pal = AnimPal();
  const std::vector<GifFrame> frames = RawFrames();
  const std::filesystem::path path = dir.file("raw.gif");
  REQUIRE(WriteBytes(path, fic::BuildGif(4, 4, PalOf(pal), 0, frames)));

  FIBITMAP* dib = FreeImage_Load(FIF_GIF, path.c_str(), 0);  // no GIF_PLAYBACK
  CHECK(dib != nullptr);
  if (dib) {
    CHECK(FreeImage_GetBPP(dib) == 8);  // 256-entry global palette forces 8bpp
    CHECK(FreeImage_GetWidth(dib) == 4u);
    CHECK(FreeImage_GetHeight(dib) == 4u);

    FIBITMAP* px = FreeImage_ConvertTo32Bits(dib);
    CHECK(px != nullptr);
    if (px) {
      // (0,0) is the transparent index -> alpha 0; the rest are green, alpha 255.
      CHECK(CanvasPx(px, 0, 0).a == 0);
      CHECK(CanvasPx(px, 3, 3).a == 255);
      CHECK(CanvasPx(px, 3, 3).g == 200);
      CHECK(CanvasPx(px, 3, 3).r == 0);
      FreeImage_Unload(px);
    }

    FITAG* tag = nullptr;
    if (FreeImage_GetMetadata(FIMD_ANIMATION, dib, "FrameTime", &tag) && tag) {
      const uint32_t ms = *static_cast<const uint32_t*>(FreeImage_GetTagValue(tag));
      CHECK(ms == 150u);  // delay_cs 15 * 10
    }
    FreeImage_Unload(dib);
  }
}
