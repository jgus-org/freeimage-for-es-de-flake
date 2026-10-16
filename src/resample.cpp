// Resampling (CORE-owned): the separable filter pass behind FreeImage_Rescale,
// written to the contract's sampling spec (design §2.8) - per destination pixel a
// renormalized window of the filter kernel, horizontal and vertical passes in
// the order classic's heuristic picks, and the destination bit-depth rules
// (24 -> 24, 32 -> 32 with alpha as an independent fourth channel, palette ->
// 24, or 32 when transparent, greyscale -> 8, or 32 when transparent, 16-bit ->
// 24). A same-size request is a copy plus at most a bit-depth conversion, never
// a filter pass: the kernel is not a delta at unit scale.
//
// Values are carried between the two passes in float and rounded once at the
// end. Classic round-trips the first pass through an 8-bit intermediate image,
// so the two differ by well under the contract's 2/255 conformance band.
// There is no mutable global state here: every table and plane is call-local
// (§2.11).
#include "resample.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "convert.h"
#include "dib.h"

namespace fi {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288;

// The classic filter library (FreeImageToolkit/Filters.h) as a value type: a
// support width plus an impulse response.
struct Filter {
  enum class Kind {
    kBox,
    kTriangle,
    kCubic,
    kLanczos3,
  };
  Kind kind;
  double width;
  double b;  // cubic parameters, Kind::kCubic only
  double c;

  double value(double x) const {
    switch (kind) {
      case Kind::kBox:
        return std::fabs(x) <= width ? 1.0 : 0.0;
      case Kind::kTriangle:
        x = std::fabs(x);
        return x < width ? width - x : 0.0;
      case Kind::kLanczos3: {
        x = std::fabs(x);
        if (x >= width) {
          return 0.0;
        }
        auto const sinc = [](double v) {
          if (v == 0.0) {
            return 1.0;
          }
          double const pv = kPi * v;
          return std::sin(pv) / pv;
        };
        return sinc(x) * sinc(x / width);
      }
      case Kind::kCubic: {
        x = std::fabs(x);
        double const p0 = (6 - 2 * b) / 6;
        double const p2 = (-18 + 12 * b + 6 * c) / 6;
        double const p3 = (12 - 9 * b - 6 * c) / 6;
        double const q0 = (8 * b + 24 * c) / 6;
        double const q1 = (-12 * b - 48 * c) / 6;
        double const q2 = (6 * b + 30 * c) / 6;
        double const q3 = (-b - 6 * c) / 6;
        if (x < 1.0) {
          return p0 + x * x * (p2 + x * p3);
        }
        if (x < 2.0) {
          return q0 + x * (q1 + x * (q2 + x * q3));
        }
        return 0.0;
      }
    }
    return 0.0;
  }
};

constexpr Filter filter_for(FREE_IMAGE_FILTER filter) {
  switch (filter) {
    case FILTER_BOX:
      return Filter{Filter::Kind::kBox, 0.5, 0, 0};
    case FILTER_BILINEAR:
      return Filter{Filter::Kind::kTriangle, 1.0, 0, 0};
    case FILTER_BICUBIC:  // Mitchell & Netravali, b = c = 1/3
      return Filter{Filter::Kind::kCubic, 2.0, 1.0 / 3.0, 1.0 / 3.0};
    case FILTER_BSPLINE:  // 4th order b-spline: b = 1, c = 0
      return Filter{Filter::Kind::kCubic, 2.0, 1.0, 0.0};
    case FILTER_CATMULLROM:  // b = 0, c = 1/2
      return Filter{Filter::Kind::kCubic, 2.0, 0.0, 0.5};
    case FILTER_LANCZOS3:
      return Filter{Filter::Kind::kLanczos3, 3.0, 0, 0};
  }
  return Filter{Filter::Kind::kBox, 0.5, 0, 0};
}

bool known_filter(FREE_IMAGE_FILTER filter) {
  switch (filter) {
    case FILTER_BOX:
    case FILTER_BICUBIC:
    case FILTER_BILINEAR:
    case FILTER_BSPLINE:
    case FILTER_CATMULLROM:
    case FILTER_LANCZOS3:
      return true;
    default:
      return false;
  }
}

// One destination pixel's contribution window over the source samples [left, right).
struct Window {
  int left = 0;
  int right = 0;
  std::vector<double> weights;
};

// classic CWeightsTable (Resize.cpp:141-208): dScale = dst/src, a window of
// +/-3 support scaled up for minification, the kernel sampled at the half-pixel
// offsets, and the window renormalized when its weights do not sum to 1. Edge
// windows truncate to the source and renormalize, which extends the edge pixels
// implicitly.
std::vector<Window> build_windows(int dst_n, int src_n, Filter const& filter) {
  std::vector<Window> table(static_cast<std::size_t>(dst_n));
  double const dScale = static_cast<double>(dst_n) / static_cast<double>(src_n);
  double const dWidth = dScale < 1.0 ? filter.width / dScale : filter.width;
  double const dFScale = dScale < 1.0 ? dScale : 1.0;
  double const dOffset = 0.5 / dScale;
  for (int u = 0; u < dst_n; ++u) {
    double const dCenter = static_cast<double>(u) / dScale + dOffset;
    Window window;
    window.left = std::max(0, static_cast<int>(dCenter - dWidth + 0.5));
    window.right = std::min(static_cast<int>(dCenter + dWidth + 0.5), src_n);
    if (window.right < window.left) {
      window.right = window.left;
    }
    window.weights.resize(static_cast<std::size_t>(window.right - window.left));
    double total = 0.0;
    for (int i = window.left; i < window.right; ++i) {
      double const weight = dFScale * filter.value(dFScale * (static_cast<double>(i) + 0.5 - dCenter));
      window.weights[static_cast<std::size_t>(i - window.left)] = weight;
      total += weight;
    }
    if (total > 0.0 && total != 1.0) {
      for (double& weight : window.weights) {
        weight /= total;
      }
    }
    table[static_cast<std::size_t>(u)] = std::move(window);
  }
  return table;
}

// One channel of a single axis's worth of samples.
struct Plane {
  Plane(int width, int height)
      : width(width),
        height(height),
        pixels(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0.0f) {}

  int width;
  int height;
  std::vector<float> pixels;

  float at(int x, int y) const {
    return pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x)];
  }
  float* row(int y) { return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width); }
  float const* row(int y) const {
    return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
  }
};

Plane filter_horizontal(Plane const& src, std::vector<Window> const& windows, int dst_width) {
  Plane dst(dst_width, src.height);
  for (int y = 0; y < src.height; ++y) {
    float const* source = src.row(y);
    float* target = dst.row(y);
    for (int u = 0; u < dst_width; ++u) {
      Window const& window = windows[static_cast<std::size_t>(u)];
      double sum = 0.0;
      for (int i = window.left; i < window.right; ++i) {
        sum += window.weights[static_cast<std::size_t>(i - window.left)] * static_cast<double>(source[i]);
      }
      target[u] = static_cast<float>(sum);
    }
  }
  return dst;
}

Plane filter_vertical(Plane const& src, std::vector<Window> const& windows, int dst_height) {
  Plane dst(src.width, dst_height);
  for (int x = 0; x < src.width; ++x) {
    for (int v = 0; v < dst_height; ++v) {
      Window const& window = windows[static_cast<std::size_t>(v)];
      double sum = 0.0;
      for (int i = window.left; i < window.right; ++i) {
        sum += window.weights[static_cast<std::size_t>(i - window.left)] * static_cast<double>(src.at(x, i));
      }
      dst.row(v)[x] = static_cast<float>(sum);
    }
  }
  return dst;
}

// Source pixel -> float channels in B, G, R, A order, expanding palette entries, the
// two 16-bit layouts and the transparency table.
std::array<float, 4> sample(Dib const& src, int x, int y) {
  std::span<BYTE const> const bits = src.row(y);
  if (src.bpp() <= 8) {
    unsigned const index = palette_index(bits, static_cast<unsigned>(x), src.bpp());
    // An index the codec never filled reads as black, not as the last palette entry.
    RGBQUAD const color = index < src.palette().size() ? src.palette()[index] : RGBQUAD{};
    return {
        static_cast<float>(color.rgbBlue),
        static_cast<float>(color.rgbGreen),
        static_cast<float>(color.rgbRed),
        src.is_transparent() ? static_cast<float>(src.transparency()[index]) : 255.0f};
  }
  if (src.bpp() == 16) {
    Channels const rgb = expand16(load16(bits.data() + 2 * x), src.is_rgb565());
    return {static_cast<float>(rgb.blue), static_cast<float>(rgb.green), static_cast<float>(rgb.red), 255.0f};
  }
  BYTE const* pixel = bits.data() + static_cast<std::size_t>(src.bpp() / 8) * static_cast<std::size_t>(x);
  return {
      static_cast<float>(pixel[FI_RGBA_BLUE]),
      static_cast<float>(pixel[FI_RGBA_GREEN]),
      static_cast<float>(pixel[FI_RGBA_RED]),
      src.bpp() == 32 ? static_cast<float>(pixel[FI_RGBA_ALPHA]) : 255.0f};
}

inline BYTE to_byte(float value) {
  double const clamped = value < 0.0f ? 0.0 : (value > 255.0f ? 255.0 : static_cast<double>(value));
  return static_cast<BYTE>(std::lround(clamped));
}

}  // namespace

std::unique_ptr<Dib> rescale(Dib const& src, int dst_width, int dst_height, FREE_IMAGE_FILTER filter) {
  if (dst_width <= 0 || dst_height <= 0 || !known_filter(filter)) {
    return nullptr;
  }

  std::optional<Ramp> const ramp = src.greyscale_ramp();
  int target_bpp;
  if (src.bpp() <= 8) {
    target_bpp = src.is_transparent() ? 32 : (ramp.has_value() ? 8 : 24);
  } else if (src.bpp() == 16) {
    target_bpp = 24;
  } else {
    target_bpp = src.bpp();  // 24 -> 24, 32 -> 32
  }

  // Same-size request: a copy, converted only when the depth changes (§2.8).
  // Both paths carry the metadata along, as classic's Rescale does.
  if (src.width() == dst_width && src.height() == dst_height) {
    if (src.bpp() == target_bpp) {
      return std::make_unique<Dib>(src);
    }
    return convert::to_dib(src, target_bpp == 32 ? 32 : 24);
  }

  int const channels = target_bpp == 32 ? 4 : (target_bpp == 24 ? 3 : 1);

  std::unique_ptr<Dib> owned = Dib::make(dst_width, dst_height, target_bpp, 0, 0, 0);
  if (owned == nullptr) {
    return nullptr;
  }
  Dib& dst = *owned;
  dst.adopt_tags_from(src);

  Filter const kernel = filter_for(filter);
  std::vector<Window> const horizontal = build_windows(dst_width, src.width(), kernel);
  std::vector<Window> const vertical = build_windows(dst_height, src.height(), kernel);
  // classic's "xy versus yx" choice: filter horizontally first when the width does not
  // grow, which keeps the more expensive vertical convolutions fewer.
  bool const horizontal_first = dst_width <= src.width();

  std::vector<Plane> planes;
  planes.reserve(static_cast<std::size_t>(channels));
  for (int channel = 0; channel < channels; ++channel) {
    Plane source_plane(src.width(), src.height());
    for (int y = 0; y < src.height(); ++y) {
      float* target = source_plane.row(y);
      for (int x = 0; x < src.width(); ++x) {
        target[x] = sample(src, x, y)[channel];
      }
    }
    // A greyscale destination keeps channel 0: every channel of a greyscale palette
    // expansion holds the same grey value.
    planes.push_back(
        horizontal_first
            ? filter_vertical(filter_horizontal(source_plane, horizontal, dst_width), vertical, dst_height)
            : filter_horizontal(filter_vertical(source_plane, vertical, dst_height), horizontal, dst_width));
  }

  for (int y = 0; y < dst_height; ++y) {
    std::span<BYTE> const target = dst.row(y);
    for (int x = 0; x < dst_width; ++x) {
      switch (target_bpp) {
        case 8:
          target[x] = to_byte(planes[0].at(x, y));
          break;
        case 24:
          target[x * 3 + FI_RGBA_BLUE] = to_byte(planes[0].at(x, y));
          target[x * 3 + FI_RGBA_GREEN] = to_byte(planes[1].at(x, y));
          target[x * 3 + FI_RGBA_RED] = to_byte(planes[2].at(x, y));
          break;
        default:
          target[x * 4 + FI_RGBA_BLUE] = to_byte(planes[0].at(x, y));
          target[x * 4 + FI_RGBA_GREEN] = to_byte(planes[1].at(x, y));
          target[x * 4 + FI_RGBA_RED] = to_byte(planes[2].at(x, y));
          target[x * 4 + FI_RGBA_ALPHA] = to_byte(planes[3].at(x, y));
          break;
      }
    }
  }

  // An 8-bit destination is greyscale, so it gets a greyscale palette - reversed for a
  // min-is-white source (§2.8).
  if (target_bpp == 8) {
    bool const palette_inverted = ramp == Ramp::Inverted;
    for (unsigned i = 0; i < dst.palette().size(); ++i) {
      BYTE const grey = palette_inverted ? static_cast<BYTE>(255 - i) : static_cast<BYTE>(i);
      dst.palette()[i] = RGBQUAD{grey, grey, grey, 0};
    }
  }
  return owned;
}

}  // namespace fi
