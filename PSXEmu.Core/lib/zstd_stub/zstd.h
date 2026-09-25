/* PSXEmu's stand-in for zstd.h - not the zstd library.

   libchdr can read CHDs compressed with zstd (the "zstd" and "cdzs" codecs),
   and includes <zstd.h> for them. chdman only uses those codecs when asked to;
   its default for CDs is LZMA, zlib and FLAC, which are all built here. Rather
   than vendor zstd for the rare disc that needs it, this declares the part of
   its API libchdr calls and makes every call fail, so opening a zstd CHD fails
   at chd_open - and psx/disc.cpp then reads the header to say why.

   Replacing this directory with the real zstd's lib/ (and building its
   decompress sources) is all zstd support would take. */
#ifndef PSXEMU_ZSTD_STUB_H
#define PSXEMU_ZSTD_STUB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ZSTD_DCtx_s ZSTD_DStream;

typedef struct ZSTD_inBuffer_s {
  const void* src;
  size_t size;
  size_t pos;
} ZSTD_inBuffer;

typedef struct ZSTD_outBuffer_s {
  void* dst;
  size_t size;
  size_t pos;
} ZSTD_outBuffer;

ZSTD_DStream* ZSTD_createDStream(void);
size_t ZSTD_freeDStream(ZSTD_DStream* stream);
size_t ZSTD_initDStream(ZSTD_DStream* stream);
size_t ZSTD_decompressStream(ZSTD_DStream* stream, ZSTD_outBuffer* output,
                             ZSTD_inBuffer* input);
unsigned ZSTD_isError(size_t code);

#ifdef __cplusplus
}
#endif

#endif
