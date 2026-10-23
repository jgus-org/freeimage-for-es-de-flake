// Runtime smoke test plus compile-time conformance of the classic constant
// surface (values are checked here; full per-function behavior is the TDD suite).
#include <FreeImage.h>

#include <cstring>

// --- classic 3.18.0 constant conformance -----------------------------------
static_assert(FREEIMAGE_MAJOR_VERSION == 3 && FREEIMAGE_MINOR_VERSION == 18 && FREEIMAGE_RELEASE_SERIAL == 0);
static_assert(FREEIMAGE_COLORORDER == FREEIMAGE_COLORORDER_BGR);

static_assert(FI_RGBA_RED == 2 && FI_RGBA_GREEN == 1 && FI_RGBA_BLUE == 0 && FI_RGBA_ALPHA == 3);
static_assert(
    FI_RGBA_RED_MASK == 0x00FF0000 && FI_RGBA_GREEN_MASK == 0x0000FF00 && FI_RGBA_BLUE_MASK == 0x000000FF &&
    FI_RGBA_ALPHA_MASK == 0xFF000000);

static_assert(FIF_UNKNOWN == -1);
static_assert(FIF_BMP == 0 && FIF_JPEG == 2 && FIF_PNG == 13 && FIF_GIF == 25 && FIF_WEBP == 35);

static_assert(GIF_PLAYBACK == 2 && GIF_LOAD256 == 1 && GIF_DEFAULT == 0);
static_assert(FILTER_LANCZOS3 == 5 && FILTER_CATMULLROM == 4);
static_assert(FIMD_ANIMATION == 9 && FIDT_LONG == 4);

// --- struct layout ---------------------------------------------------------
static_assert(sizeof(RGBQUAD) == 4);
static_assert(sizeof(FreeImageIO) == 4 * sizeof(void*));

int main() {
  FreeImage_Initialise();
  FreeImage_Initialise(TRUE);

  if (std::strcmp(FreeImage_GetVersion(), "3.18.0") != 0) {
    return 1;
  }
  if (FreeImage_GetCopyrightMessage() == nullptr) {
    return 2;
  }

  // Detection of an absent file and a null stream are the classic failure path.
  if (FreeImage_GetFileType("/nonexistent-image.png") != FIF_UNKNOWN) {
    return 3;
  }
  if (FreeImage_GetFileTypeFromMemory(nullptr) != FIF_UNKNOWN) {
    return 4;
  }
  if (FreeImage_GetFIFFromFilename("archive.xyz") != FIF_UNKNOWN) {
    return 5;
  }

  // The five implemented formats report read+write support via the registry.
  const FREE_IMAGE_FORMAT supported[] = {FIF_BMP, FIF_JPEG, FIF_PNG, FIF_GIF, FIF_WEBP};
  for (FREE_IMAGE_FORMAT fif : supported) {
    if (!FreeImage_FIFSupportsReading(fif) || !FreeImage_FIFSupportsWriting(fif)) {
      return 6;
    }
  }
  if (FreeImage_FIFSupportsReading(FIF_UNKNOWN)) {
    return 7;
  }

  FreeImage_DeInitialise();
  return 0;
}
