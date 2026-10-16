// Links every FreeImage_* function declared in the shipped header. If a
// declared function is not defined by the library, this fails to link.
#include <FreeImage.h>

int main() {
  void* const functions[] = {
      // Init / version / messages
      (void*) &FreeImage_Initialise,
      (void*) &FreeImage_DeInitialise,
      (void*) &FreeImage_GetVersion,
      (void*) &FreeImage_GetCopyrightMessage,
      (void*) &FreeImage_SetOutputMessage,
      (void*) &FreeImage_SetOutputMessageStdCall,
      (void*) &FreeImage_OutputMessageProc,
      // Load / save
      (void*) &FreeImage_Unload,
      (void*) &FreeImage_Load,
      (void*) &FreeImage_LoadU,
      (void*) &FreeImage_LoadFromHandle,
      (void*) &FreeImage_Save,
      (void*) &FreeImage_SaveU,
      (void*) &FreeImage_SaveToHandle,
      // Memory I/O
      (void*) &FreeImage_OpenMemory,
      (void*) &FreeImage_CloseMemory,
      (void*) &FreeImage_LoadFromMemory,
      (void*) &FreeImage_SaveToMemory,
      (void*) &FreeImage_TellMemory,
      (void*) &FreeImage_SeekMemory,
      (void*) &FreeImage_AcquireMemory,
      (void*) &FreeImage_ReadMemory,
      (void*) &FreeImage_WriteMemory,
      // Plugin / format queries
      (void*) &FreeImage_GetFIFFromFilename,
      (void*) &FreeImage_GetFIFFromFilenameU,
      (void*) &FreeImage_FIFSupportsReading,
      (void*) &FreeImage_FIFSupportsWriting,
      // Multipage
      (void*) &FreeImage_OpenMultiBitmap,
      (void*) &FreeImage_OpenMultiBitmapFromHandle,
      (void*) &FreeImage_CloseMultiBitmap,
      (void*) &FreeImage_GetPageCount,
      (void*) &FreeImage_LockPage,
      (void*) &FreeImage_UnlockPage,
      // File type
      (void*) &FreeImage_GetFileType,
      (void*) &FreeImage_GetFileTypeU,
      (void*) &FreeImage_GetFileTypeFromMemory,
      // Image type
      (void*) &FreeImage_GetImageType,
      // Pixel / bits
      (void*) &FreeImage_GetBits,
      (void*) &FreeImage_GetScanLine,
      (void*) &FreeImage_GetPixelIndex,
      (void*) &FreeImage_GetPixelColor,
      (void*) &FreeImage_SetPixelIndex,
      (void*) &FreeImage_SetPixelColor,
      // DIB info
      (void*) &FreeImage_GetBPP,
      (void*) &FreeImage_GetWidth,
      (void*) &FreeImage_GetHeight,
      (void*) &FreeImage_GetLine,
      (void*) &FreeImage_GetPitch,
      // Conversion
      (void*) &FreeImage_ConvertTo24Bits,
      (void*) &FreeImage_ConvertTo32Bits,
      (void*) &FreeImage_ConvertFromRawBits,
      (void*) &FreeImage_ConvertFromRawBitsEx,
      (void*) &FreeImage_ConvertToRawBits,
      // Toolkit
      (void*) &FreeImage_Rescale,
      (void*) &FreeImage_PreMultiplyWithAlpha,
      // Metadata
      (void*) &FreeImage_GetTagCount,
      (void*) &FreeImage_GetTagValue,
      (void*) &FreeImage_GetMetadata,
  };
  return sizeof(functions) / sizeof(*functions) == 58 ? 0 : 1;
}
