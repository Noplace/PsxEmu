/* See zstd.h: every call fails, so a zstd-compressed CHD does not open. */
#include "zstd.h"

/* An error code, in zstd's convention of a size_t near SIZE_MAX. */
#define PSXEMU_ZSTD_UNSUPPORTED ((size_t)-1)

ZSTD_DStream* ZSTD_createDStream(void) { return NULL; }

size_t ZSTD_freeDStream(ZSTD_DStream* stream) {
  (void)stream;
  return 0;
}

size_t ZSTD_initDStream(ZSTD_DStream* stream) {
  (void)stream;
  return PSXEMU_ZSTD_UNSUPPORTED;
}

size_t ZSTD_decompressStream(ZSTD_DStream* stream, ZSTD_outBuffer* output,
                             ZSTD_inBuffer* input) {
  (void)stream;
  (void)output;
  (void)input;
  return PSXEMU_ZSTD_UNSUPPORTED;
}

unsigned ZSTD_isError(size_t code) { return code == PSXEMU_ZSTD_UNSUPPORTED; }
