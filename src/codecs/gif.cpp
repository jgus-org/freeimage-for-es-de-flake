// GIF codec. Backend is giflib 5.2.x (DGifOpen/Slurp for decode, EGifOpen/PutLine for
// encode), driven over the caller's FreeImageIO rather than stdio so file, memory and
// custom-stream loads share one path (§2.9). All state is per call - the giflib handle
// and every scratch buffer are local - so the codec satisfies the concurrent-distinct-
// objects requirement (§2.11).
//
// The playback compositor reproduces classic PluginGIF.cpp (design §2.7): a
// full-logical-canvas 32bpp image initialized to the global background with
// alpha 0, a backward walk that rewinds to a boundary page, then a forward
// replay that skips PREVIOUS-disposal frames, clears BACKGROUND-disposal rects,
// and blits each frame skipping its transparent index with painted alpha 255.
// Without GIF_PLAYBACK the raw frame is returned as 8/4/1bpp indexed with the
// frame's palette and a per-index transparency table. "FrameTime" (FIMD_ANIMATION,
// FIDT_LONG, count 1, length 4) carries the GCE delay in milliseconds
// (delay_cs * 10) on both paths.
#include "codecs/gif.h"

#include <gif_lib.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fi {
namespace {

// GIF stores every dimension as a 16-bit field, so 65535 is the largest legal
// value; a 64-megapixel bound per image rejects the allocation bombs a
// 65535x65535 descriptor would otherwise hand to giflib's decoder.
constexpr long kMaxDim = 65535;
constexpr std::uint64_t kMaxPixels = 64ull << 20;

constexpr std::size_t kSignatureBytes = 6;

// Classic GIF_DISPOSAL_* values match giflib's DISPOSE_* (0 unspecified, 1
// leave, 2 background, 3 previous).
constexpr int kDisposalBackground = 2;
constexpr int kDisposalPrevious = 3;

// Binds a giflib stream callback to the caller's FreeImageIO + fi_handle pair,
// which giflib hands back through GifFileType::UserData.
struct GifStream {
  FreeImageIO* io;
  fi_handle handle;
};

bool read_all(FreeImageIO const& io, fi_handle handle, void* buffer, std::size_t bytes) {
  return bytes == 0 || io.read_proc(buffer, 1, static_cast<unsigned>(bytes), handle) == bytes;
}

bool skip_bytes(FreeImageIO const& io, fi_handle handle, long bytes) {
  return io.seek_proc(handle, bytes, SEEK_CUR) == 0;
}

bool image_size_ok(long w, long h) {
  if (w <= 0 || h <= 0 || w > kMaxDim || h > kMaxDim) {
    return false;
  }
  return static_cast<std::uint64_t>(w) * static_cast<std::uint64_t>(h) <= kMaxPixels;
}

bool gif_signature(BYTE const* version) {
  return std::memcmp(version, "GIF87a", kSignatureBytes) == 0 || std::memcmp(version, "GIF89a", kSignatureBytes) == 0;
}

struct GifBounds {
  int image_count = 0;
  long logical_width = 0;
  long logical_height = 0;
};

// Reads only headers and block lengths (never pixel payloads) to reject a
// malformed or absurd stream before giflib allocates for a full decode, and to
// supply the image-descriptor count for PageCount - the same structural pass
// classic's Open performs.
bool scan_bounds(FreeImageIO const& io, fi_handle handle, GifBounds& out) {
  if (io.seek_proc(handle, 0, SEEK_SET) != 0) {
    return false;
  }
  std::array<BYTE, 13> header{};
  if (!read_all(io, handle, header.data(), header.size())) {
    return false;
  }
  if (!gif_signature(header.data())) {
    return false;
  }
  out.logical_width = static_cast<long>(header[6]) | (static_cast<long>(header[7]) << 8);
  out.logical_height = static_cast<long>(header[8]) | (static_cast<long>(header[9]) << 8);
  BYTE const packed = header[10];
  if (packed & 0x80) {
    if (!skip_bytes(io, handle, 3 * (2 << (packed & 0x07)))) {
      return false;
    }
  }
  bool overflow =
      out.logical_width < 1 || out.logical_width > kMaxDim || out.logical_height < 1 || out.logical_height > kMaxDim;
  for (;;) {
    BYTE block = 0;
    if (!read_all(io, handle, &block, 1)) {
      return false;
    }
    if (block == 0x3B) {  // GIF trailer
      break;
    }
    if (block == 0x2C) {  // image descriptor
      std::array<BYTE, 9> desc{};
      if (!read_all(io, handle, desc.data(), desc.size())) {
        return false;
      }
      long const w = static_cast<long>(desc[4]) | (static_cast<long>(desc[5]) << 8);
      long const h = static_cast<long>(desc[6]) | (static_cast<long>(desc[7]) << 8);
      overflow = overflow || !image_size_ok(w, h);
      if (desc[8] & 0x80) {
        if (!skip_bytes(io, handle, 3 * (2 << (desc[8] & 0x07)))) {
          return false;
        }
      }
      // The LZW minimum code size byte precedes the image-data sub-blocks.
      if (!skip_bytes(io, handle, 1)) {
        return false;
      }
      ++out.image_count;
    } else if (block == 0x21) {  // extension: skip its label byte, then the sub-blocks
      if (!skip_bytes(io, handle, 1)) {
        return false;
      }
    } else {  // not an image descriptor or an extension introducer
      return false;
    }
    BYTE len = 0;
    if (!read_all(io, handle, &len, 1)) {
      return false;
    }
    while (len != 0) {
      if (!skip_bytes(io, handle, len)) {
        return false;
      }
      if (!read_all(io, handle, &len, 1)) {
        return false;
      }
    }
  }
  return !overflow;
}

int GifInput(GifFileType* gif, GifByteType* dest, int length) {
  auto* stream = static_cast<GifStream*>(gif->UserData);
  return static_cast<int>(stream->io->read_proc(dest, 1, static_cast<unsigned>(length), stream->handle));
}

int GifOutput(GifFileType* gif, GifByteType const* src, int len) {
  auto* stream = static_cast<GifStream*>(gif->UserData);
  if (stream->io->write_proc == nullptr) {
    return 0;
  }
  return static_cast<int>(
      stream->io->write_proc(const_cast<GifByteType*>(src), 1, static_cast<unsigned>(len), stream->handle));
}

struct GifCloser {
  void operator()(GifFileType* gif) const {
    int err = 0;
    DGifCloseFile(gif, &err);
  }
};

using GifHandle = std::unique_ptr<GifFileType, GifCloser>;

// `stream` must outlive the returned handle's use; it is only read during the
// DGifOpen/DGifSlurp calls below, so the caller's stack object is sufficient.
GifHandle open_slurped(GifStream& stream) {
  GifHandle gif(DGifOpen(&stream, &GifInput, nullptr));
  if (gif == nullptr) {
    return nullptr;
  }
  if (DGifSlurp(gif.get()) != GIF_OK) {
    return nullptr;
  }
  return gif;
}

// The closing call flushes the buffered stream, so its verdict belongs to the
// write result; the closer reports it into the flag named when the handle is
// made, which must outlive the handle.
struct EgifCloser {
  bool* flushed;
  void operator()(GifFileType* gif) const {
    int err = 0;
    if (EGifCloseFile(gif, &err) != GIF_OK) {
      *flushed = false;
    }
  }
};

using EgifHandle = std::unique_ptr<GifFileType, EgifCloser>;

ColorMapObject const* effective_map(GifFileType const& gif, SavedImage const& si) {
  if (si.ImageDesc.ColorMap != nullptr) {
    return si.ImageDesc.ColorMap;
  }
  return gif.SColorMap;
}

struct Gcb {
  int disposal = 0;
  int delay_cs = 0;
  int trans = NO_TRANSPARENT_COLOR;
};

Gcb read_gcb(SavedImage const& si) {
  Gcb g;
  for (int i = 0; i < si.ExtensionBlockCount; ++i) {
    ExtensionBlock const& eb = si.ExtensionBlocks[i];
    if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
      GraphicsControlBlock gcb;
      if (DGifExtensionToGCB(static_cast<size_t>(eb.ByteCount), eb.Bytes, &gcb) == GIF_OK) {
        g.disposal = gcb.DisposalMode;
        g.delay_cs = gcb.DelayTime;
        g.trans = gcb.TransparentColor;
      }
      break;
    }
  }
  return g;
}

// bpp chooses the palette slot count already sized by Dib::make (1<<bpp).
void apply_map(Dib& d, ColorMapObject const* map) {
  std::span<RGBQUAD> const slots = d.palette();
  if (map == nullptr || map->Colors == nullptr) {
    // A GIF with no color table is legal; classic synthesises a greyscale ramp.
    for (std::size_t i = 0; i < slots.size(); ++i) {
      BYTE const level = static_cast<BYTE>(i);
      slots[i] = RGBQUAD{level, level, level, 0};
    }
    return;
  }
  for (std::size_t i = 0; i < slots.size(); ++i) {
    if (static_cast<int>(i) < map->ColorCount) {
      GifColorType const& c = map->Colors[i];
      slots[i] = RGBQUAD{c.Blue, c.Green, c.Red, 0};
    } else {
      slots[i] = RGBQUAD{0, 0, 0, 0};
    }
  }
}

void add_animation_tags(Dib& d, SavedImage const& si, Gcb const& gcb) {
  LONG const left = si.ImageDesc.Left;
  LONG const top = si.ImageDesc.Top;
  BYTE const no_local = si.ImageDesc.ColorMap == nullptr ? 1 : 0;
  BYTE const interlaced = si.ImageDesc.Interlace ? 1 : 0;
  LONG const frame_time = static_cast<LONG>(gcb.delay_cs) * 10;  // cs -> ms, design §2.7
  BYTE const disposal = static_cast<BYTE>(gcb.disposal);
  d.add(FIMD_ANIMATION, "FrameLeft", FIDT_LONG, scalar_bytes(left));
  d.add(FIMD_ANIMATION, "FrameTop", FIDT_LONG, scalar_bytes(top));
  d.add(FIMD_ANIMATION, "NoLocalPalette", FIDT_BYTE, scalar_bytes(no_local));
  d.add(FIMD_ANIMATION, "Interlaced", FIDT_BYTE, scalar_bytes(interlaced));
  d.add(FIMD_ANIMATION, "FrameTime", FIDT_LONG, scalar_bytes(frame_time));
  d.add(FIMD_ANIMATION, "DisposalMethod", FIDT_BYTE, scalar_bytes(disposal));
}

// Decodes one frame at its own dimensions. force256 (GIF_LOAD256) keeps the
// 8bpp representation even when the effective color table is small; otherwise a
// <=2 or <=16 color table drops to 1bpp/4bpp as classic's PluginGIF raw path does.
std::unique_ptr<Dib> load_raw(GifFileType const& gif, int page, bool force256) {
  SavedImage const& si = gif.SavedImages[page];
  int const width = si.ImageDesc.Width;
  int const height = si.ImageDesc.Height;
  if (width <= 0 || height <= 0) {
    return nullptr;
  }
  ColorMapObject const* map = effective_map(gif, si);
  int bpp = 8;
  if (!force256 && map != nullptr) {
    if (map->ColorCount <= 2) {
      bpp = 1;
    } else if (map->ColorCount <= 16) {
      bpp = 4;
    }
  }
  std::unique_ptr<Dib> d = Dib::make(width, height, bpp);
  if (d == nullptr) {
    return nullptr;
  }
  apply_map(*d, map);

  Gcb const gcb = read_gcb(si);
  if (gcb.trans >= 0 && gcb.trans < kMaxPalette) {
    d->set_transparent();
    d->transparency()[static_cast<std::size_t>(gcb.trans)] = 0;
  }

  std::span<GifPixelType const> const raster(
      si.RasterBits,
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height));
  for (int y = 0; y < height; ++y) {
    // giflib hands back a de-interlaced, top-down raster; scanline 0 is the
    // visual bottom, so raster row y lands at scanline height-1-y.
    std::span<GifPixelType const> const src = raster.subspan(static_cast<std::size_t>(y) * width, width);
    std::span<BYTE> const line = d->row(height - 1 - y);
    if (bpp == 8) {
      for (int x = 0; x < width; ++x) {
        line[x] = src[x];
      }
    } else if (bpp == 4) {
      for (int x = 0; x < width; ++x) {
        BYTE const nibble = src[x] & 0x0F;
        if ((x & 1) == 0) {
          line[x >> 1] = static_cast<BYTE>((line[x >> 1] & 0x0F) | (nibble << 4));
        } else {
          line[x >> 1] = static_cast<BYTE>((line[x >> 1] & 0xF0) | nibble);
        }
      }
    } else {  // bpp == 1
      for (int x = 0; x < width; ++x) {
        if ((src[x] & 1) != 0) {
          line[x >> 3] = static_cast<BYTE>(line[x >> 3] | (0x80 >> (x & 7)));
        }
      }
    }
  }

  add_animation_tags(*d, si, gcb);
  if (page == 0) {
    LONG const logical_width = gif.SWidth;
    LONG const logical_height = gif.SHeight;
    d->add(FIMD_ANIMATION, "LogicalWidth", FIDT_LONG, scalar_bytes(logical_width));
    d->add(FIMD_ANIMATION, "LogicalHeight", FIDT_LONG, scalar_bytes(logical_height));
  }
  return d;
}

std::unique_ptr<Dib> load_playback(GifFileType const& gif, int page) {
  int const lw = gif.SWidth;
  int const lh = gif.SHeight;
  if (lw <= 0 || lh <= 0) {
    return nullptr;
  }
  RGBQUAD bg{0, 0, 0, 0};
  ColorMapObject const* gct = gif.SColorMap;
  if (gct != nullptr && gif.SBackGroundColor >= 0 && gif.SBackGroundColor < gct->ColorCount) {
    GifColorType const& c = gct->Colors[gif.SBackGroundColor];
    bg = RGBQUAD{c.Blue, c.Green, c.Red, 0};
  }

  std::unique_ptr<Dib> d = Dib::make(lw, lh, 32, FI_RGBA_RED_MASK, FI_RGBA_GREEN_MASK, FI_RGBA_BLUE_MASK);
  if (d == nullptr) {
    return nullptr;
  }
  for (int y = 0; y < lh; ++y) {
    std::span<BYTE> const line = d->row(y);
    for (int x = 0; x < lw; ++x) {
      line[x * 4 + FI_RGBA_BLUE] = bg.rgbBlue;
      line[x * 4 + FI_RGBA_GREEN] = bg.rgbGreen;
      line[x * 4 + FI_RGBA_RED] = bg.rgbRed;
      line[x * 4 + FI_RGBA_ALPHA] = 0;
    }
  }

  // Backward walk for the boundary page (design §2.7): a full-canvas frame whose
  // disposal is BACKGROUND clears the canvas, so replay can start after it; a
  // full-canvas, non-PREVIOUS, opaque frame repaints the whole canvas, so replay
  // can start at it. Otherwise rewind to page 0.
  int start = page;
  while (start >= 0) {
    SavedImage const& si = gif.SavedImages[start];
    if (start != page) {
      Gcb const g = read_gcb(si);
      bool const full =
          si.ImageDesc.Left == 0 && si.ImageDesc.Top == 0 && si.ImageDesc.Width == lw && si.ImageDesc.Height == lh;
      if (full) {
        if (g.disposal == kDisposalBackground) {
          ++start;
          break;
        }
        if (g.disposal != kDisposalPrevious && g.trans == NO_TRANSPARENT_COLOR) {
          break;
        }
      }
    }
    --start;
  }
  if (start < 0) {
    start = 0;
  }

  for (int p = start; p <= page; ++p) {
    SavedImage const& si = gif.SavedImages[p];
    Gcb const g = read_gcb(si);
    if (p != page) {
      if (g.disposal == kDisposalPrevious) {
        continue;
      }
      if (g.disposal == kDisposalBackground) {
        for (int y = 0; y < si.ImageDesc.Height; ++y) {
          int const cy = lh - 1 - (si.ImageDesc.Top + y);
          if (cy < 0) {
            break;
          }
          std::span<BYTE> const line = d->row(cy);
          for (int x = 0; x < si.ImageDesc.Width; ++x) {
            int const cx = si.ImageDesc.Left + x;
            if (cx < 0 || cx >= lw) {
              continue;
            }
            line[cx * 4 + FI_RGBA_BLUE] = bg.rgbBlue;
            line[cx * 4 + FI_RGBA_GREEN] = bg.rgbGreen;
            line[cx * 4 + FI_RGBA_RED] = bg.rgbRed;
            line[cx * 4 + FI_RGBA_ALPHA] = 0;
          }
        }
        continue;
      }
    }
    ColorMapObject const* map = effective_map(gif, si);
    if (map == nullptr || si.RasterBits == nullptr) {
      continue;
    }
    for (int y = 0; y < si.ImageDesc.Height; ++y) {
      int const cy = lh - 1 - (si.ImageDesc.Top + y);
      if (cy < 0) {
        break;
      }
      std::span<BYTE> const line = d->row(cy);
      for (int x = 0; x < si.ImageDesc.Width; ++x) {
        int const cx = si.ImageDesc.Left + x;
        if (cx < 0 || cx >= lw) {
          continue;
        }
        int const idx = si.RasterBits[static_cast<std::size_t>(y) * si.ImageDesc.Width + x];
        if (g.trans != NO_TRANSPARENT_COLOR && idx == g.trans) {
          continue;
        }
        GifColorType const c = idx < map->ColorCount ? map->Colors[idx] : GifColorType{0, 0, 0};
        line[cx * 4 + FI_RGBA_BLUE] = c.Blue;
        line[cx * 4 + FI_RGBA_GREEN] = c.Green;
        line[cx * 4 + FI_RGBA_RED] = c.Red;
        line[cx * 4 + FI_RGBA_ALPHA] = 255;
      }
    }
  }

  Gcb const end_gcb = read_gcb(gif.SavedImages[page]);
  LONG const frame_time = static_cast<LONG>(end_gcb.delay_cs) * 10;
  d->add(FIMD_ANIMATION, "FrameTime", FIDT_LONG, scalar_bytes(frame_time));
  return d;
}

}  // namespace

std::string_view GifCodec::format_name() const { return "GIF"; }

std::string_view GifCodec::extensions() const { return "gif"; }

std::string_view GifCodec::description() const { return "Graphics Interchange Format"; }

// Classic ValidateGIF reads the six-byte signature and lets the dispatcher
// reposition; detect() seeks to 0 before and after each probe, so no seek-back
// is needed here (the same house pattern as bmp.cpp's validate).
bool GifCodec::validate(FreeImageIO const& io, fi_handle handle) const {
  std::array<BYTE, kSignatureBytes> signature{};
  if (!read_all(io, handle, signature.data(), signature.size())) {
    return false;
  }
  return gif_signature(signature.data());
}

std::unique_ptr<Dib> GifCodec::load(FreeImageIO& io, fi_handle handle, int page, int flags) const {
  if (handle == nullptr) {
    return nullptr;
  }
  if (page == -1) {
    page = 0;
  }
  GifBounds bounds;
  if (!scan_bounds(io, handle, bounds) || page < 0 || page >= bounds.image_count) {
    return nullptr;
  }
  if (io.seek_proc(handle, 0, SEEK_SET) != 0) {
    return nullptr;
  }
  GifStream stream{&io, handle};
  GifHandle gif = open_slurped(stream);
  if (gif == nullptr || page >= gif->ImageCount) {
    return nullptr;
  }
  if ((flags & GIF_PLAYBACK) == GIF_PLAYBACK) {
    return load_playback(*gif, page);
  }
  return load_raw(*gif, page, (flags & GIF_LOAD256) == GIF_LOAD256);
}

// The graphic control extension precedes the image descriptor it governs;
// giflib writes each EGifPut* call to the stream immediately, so the GCE must be
// emitted before EGifPutImageDesc or the reader mis-parses it as image data.
namespace {

bool put_image(GifFileType& gif, Dib const& d, int delay_ms, int disposal, int trans_color) {
  int const width = d.width();
  int const height = d.height();
  std::array<GifColorType, kMaxPalette> colors{};
  for (int i = 0; i < kMaxPalette; ++i) {
    RGBQUAD const& q = i < static_cast<int>(d.palette().size()) ? d.palette()[i] : RGBQUAD{};
    colors[i] = GifColorType{q.rgbRed, q.rgbGreen, q.rgbBlue};
  }
  // One 256-entry table serves 1, 4 and 8 bpp alike.
  ColorMapObject cmap;
  cmap.ColorCount = kMaxPalette;
  cmap.BitsPerPixel = 8;
  cmap.SortFlag = false;
  cmap.Colors = colors.data();

  if (EGifPutScreenDesc(&gif, width, height, 8, 0, &cmap) != GIF_OK) {
    return false;
  }

  GraphicsControlBlock gcb;
  gcb.DisposalMode = disposal;
  gcb.UserInputFlag = false;
  gcb.DelayTime = delay_ms / 10;  // ms -> cs
  gcb.TransparentColor = trans_color;
  GifByteType ext[4];
  size_t const ext_len = EGifGCBToExtension(&gcb, ext);
  if (EGifPutExtensionLeader(&gif, GRAPHICS_EXT_FUNC_CODE) != GIF_OK ||
      EGifPutExtensionBlock(&gif, static_cast<int>(ext_len), ext) != GIF_OK ||
      EGifPutExtensionTrailer(&gif) != GIF_OK) {
    return false;
  }

  if (EGifPutImageDesc(&gif, 0, 0, width, height, false, &cmap) != GIF_OK) {
    return false;
  }

  std::vector<GifPixelType> row(static_cast<std::size_t>(width));
  for (int y = 0; y < height; ++y) {
    // scanline height-1-y is the visual top row, which is GIF row y.
    BYTE const* line = d.row(height - 1 - y).data();
    for (int x = 0; x < width; ++x) {
      if (d.bpp() == 8) {
        row[x] = line[x];
      } else if (d.bpp() == 4) {
        row[x] = (x & 1) == 0 ? static_cast<GifPixelType>(line[x >> 1] >> 4)
                              : static_cast<GifPixelType>(line[x >> 1] & 0x0F);
      } else {
        row[x] = static_cast<GifPixelType>((line[x >> 3] & (0x80 >> (x & 7))) != 0 ? 1 : 0);
      }
    }
    if (EGifPutLine(&gif, row.data(), width) != GIF_OK) {
      return false;
    }
  }
  return true;
}

}  // namespace

bool GifCodec::save(FreeImageIO& io, Dib& dib, fi_handle handle, int /*flags*/) const {
  if (!dib.has_pixels() || !dib.is_bitmap()) {
    return false;
  }
  if (dib.bpp() != 1 && dib.bpp() != 4 && dib.bpp() != 8) {
    FreeImage_OutputMessageProc(FIF_GIF, "GIF: only 1, 4 or 8 bpp images can be saved");
    return false;
  }
  if (dib.width() <= 0 || dib.height() <= 0 || dib.width() > kMaxDim || dib.height() > kMaxDim) {
    return false;
  }

  int delay_ms = 100;  // classic default when no FrameTime tag is present
  if (FITAG const* time_tag = dib.find(FIMD_ANIMATION, "FrameTime"); time_tag != nullptr) {
    Tag const* tag = stored_tag(time_tag);
    if (tag != nullptr && tag->value.size() >= sizeof(LONG)) {
      LONG ms = 0;
      std::memcpy(&ms, tag->value.data(), sizeof(ms));
      delay_ms = static_cast<int>(ms);
    }
  }

  int disposal = kDisposalBackground;  // design §3 default
  if (FITAG const* disposal_tag = dib.find(FIMD_ANIMATION, "DisposalMethod"); disposal_tag != nullptr) {
    Tag const* tag = stored_tag(disposal_tag);
    if (tag != nullptr && !tag->value.empty()) {
      disposal = tag->value[0];
    }
  }

  int trans_color = NO_TRANSPARENT_COLOR;
  if (dib.is_transparent()) {
    std::span<BYTE const> const alpha = dib.transparency();
    for (int i = 0; i < kMaxPalette; ++i) {
      if (alpha[static_cast<std::size_t>(i)] == 0) {
        trans_color = i;
        break;
      }
    }
  }

  bool written = false;
  bool flushed = true;
  {
    GifStream stream{&io, handle};
    GifFileType* opened = EGifOpen(&stream, &GifOutput, nullptr);
    if (opened == nullptr) {
      return false;
    }
    EgifHandle const gif(opened, EgifCloser{&flushed});
    EGifSetGifVersion(gif.get(), true);  // GIF89a: the GCE requires the 89 container
    written = put_image(*gif, dib, delay_ms, disposal, trans_color);
  }  // closing the handle flushes the stream and reports into `flushed`
  if (!written || !flushed) {
    FreeImage_OutputMessageProc(FIF_GIF, "GIF: failed to write the image stream");
    return false;
  }
  return true;
}

int GifCodec::page_count(FreeImageIO& io, fi_handle handle) const {
  if (handle == nullptr) {
    return 0;
  }
  GifBounds bounds;
  if (!scan_bounds(io, handle, bounds)) {
    return 0;
  }
  return bounds.image_count;
}

GifCodec const gif_codec;

}  // namespace fi
