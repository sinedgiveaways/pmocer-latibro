// The C entry point of kraken.cpp (see ooz.h).
#include "ooz_compat.h"
#include "ooz.h"

int Kraken_Decompress(const byte *src, size_t src_len, byte *dst, size_t dst_len);

extern "C" int ooz_decompress(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len) {
    return Kraken_Decompress(src, src_len, dst, dst_len);
}
