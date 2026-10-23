// Implementation of the independent reference codecs. See helpers_codecs.h.
#include "helpers_codecs.h"

// jpeglib.h uses FILE without including <stdio.h> (jpeglib.h:984), so <cstdio> must stay
// ahead of it; Google style regroups and re-sorts includes across blank lines, so the
// block is fenced to keep that order through the formatter.
// clang-format off
#include <algorithm>
#include <cmath>
#include <csetjmp>
#include <cstdio>
#include <cstring>

#include <gif_lib.h>
#include <jpeglib.h>
#include <png.h>
#include <webp/decode.h>
#include <webp/encode.h>
// clang-format on

namespace fic {
namespace {

constexpr int kLumaR = 54;   // 0.2126 * 256
constexpr int kLumaG = 183;  // 0.7152 * 256
constexpr int kLumaB = 19;   // 0.0722 * 256

uint8_t Luma(const Rgba& p) { return static_cast<uint8_t>((kLumaR * p.r + kLumaG * p.g + kLumaB * p.b + 128) >> 8); }

void PutLE16(std::vector<uint8_t>& v, uint16_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xff));
  v.push_back(static_cast<uint8_t>(x >> 8));
}

void PutLE32(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back(static_cast<uint8_t>(x & 0xff));
  v.push_back(static_cast<uint8_t>((x >> 8) & 0xff));
  v.push_back(static_cast<uint8_t>((x >> 16) & 0xff));
  v.push_back(static_cast<uint8_t>((x >> 24) & 0xff));
}

// --- libpng write (classic API, grow-to-memory) ---------------------------

void PngWriteCb(png_structp png, png_bytep data, png_size_t len) {
  auto* out = static_cast<std::vector<uint8_t>*>(png_get_io_ptr(png));
  out->insert(out->end(), data, data + len);
}

// Writes `img` as an 8-bit PNG of the given color type using rows supplied by
// the caller (row_bytes == w * bytes_per_pixel, top-down in file order).
std::vector<uint8_t>
PngEncode(const Image& img, int color_type, int bytes_per_pixel, const std::vector<uint8_t>& rows, int interlace) {
  std::vector<uint8_t> out;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    return out;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    return out;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return {};
  }
  png_set_write_fn(png, &out, &PngWriteCb, nullptr);
  png_set_IHDR(
      png,
      info,
      static_cast<png_uint_32>(img.w),
      static_cast<png_uint_32>(img.h),
      8,
      color_type,
      interlace,
      PNG_COMPRESSION_TYPE_BASE,
      PNG_FILTER_TYPE_BASE);
  png_write_info(png, info);
  for (int y = 0; y < img.h; ++y) {
    png_write_row(png, rows.data() + static_cast<size_t>(y) * img.w * bytes_per_pixel);
  }
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return out;
}

}  // namespace

Image Pattern(int w, int h) {
  Image img;
  img.w = w;
  img.h = h;
  img.px.resize(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      Rgba p;
      p.r = static_cast<uint8_t>(x * 16 & 0xff);
      p.g = static_cast<uint8_t>(y * 24 & 0xff);
      p.b = static_cast<uint8_t>((y * w + x) & 0xff);
      p.a = static_cast<uint8_t>(50 + (x * 7 + y * 13) % 206);  // range [50,255]: semi-transparent + opaque
      img.px[static_cast<size_t>(y) * w + x] = p;
    }
  }
  return img;
}

Image Solid(int w, int h, Rgba c) {
  Image img;
  img.w = w;
  img.h = h;
  img.px.assign(static_cast<size_t>(w) * h, c);
  return img;
}

Image Split(int w, int h, Rgba top, Rgba bottom) {
  Image img;
  img.w = w;
  img.h = h;
  img.px.resize(static_cast<size_t>(w) * h);
  for (int y = 0; y < h; ++y) {
    Rgba c = (y < h / 2) ? top : bottom;
    for (int x = 0; x < w; ++x) {
      img.px[static_cast<size_t>(y) * w + x] = c;
    }
  }
  return img;
}

// --- PNG ------------------------------------------------------------------

namespace {
std::vector<uint8_t> RgbaRows(const Image& img) {
  std::vector<uint8_t> rows(static_cast<size_t>(img.w) * img.h * 4);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      Rgba p = img.at(x, y);
      uint8_t* d = rows.data() + (static_cast<size_t>(y) * img.w + x) * 4;
      d[0] = p.r;
      d[1] = p.g;
      d[2] = p.b;
      d[3] = p.a;
    }
  }
  return rows;
}
}  // namespace

std::vector<uint8_t> EncodePngRGBA(const Image& img) {
  return PngEncode(img, PNG_COLOR_TYPE_RGBA, 4, RgbaRows(img), PNG_INTERLACE_NONE);
}

std::vector<uint8_t> EncodePngRGB(const Image& img) {
  std::vector<uint8_t> rows(static_cast<size_t>(img.w) * img.h * 3);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      Rgba p = img.at(x, y);
      uint8_t* d = rows.data() + (static_cast<size_t>(y) * img.w + x) * 3;
      d[0] = p.r;
      d[1] = p.g;
      d[2] = p.b;
    }
  }
  return PngEncode(img, PNG_COLOR_TYPE_RGB, 3, rows, PNG_INTERLACE_NONE);
}

std::vector<uint8_t> EncodePngGray(const Image& img) {
  std::vector<uint8_t> rows(static_cast<size_t>(img.w) * img.h);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      rows[static_cast<size_t>(y) * img.w + x] = Luma(img.at(x, y));
    }
  }
  return PngEncode(img, PNG_COLOR_TYPE_GRAY, 1, rows, PNG_INTERLACE_NONE);
}

std::vector<uint8_t> EncodePngGrayWithGama(const Image& img, double file_gamma) {
  std::vector<uint8_t> rows(static_cast<size_t>(img.w) * img.h);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      rows[static_cast<size_t>(y) * img.w + x] = Luma(img.at(x, y));
    }
  }
  std::vector<uint8_t> out;
  png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, nullptr, nullptr);
  if (png == nullptr) {
    return out;
  }
  png_infop info = png_create_info_struct(png);
  if (info == nullptr) {
    png_destroy_write_struct(&png, nullptr);
    return out;
  }
  if (setjmp(png_jmpbuf(png))) {
    png_destroy_write_struct(&png, &info);
    return {};
  }
  png_set_write_fn(png, &out, &PngWriteCb, nullptr);
  png_set_IHDR(
      png,
      info,
      static_cast<png_uint_32>(img.w),
      static_cast<png_uint_32>(img.h),
      8,
      PNG_COLOR_TYPE_GRAY,
      PNG_INTERLACE_NONE,
      PNG_COMPRESSION_TYPE_BASE,
      PNG_FILTER_TYPE_BASE);
  png_set_gAMA(png, info, file_gamma);
  png_write_info(png, info);
  for (int y = 0; y < img.h; ++y) {
    png_write_row(png, rows.data() + static_cast<size_t>(y) * img.w);
  }
  png_write_end(png, nullptr);
  png_destroy_write_struct(&png, &info);
  return out;
}

PngIhdr PngPeekIhdr(const std::vector<uint8_t>& b) {
  PngIhdr h;
  const uint8_t kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if (b.size() < 8 + 4 + 4 + 13 || !std::equal(kSig, kSig + 8, b.begin())) {
    return h;
  }
  // IHDR chunk: len(4) 'IHDR'(4) data(13).
  if (b[12] != 'I' || b[13] != 'H' || b[14] != 'D' || b[15] != 'R') {
    return h;
  }
  h.bit_depth = b[16 + 8];
  h.color_type = b[16 + 9];
  h.compression = b[16 + 10];
  h.filter = b[16 + 11];
  h.interlace = b[16 + 12];
  h.ok = true;
  return h;
}

Image DecodePng(const std::vector<uint8_t>& b) {
  Image img;
  png_image im;
  std::memset(&im, 0, sizeof im);
  im.version = PNG_IMAGE_VERSION;
  im.format = PNG_FORMAT_RGBA;
  if (!png_image_begin_read_from_memory(&im, b.data(), b.size())) {
    return img;
  }
  int w = static_cast<int>(im.width);
  int h = static_cast<int>(im.height);
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4);
  im.format = PNG_FORMAT_RGBA;
  if (!png_image_finish_read(&im, nullptr, buf.data(), 0, nullptr)) {
    return img;
  }
  img.w = w;
  img.h = h;
  img.px.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < img.px.size(); ++i) {
    img.px[i] = Rgba{buf[i * 4], buf[i * 4 + 1], buf[i * 4 + 2], buf[i * 4 + 3]};
  }
  return img;
}

// --- JPEG -----------------------------------------------------------------

namespace {

// The library's default error_exit prints and calls exit(EXIT_FAILURE), which would
// end the test binary instead of failing a check -- a save that produced no bytes
// would take every later test case down with it. Both JPEG paths install this
// manager, so a fatal library error unwinds to the caller and yields the same empty
// result the PNG helpers produce with setjmp(png_jmpbuf(png)). emit_message,
// output_message, format_message and reset_error_mgr keep their library defaults, so
// warning counting and trace output are unchanged.
struct JpegErrorMgr : jpeg_error_mgr {
  std::jmp_buf jump_buffer;
};

void JpegErrorExit(j_common_ptr cinfo) { std::longjmp(static_cast<JpegErrorMgr*>(cinfo->err)->jump_buffer, 1); }

// jpeg_std_error fills the manager with the library defaults, so the exit-ing
// error_exit is swapped afterwards.
jpeg_error_mgr* JpegErrorStart(JpegErrorMgr* jerr) {
  jpeg_std_error(jerr);
  jerr->error_exit = &JpegErrorExit;
  return jerr;
}

std::vector<uint8_t> JpegEncode(const std::vector<uint8_t>& plane, int w, int h, int channels, int quality) {
  std::vector<uint8_t> out;
  jpeg_compress_struct cinfo;
  JpegErrorMgr jerr;
  cinfo.err = JpegErrorStart(&jerr);
  jpeg_create_compress(&cinfo);
  if (setjmp(jerr.jump_buffer)) {
    jpeg_destroy_compress(&cinfo);
    return {};
  }
  unsigned char* mem = nullptr;
  unsigned long memsize = 0;
  jpeg_mem_dest(&cinfo, &mem, &memsize);
  cinfo.image_width = static_cast<JDIMENSION>(w);
  cinfo.image_height = static_cast<JDIMENSION>(h);
  cinfo.input_components = channels;
  cinfo.in_color_space = channels == 1 ? JCS_GRAYSCALE : JCS_EXT_RGB;
  jpeg_set_defaults(&cinfo);
  jpeg_set_quality(&cinfo, quality, TRUE);
  if (channels == 1) {
    jpeg_set_colorspace(&cinfo, JCS_GRAYSCALE);
  }
  jpeg_start_compress(&cinfo, TRUE);
  while (cinfo.next_scanline < cinfo.image_height) {
    JSAMPROW row = const_cast<JSAMPROW>(plane.data() + static_cast<size_t>(cinfo.next_scanline) * w * channels);
    jpeg_write_scanlines(&cinfo, &row, 1);
  }
  jpeg_finish_compress(&cinfo);
  jpeg_destroy_compress(&cinfo);
  out.assign(mem, mem + memsize);
  free(mem);
  return out;
}
}  // namespace

std::vector<uint8_t> EncodeJpeg(const Image& img, int quality) {
  std::vector<uint8_t> plane(static_cast<size_t>(img.w) * img.h * 3);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      Rgba p = img.at(x, y);
      uint8_t* d = plane.data() + (static_cast<size_t>(y) * img.w + x) * 3;
      d[0] = p.r;
      d[1] = p.g;
      d[2] = p.b;
    }
  }
  return JpegEncode(plane, img.w, img.h, 3, quality);
}

std::vector<uint8_t> EncodeJpegGray(const Image& img, int quality) {
  std::vector<uint8_t> plane(static_cast<size_t>(img.w) * img.h);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      plane[static_cast<size_t>(y) * img.w + x] = Luma(img.at(x, y));
    }
  }
  return JpegEncode(plane, img.w, img.h, 1, quality);
}

Image DecodeJpeg(const std::vector<uint8_t>& b) {
  Image img;
  jpeg_decompress_struct cinfo;
  JpegErrorMgr jerr;
  cinfo.err = JpegErrorStart(&jerr);
  jpeg_create_decompress(&cinfo);
  if (setjmp(jerr.jump_buffer)) {
    // An empty or corrupt buffer reaches JERR_INPUT_EMPTY/JERR_NO_SOI rather than
    // returning a status; both end up here as an empty image.
    jpeg_destroy_decompress(&cinfo);
    return {};
  }
  jpeg_mem_src(&cinfo, b.data(), b.size());
  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return img;
  }
  cinfo.out_color_space = JCS_EXT_RGB;
  if (!jpeg_start_decompress(&cinfo)) {
    jpeg_destroy_decompress(&cinfo);
    return img;
  }
  int w = static_cast<int>(cinfo.output_width);
  int h = static_cast<int>(cinfo.output_height);
  int nc = static_cast<int>(cinfo.output_components);
  std::vector<uint8_t> rowbuf(static_cast<size_t>(w) * nc);
  img.w = w;
  img.h = h;
  img.px.resize(static_cast<size_t>(w) * h);
  while (cinfo.output_scanline < cinfo.output_height) {
    int y = static_cast<int>(cinfo.output_scanline);
    JSAMPROW row = rowbuf.data();
    jpeg_read_scanlines(&cinfo, &row, 1);
    for (int x = 0; x < w; ++x) {
      uint8_t* s = rowbuf.data() + static_cast<size_t>(x) * nc;
      img.px[static_cast<size_t>(y) * w + x] = Rgba{s[0], s[1], s[2], 255};
    }
  }
  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return img;
}

// --- WebP -----------------------------------------------------------------

std::vector<uint8_t> EncodeWebp(const Image& img, int quality) {
  std::vector<uint8_t> plane(static_cast<size_t>(img.w) * img.h * 4);
  for (int y = 0; y < img.h; ++y) {
    for (int x = 0; x < img.w; ++x) {
      Rgba p = img.at(x, y);
      uint8_t* d = plane.data() + (static_cast<size_t>(y) * img.w + x) * 4;
      d[0] = p.r;
      d[1] = p.g;
      d[2] = p.b;
      d[3] = p.a;
    }
  }
  uint8_t* out = nullptr;
  size_t n = WebPEncodeRGBA(plane.data(), img.w, img.h, img.w * 4, quality, &out);
  if (out == nullptr || n == 0) {
    return {};
  }
  std::vector<uint8_t> bytes(out, out + n);
  WebPFree(out);
  return bytes;
}

Image DecodeWebp(const std::vector<uint8_t>& b) {
  Image img;
  int w = 0;
  int h = 0;
  if (!WebPGetInfo(b.data(), b.size(), &w, &h)) {
    return img;
  }
  uint8_t* rgba = WebPDecodeRGBA(b.data(), b.size(), &w, &h);
  if (rgba == nullptr) {
    return img;
  }
  img.w = w;
  img.h = h;
  img.px.resize(static_cast<size_t>(w) * h);
  for (size_t i = 0; i < img.px.size(); ++i) {
    img.px[i] = Rgba{rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2], rgba[i * 4 + 3]};
  }
  WebPFree(rgba);
  return img;
}

// --- BMP ------------------------------------------------------------------

namespace {
struct BmpBits {
  uint16_t bpp;
  int bytes_per_row;  // unpadded pixel bytes for a row
};

std::vector<uint8_t>
BmpFinish(uint16_t bpp, int w, int h, const std::vector<uint8_t>& palette, const std::vector<uint8_t>& pixels) {
  std::vector<uint8_t> v;
  uint32_t pixel_offset = 14 + 40 + static_cast<uint32_t>(palette.size());
  v.push_back('B');
  v.push_back('M');
  PutLE32(v, pixel_offset + static_cast<uint32_t>(pixels.size()));
  PutLE32(v, 0);
  PutLE32(v, pixel_offset);
  PutLE32(v, 40);
  PutLE32(v, static_cast<uint32_t>(w));
  PutLE32(v, static_cast<uint32_t>(h));  // positive => bottom-up
  v.push_back(1);
  v.push_back(0);  // planes = 1
  PutLE16(v, bpp);
  PutLE32(v, 0);  // BI_RGB
  PutLE32(v, static_cast<uint32_t>(pixels.size()));
  PutLE32(v, 0);
  PutLE32(v, 0);
  PutLE32(v, palette.empty() ? 0 : palette.size() / 4);
  PutLE32(v, 0);
  v.insert(v.end(), palette.begin(), palette.end());
  v.insert(v.end(), pixels.begin(), pixels.end());
  return v;
}
}  // namespace

std::vector<uint8_t> EncodeBmp24(const Image& img) {
  int row = ((img.w * 3 + 3) / 4) * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(row) * img.h, 0);
  for (int y = 0; y < img.h; ++y) {
    int by = img.h - 1 - y;  // bottom-up
    for (int x = 0; x < img.w; ++x) {
      Rgba p = img.at(x, y);
      uint8_t* d = pixels.data() + static_cast<size_t>(by) * row + x * 3;
      d[0] = p.b;
      d[1] = p.g;
      d[2] = p.r;
    }
  }
  return BmpFinish(24, img.w, img.h, {}, pixels);
}

std::vector<uint8_t> EncodeBmp32(const Image& img) {
  int row = img.w * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(row) * img.h);
  for (int y = 0; y < img.h; ++y) {
    int by = img.h - 1 - y;
    for (int x = 0; x < img.w; ++x) {
      Rgba p = img.at(x, y);
      uint8_t* d = pixels.data() + static_cast<size_t>(by) * row + x * 4;
      d[0] = p.b;
      d[1] = p.g;
      d[2] = p.r;
      d[3] = p.a;
    }
  }
  return BmpFinish(32, img.w, img.h, {}, pixels);
}

std::vector<uint8_t> EncodeBmp8(const Image& img) {
  std::vector<uint8_t> palette;
  for (int i = 0; i < 256; ++i) {
    palette.insert(palette.end(), {static_cast<uint8_t>(i), static_cast<uint8_t>(i), static_cast<uint8_t>(i), 0});
  }
  int row = ((img.w + 3) / 4) * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(row) * img.h, 0);
  for (int y = 0; y < img.h; ++y) {
    int by = img.h - 1 - y;
    for (int x = 0; x < img.w; ++x) {
      pixels[static_cast<size_t>(by) * row + x] = Luma(img.at(x, y));
    }
  }
  return BmpFinish(8, img.w, img.h, palette, pixels);
}

std::vector<uint8_t> EncodeBmp4(const Image& img) {
  std::vector<uint8_t> palette;
  for (int i = 0; i < 16; ++i) {
    palette.insert(
        palette.end(),
        {static_cast<uint8_t>(i * 17), static_cast<uint8_t>(i * 17), static_cast<uint8_t>(i * 17), 0});
  }
  int row = ((img.w + 7) / 8) * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(row) * img.h, 0);
  for (int y = 0; y < img.h; ++y) {
    int by = img.h - 1 - y;
    for (int x = 0; x < img.w; ++x) {
      uint8_t idx = static_cast<uint8_t>(Luma(img.at(x, y)) >> 4);
      uint8_t* byte = pixels.data() + static_cast<size_t>(by) * row + x / 2;
      if ((x & 1) == 0) {
        *byte = static_cast<uint8_t>((*byte & 0x0f) | (idx << 4));  // high nibble first
      } else {
        *byte = static_cast<uint8_t>((*byte & 0xf0) | idx);
      }
    }
  }
  return BmpFinish(4, img.w, img.h, palette, pixels);
}

std::vector<uint8_t> EncodeBmp1(const Image& img) {
  std::vector<uint8_t> palette = {0, 0, 0, 0, 255, 255, 255, 0};  // 0=black, 1=white
  int row = ((img.w + 31) / 32) * 4;
  std::vector<uint8_t> pixels(static_cast<size_t>(row) * img.h, 0);
  for (int y = 0; y < img.h; ++y) {
    int by = img.h - 1 - y;
    for (int x = 0; x < img.w; ++x) {
      uint8_t bit = Luma(img.at(x, y)) >= 128 ? 1 : 0;  // MSB-first
      if (bit) {
        pixels[static_cast<size_t>(by) * row + x / 8] =
            static_cast<uint8_t>(pixels[static_cast<size_t>(by) * row + x / 8] | (1 << (7 - (x % 8))));
      }
    }
  }
  return BmpFinish(1, img.w, img.h, palette, pixels);
}

// --- Lanczos3 reference (design §2.8) -------------------------------------

namespace {

double LanczosSinc(double x) {
  if (x == 0.0) {
    return 1.0;
  }
  double px = M_PI * x;
  return std::sin(px) / px;
}

double L3(double x) { return LanczosSinc(x) * LanczosSinc(x / 3.0); }

// Resample one channel along one axis (src length -> dst length).
std::vector<float> Axis(const std::vector<float>& src, int src_n, int dst_n) {
  std::vector<float> dst(static_cast<size_t>(dst_n));
  double d_scale = static_cast<double>(dst_n) / src_n;
  double d_width = d_scale < 1.0 ? 3.0 / d_scale : 3.0;
  double d_fscale = d_scale < 1.0 ? d_scale : 1.0;
  for (int u = 0; u < dst_n; ++u) {
    double d_center = u / d_scale + 0.5 / d_scale;
    int i_left = std::max(0, static_cast<int>(d_center - d_width + 0.5));
    int i_right = std::min(static_cast<int>(d_center + d_width + 0.5), src_n);
    double sum = 0.0;
    double norm = 0.0;
    for (int i = i_left; i < i_right; ++i) {
      double w = d_fscale * L3(d_fscale * (i + 0.5 - d_center));
      sum += w * src[i];
      norm += w;
    }
    if (norm > 0.0 && norm != 1.0) {
      sum /= norm;
    }
    dst[u] = static_cast<float>(sum);
  }
  return dst;
}

double ClampByte(double v) {
  if (v < 0.0) {
    return 0.0;
  }
  if (v > 255.0) {
    return 255.0;
  }
  return v;
}

}  // namespace

Image Lanczos3Reference(const Image& src, int dst_w, int dst_h) {
  auto channel = [&](int ch) {
    std::vector<float> sc(static_cast<size_t>(src.w) * src.h);
    for (int y = 0; y < src.h; ++y) {
      for (int x = 0; x < src.w; ++x) {
        Rgba p = src.at(x, y);
        sc[static_cast<size_t>(y) * src.w + x] = static_cast<float>(
            ch == 0   ? p.r
            : ch == 1 ? p.g
            : ch == 2 ? p.b
                      : p.a);
      }
    }
    // Horizontal pass: src.w -> dst_w, keep src.h rows.
    std::vector<float> h(static_cast<size_t>(dst_w) * src.h);
    for (int y = 0; y < src.h; ++y) {
      std::vector<float> row(
          sc.begin() + static_cast<size_t>(y) * src.w,
          sc.begin() + static_cast<size_t>(y + 1) * src.w);
      std::vector<float> o = Axis(row, src.w, dst_w);
      std::copy(o.begin(), o.end(), h.begin() + static_cast<size_t>(y) * dst_w);
    }
    // Vertical pass: src.h -> dst_h for each of dst_w columns.
    std::vector<float> out(static_cast<size_t>(dst_w) * dst_h);
    for (int x = 0; x < dst_w; ++x) {
      std::vector<float> col(static_cast<size_t>(src.h));
      for (int y = 0; y < src.h; ++y) {
        col[y] = h[static_cast<size_t>(y) * dst_w + x];
      }
      std::vector<float> o = Axis(col, src.h, dst_h);
      for (int y = 0; y < dst_h; ++y) {
        out[static_cast<size_t>(y) * dst_w + x] = o[y];
      }
    }
    return out;
  };
  std::vector<float> r = channel(0);
  std::vector<float> g = channel(1);
  std::vector<float> b = channel(2);
  std::vector<float> a = channel(3);
  Image out;
  out.w = dst_w;
  out.h = dst_h;
  out.px.resize(static_cast<size_t>(dst_w) * dst_h);
  for (size_t i = 0; i < out.px.size(); ++i) {
    out.px[i] = Rgba{
        static_cast<uint8_t>(std::lround(ClampByte(r[i]))),
        static_cast<uint8_t>(std::lround(ClampByte(g[i]))),
        static_cast<uint8_t>(std::lround(ClampByte(b[i]))),
        static_cast<uint8_t>(std::lround(ClampByte(a[i])))};
  }
  return out;
}

// --- GIF builder ----------------------------------------------------------

namespace {

// Minimal bit writer for the GIF LZW data stream (LSB-first).
struct BitWriter {
  std::vector<uint8_t> out;
  uint32_t buf = 0;
  int nbits = 0;
  void write(uint32_t code, int size) {
    buf |= code << nbits;
    nbits += size;
    while (nbits >= 8) {
      out.push_back(static_cast<uint8_t>(buf & 0xff));
      buf >>= 8;
      nbits -= 8;
    }
  }
  void flush() {
    if (nbits > 0) {
      out.push_back(static_cast<uint8_t>(buf & 0xff));
      buf = 0;
      nbits = 0;
    }
  }
};

// Emits a valid, dictionary-free ("uncompressed") GIF LZW stream at min code
// size 8: a clear code, then 9-bit literals with periodic clears, then EOI.
std::vector<uint8_t> GifLz(const std::vector<uint8_t>& idx) {
  BitWriter bw;
  bw.write(256, 9);  // clear
  int lit = 0;
  for (uint8_t p : idx) {
    bw.write(p, 9);
    if (++lit == 254) {
      bw.write(256, 9);  // reset before the decoder grows past 9-bit codes
      lit = 0;
    }
  }
  bw.write(257, 9);  // end of information
  bw.flush();
  return bw.out;
}

void EmitSubBlocks(std::vector<uint8_t>& v, const std::vector<uint8_t>& data) {
  size_t off = 0;
  while (off < data.size()) {
    size_t n = std::min<size_t>(255, data.size() - off);
    v.push_back(static_cast<uint8_t>(n));
    v.insert(v.end(), data.begin() + off, data.begin() + off + n);
    off += n;
  }
  v.push_back(0);  // block terminator
}

int CtSize(int entries) {
  int bits = 1;
  while ((1 << bits) < entries) {
    ++bits;
  }
  return bits - 1;  // table size == 2 << ctSize
}

}  // namespace

std::vector<uint8_t>
BuildGif(int logical_w, int logical_h, const GifPalette& global, int bg_index, const std::vector<GifFrame>& frames) {
  std::vector<uint8_t> v;
  const char* hdr = "GIF89a";
  v.insert(v.end(), hdr, hdr + 6);
  PutLE16(v, static_cast<uint16_t>(logical_w));
  PutLE16(v, static_cast<uint16_t>(logical_h));
  bool have_global = !global.entries.empty();
  int gct_size = 0;
  if (have_global) {
    gct_size = CtSize(static_cast<int>(global.entries.size()));
    v.push_back(static_cast<uint8_t>(0x80 | (gct_size & 0x07)));  // GCT flag, color res 7, not sorted
  } else {
    v.push_back(0);
  }
  v.push_back(static_cast<uint8_t>(bg_index));
  v.push_back(0);  // pixel aspect ratio
  if (have_global) {
    int table_size = 2 << gct_size;
    for (int i = 0; i < table_size; ++i) {
      Rgba c = i < static_cast<int>(global.entries.size()) ? global.entries[i] : Rgba{};
      v.push_back(c.r);
      v.push_back(c.g);
      v.push_back(c.b);
    }
  }
  for (const GifFrame& f : frames) {
    // Graphic Control Extension.
    v.push_back(0x21);
    v.push_back(0xF9);
    v.push_back(0x04);
    uint8_t packed = static_cast<uint8_t>((f.disposal << 2) & 0x1C);
    if (f.transparent) {
      packed |= 0x01;
    }
    v.push_back(packed);
    PutLE16(v, static_cast<uint16_t>(f.delay_cs));
    v.push_back(static_cast<uint8_t>(f.transparent ? f.trans_index : 0));
    v.push_back(0);  // GCE block terminator
    // Image Descriptor.
    v.push_back(0x2C);
    PutLE16(v, static_cast<uint16_t>(f.left));
    PutLE16(v, static_cast<uint16_t>(f.top));
    PutLE16(v, static_cast<uint16_t>(f.w));
    PutLE16(v, static_cast<uint16_t>(f.h));
    bool have_local = !f.local.empty();
    if (have_local) {
      int lct = CtSize(static_cast<int>(f.local.size()));
      v.push_back(static_cast<uint8_t>(0x80 | (lct & 0x07)));
      int table_size = 2 << lct;
      for (int i = 0; i < table_size; ++i) {
        Rgba c = i < static_cast<int>(f.local.size()) ? f.local[i] : Rgba{};
        v.push_back(c.r);
        v.push_back(c.g);
        v.push_back(c.b);
      }
    } else {
      v.push_back(0);
    }
    // Image data: min code size 8, then the LZW sub-blocks.
    v.push_back(8);
    EmitSubBlocks(v, GifLz(f.idx));
  }
  v.push_back(0x3B);  // trailer
  return v;
}

GifProbe GifProbeBytes(const std::vector<uint8_t>& bytes) {
  GifProbe p;
  int err = 0;
  struct Buf {
    const unsigned char* p;
    size_t n;
    size_t off;
  };
  auto* buf = new Buf{bytes.data(), bytes.size(), 0};
  GifFileType* gif = DGifOpen(
      buf,
      [](GifFileType* g, GifByteType* d, int n) -> int {
        auto* b = static_cast<Buf*>(g->UserData);
        int got = 0;
        while (got < n && b->off < b->n) {
          d[got++] = b->p[b->off++];
        }
        return got;
      },
      &err);
  if (gif == nullptr) {
    delete buf;
    return p;
  }
  if (DGifSlurp(gif) != GIF_OK) {
    DGifCloseFile(gif, &err);
    return p;
  }
  for (int i = 0; i < gif->ImageCount; ++i) {
    const SavedImage& si = gif->SavedImages[i];
    p.page_count++;
    p.frame_left.push_back(si.ImageDesc.Left);
    p.frame_top.push_back(si.ImageDesc.Top);
    p.frame_w.push_back(si.ImageDesc.Width);
    p.frame_h.push_back(si.ImageDesc.Height);
    int disposal = 0;
    int delay = 0;
    int trans = -1;
    for (int e = 0; e < si.ExtensionBlockCount; ++e) {
      const ExtensionBlock& eb = si.ExtensionBlocks[e];
      if (eb.Function == GRAPHICS_EXT_FUNC_CODE && eb.ByteCount >= 4) {
        GraphicsControlBlock gcb;
        if (DGifExtensionToGCB(eb.ByteCount, eb.Bytes, &gcb) == GIF_OK) {
          disposal = gcb.DisposalMode;
          delay = gcb.DelayTime;
          trans = gcb.TransparentColor;  // NO_TRANSPARENT_COLOR when absent
        }
      }
    }
    p.disposal.push_back(disposal);
    p.delay_cs.push_back(delay);
    p.trans_index.push_back(trans == NO_TRANSPARENT_COLOR ? -1 : trans);
  }
  DGifCloseFile(gif, &err);
  return p;
}

}  // namespace fic
