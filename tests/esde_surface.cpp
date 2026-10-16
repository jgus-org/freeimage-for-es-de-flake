// Faithful replica of every FreeImage-using code region in ES-DE's four TUs
// (ImageIO.cpp, GIFAnimComponent.{h,cpp}, MiximageGenerator.cpp,
// Scraper.cpp) at rev 5db4e2a. Surroundings are stubbed; the FreeImage call
// expressions are reproduced verbatim (POSIX branch only). Compiled and linked
// against the library as the `esde-surface` flake check, so any signature or
// symbol mismatch with the shipped header fails the build.
#include <FreeImage.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// --- ImageIO.cpp::loadFromMemoryRGBA32 -------------------------------------

void esde_imageio_region(const unsigned char* data, const size_t size, size_t& width, size_t& height) {
  std::vector<unsigned char> rawData;
  width = 0;
  height = 0;
  FIMEMORY* fiMemory{FreeImage_OpenMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size))};

  if (fiMemory != nullptr) {
    FREE_IMAGE_FORMAT format{FreeImage_GetFileTypeFromMemory(fiMemory)};
    if (format != FIF_UNKNOWN && FreeImage_FIFSupportsReading(format)) {
      FIBITMAP* fiBitmap{FreeImage_LoadFromMemory(format, fiMemory)};
      if (fiBitmap != nullptr) {
        if (FreeImage_GetBPP(fiBitmap) != 32) {
          FIBITMAP* fiConverted{FreeImage_ConvertTo32Bits(fiBitmap)};
          if (fiConverted != nullptr) {
            FreeImage_Unload(fiBitmap);
            fiBitmap = fiConverted;
          }
        }
        FreeImage_PreMultiplyWithAlpha(fiBitmap);

        if (fiBitmap != nullptr) {
          width = FreeImage_GetWidth(fiBitmap);
          height = FreeImage_GetHeight(fiBitmap);
          unsigned char* tempData{new unsigned char[width * height * 4]};
          for (size_t i = 0; i < height; ++i) {
            const BYTE* scanLine{FreeImage_GetScanLine(fiBitmap, static_cast<int>(i))};
            memcpy(tempData + (i * width * 4), scanLine, width * 4);
          }
          rawData = std::vector<unsigned char>{tempData, tempData + width * height * 4};
          FreeImage_Unload(fiBitmap);
          delete[] tempData;
        }
      }
    }
    FreeImage_CloseMemory(fiMemory);
  }
  (void) rawData;
}

// --- GIFAnimComponent.{h,cpp} ----------------------------------------------
// The component's FreeImageIO callbacks (from GIFAnimComponent.h) verbatim.

static unsigned int readProc(void* buffer, unsigned int size, unsigned int count, fi_handle handle) {
  return static_cast<unsigned int>(fread(buffer, size, count, reinterpret_cast<FILE*>(handle)));
}

static unsigned int writeProc(void* buffer, unsigned int size, unsigned int count, fi_handle handle) {
  return static_cast<unsigned int>(fwrite(buffer, size, count, reinterpret_cast<FILE*>(handle)));
}

static int seekProc(fi_handle handle, long offset, int origin) {
  return fseek(reinterpret_cast<FILE*>(handle), offset, origin);
}

static long int tellProc(fi_handle handle) { return ftell(reinterpret_cast<FILE*>(handle)); }

struct GifAnimSurface {
  FILE* mAnimFile{nullptr};
  FreeImageIO mAnimIO;
  FIMULTIBITMAP* mAnimation{nullptr};
  FIBITMAP* mFrame{nullptr};
  std::vector<uint8_t> mPictureRGBA;
  size_t mFrameSize{0};
  int mTotalFrames{0};
  int mFrameNum{0};
  int mFrameTime{0};
  unsigned int mFileWidth{0};
  unsigned int mFileHeight{0};

  void bindIo() {
    mAnimIO.read_proc = readProc;
    mAnimIO.write_proc = writeProc;
    mAnimIO.seek_proc = seekProc;
    mAnimIO.tell_proc = tellProc;
  }

  // GIFAnimComponent::setAnimation() (POSIX branch).
  void setAnimation(const std::string& path) {
    if (mAnimation != nullptr) {
      FreeImage_CloseMultiBitmap(mAnimation, 0);
      mAnimation = nullptr;
      mPictureRGBA.clear();
    }

    FREE_IMAGE_FORMAT fileFormat = FreeImage_GetFileType(path.c_str());
    if (fileFormat == FIF_UNKNOWN)
      fileFormat = FreeImage_GetFIFFromFilename(path.c_str());
    if (fileFormat != FIF_GIF)
      return;

    if (FreeImage_FIFSupportsReading(fileFormat)) {
      if (mAnimFile != nullptr)
        mAnimation =
            FreeImage_OpenMultiBitmapFromHandle(fileFormat, &mAnimIO, static_cast<fi_handle>(mAnimFile), GIF_PLAYBACK);

      mFrame = FreeImage_LockPage(mAnimation, 0);
      FITAG* tagFrameTime{nullptr};

      FreeImage_GetMetadata(FIMD_ANIMATION, mFrame, "FrameTime", &tagFrameTime);
      if (tagFrameTime != nullptr) {
        if (FreeImage_GetTagCount(tagFrameTime) == 1) {
          const uint32_t frameTime{*static_cast<const uint32_t*>(FreeImage_GetTagValue(tagFrameTime))};
          if (frameTime >= 20 && frameTime <= 1000)
            mFrameTime = frameTime;
        }
      }
    }

    if (!mAnimation)
      return;

    unsigned int filePitch{0};
    mTotalFrames = static_cast<size_t>(FreeImage_GetPageCount(mAnimation));
    mFileWidth = FreeImage_GetWidth(mFrame);
    mFileHeight = FreeImage_GetHeight(mFrame);
    filePitch = FreeImage_GetPitch(mFrame);

    FreeImage_PreMultiplyWithAlpha(mFrame);
    mPictureRGBA.resize(mFileWidth * mFileHeight * 4);

    FreeImage_ConvertToRawBits(
        reinterpret_cast<BYTE*>(&mPictureRGBA.at(0)),
        mFrame,
        filePitch,
        32,
        FI_RGBA_BLUE,
        FI_RGBA_GREEN,
        FI_RGBA_RED,
        1);

    FreeImage_UnlockPage(mAnimation, mFrame, false);
    FreeImage_CloseMultiBitmap(mAnimation, 0);

    mFrameSize = mFileWidth * mFileHeight * 4;
  }

  // GIFAnimComponent::render() frame path (POSIX branch).
  void renderFrame() {
    mAnimation =
        FreeImage_OpenMultiBitmapFromHandle(FIF_GIF, &mAnimIO, static_cast<fi_handle>(mAnimFile), GIF_PLAYBACK);

    mFrame = FreeImage_LockPage(mAnimation, mFrameNum);
    FreeImage_PreMultiplyWithAlpha(mFrame);
    mPictureRGBA.clear();
    mPictureRGBA.resize(mFrameSize);

    FreeImage_ConvertToRawBits(
        reinterpret_cast<BYTE*>(&mPictureRGBA.at(0)),
        mFrame,
        FreeImage_GetPitch(mFrame),
        32,
        FI_RGBA_RED,
        FI_RGBA_GREEN,
        FI_RGBA_BLUE,
        1);

    FreeImage_UnlockPage(mAnimation, mFrame, false);
    FreeImage_CloseMultiBitmap(mAnimation, 0);
  }
};

// --- MiximageGenerator.cpp (screenshot load + raw export + save) -----------

struct FakeCanvas {
  unsigned int w{0};
  unsigned int h{0};
  unsigned int width() const { return w; }
  unsigned int height() const { return h; }
};

void esde_miximage_region(
    const std::string& screenshotPath,
    const std::string& savePath,
    const std::string& fileFormatSetting) {
  FREE_IMAGE_FORMAT fileFormat = FreeImage_GetFileType(screenshotPath.c_str());
  if (fileFormat == FIF_UNKNOWN)
    fileFormat = FreeImage_GetFIFFromFilename(screenshotPath.c_str());
  if (fileFormat == FIF_UNKNOWN)
    return;

  FIBITMAP* screenshotFile{nullptr};
  if (FreeImage_FIFSupportsReading(fileFormat))
    screenshotFile = FreeImage_Load(fileFormat, screenshotPath.c_str());
  if (!screenshotFile)
    return;

  if (FreeImage_GetBPP(screenshotFile) != 32) {
    FIBITMAP* screenshotTemp{FreeImage_ConvertTo32Bits(screenshotFile)};
    FreeImage_Unload(screenshotFile);
    screenshotFile = screenshotTemp;
  }

  unsigned int fileWidth = FreeImage_GetWidth(screenshotFile);
  unsigned int fileHeight = FreeImage_GetHeight(screenshotFile);
  unsigned int filePitch = FreeImage_GetPitch(screenshotFile);

  std::vector<unsigned char> screenshotVector(fileWidth * fileHeight * 4);
  FreeImage_ConvertToRawBits(
      reinterpret_cast<BYTE*>(&screenshotVector.at(0)),
      screenshotFile,
      filePitch,
      32,
      FI_RGBA_BLUE,
      FI_RGBA_GREEN,
      FI_RGBA_RED,
      1);
  FreeImage_Unload(screenshotFile);

  FakeCanvas canvasImage{1280, 960};
  std::vector<unsigned char> canvasVector(static_cast<size_t>(canvasImage.width()) * canvasImage.height() * 4, 0);

  FIBITMAP* mixImage{nullptr};
  mixImage = FreeImage_ConvertFromRawBits(
      &canvasVector.at(0),
      canvasImage.width(),
      canvasImage.height(),
      canvasImage.width() * 4,
      32,
      FI_RGBA_BLUE,
      FI_RGBA_GREEN,
      FI_RGBA_RED);

  FREE_IMAGE_FORMAT format{FIF_UNKNOWN};
  int flags{0};
  if (fileFormatSetting == "webp") {
    format = FIF_WEBP;
    flags = 85;  // quality 85 instead of the default 75
  } else {
    format = FIF_PNG;
  }

  bool savedImage{FreeImage_Save(format, mixImage, savePath.c_str(), flags) != 0};
  FreeImage_Unload(mixImage);
  (void) savedImage;
}

// --- Scraper.cpp (empty-image check + resizeImage) -------------------------

void esde_scraper_empty_check_region(std::string& imageData) {
  bool emptyImage{false};
  FREE_IMAGE_FORMAT imageFormat{FIF_UNKNOWN};
  FIMEMORY* memoryStream{
      FreeImage_OpenMemory(reinterpret_cast<BYTE*>(&imageData.at(0)), static_cast<DWORD>(imageData.size()))};
  imageFormat = FreeImage_GetFileTypeFromMemory(memoryStream, 0);

  if (imageFormat != FIF_UNKNOWN) {
    emptyImage = true;
    FIBITMAP* tempImage{FreeImage_LoadFromMemory(imageFormat, memoryStream)};
    RGBQUAD firstPixel;
    RGBQUAD currPixel;

    unsigned int width{FreeImage_GetWidth(tempImage)};
    unsigned int height{FreeImage_GetHeight(tempImage)};

    if (width < 50) {
      emptyImage = true;
    } else if (height < 50) {
      emptyImage = true;
    } else {
      if (FreeImage_GetBPP(tempImage) != 24) {
        FIBITMAP* convertImage{FreeImage_ConvertTo24Bits(tempImage)};
        FreeImage_Unload(tempImage);
        tempImage = convertImage;
      }

      FreeImage_GetPixelColor(tempImage, 0, 1, &firstPixel);

      for (unsigned int x{0}; x < width; ++x) {
        if (!emptyImage)
          break;
        for (unsigned int y{1}; y < height - 1; ++y) {
          FreeImage_GetPixelColor(tempImage, x, y, &currPixel);
          if (currPixel.rgbBlue != firstPixel.rgbBlue || currPixel.rgbGreen != firstPixel.rgbGreen ||
              currPixel.rgbRed != firstPixel.rgbRed) {
            emptyImage = false;
            break;
          }
        }
      }
    }
    FreeImage_Unload(tempImage);
  }
  FreeImage_CloseMemory(memoryStream);
  (void) emptyImage;
}

bool esde_scraper_resize_region(const std::string& path) {
  float maxWidth{2560.0f};
  float maxHeight{1440.0f};

  FREE_IMAGE_FORMAT format{FIF_UNKNOWN};
  FIBITMAP* image{nullptr};

  format = FreeImage_GetFileType(path.c_str(), 0);
  if (format == FIF_UNKNOWN)
    format = FreeImage_GetFIFFromFilename(path.c_str());
  if (format == FIF_UNKNOWN) {
    return false;
  }

  if (FreeImage_FIFSupportsReading(format)) {
    image = FreeImage_Load(format, path.c_str());
  } else {
    return false;
  }

  float width{static_cast<float>(FreeImage_GetWidth(image))};
  float height{static_cast<float>(FreeImage_GetHeight(image))};

  if (maxWidth >= width && maxHeight >= height) {
    FreeImage_Unload(image);
    return true;
  }

  float scaleFactor{0.0f};
  if (width > maxWidth) {
    scaleFactor = maxWidth / width;
    if (height * scaleFactor > maxHeight)
      scaleFactor = maxHeight / height;
  } else {
    scaleFactor = maxHeight / height;
  }

  maxWidth = floorf(width * scaleFactor);
  maxHeight = floorf(height * scaleFactor);

  FIBITMAP* imageRescaled{
      FreeImage_Rescale(image, static_cast<int>(maxWidth), static_cast<int>(maxHeight), FILTER_LANCZOS3)};
  FreeImage_Unload(image);

  if (imageRescaled == nullptr) {
    return false;
  }

  bool saved{FreeImage_Save(format, imageRescaled, path.c_str()) != 0};
  FreeImage_Unload(imageRescaled);
  return saved;
}

// Force emission and linking of every region (main does not run them, so the
// stub library's runtime behavior is irrelevant; only compile+link is checked).
int main() {
  volatile void* const free_regions[] = {
      reinterpret_cast<void*>(&esde_imageio_region),
      reinterpret_cast<void*>(&esde_miximage_region),
      reinterpret_cast<void*>(&esde_scraper_empty_check_region),
      reinterpret_cast<void*>(&esde_scraper_resize_region),
  };
  volatile auto bind_io = &GifAnimSurface::bindIo;
  volatile auto set_animation = &GifAnimSurface::setAnimation;
  volatile auto render_frame = &GifAnimSurface::renderFrame;
  (void) free_regions;
  (void) bind_io;
  (void) set_animation;
  (void) render_frame;
  return 0;
}
