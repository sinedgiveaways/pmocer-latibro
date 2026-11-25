// C entry point to ooz's decompressor (kraken.cpp, powzix/ooz @ 0503806, GPL-3.0-or-later).
#pragma once
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Decompress one raw Oodle stream (Kraken, Mermaid, Selkie or Leviathan, as the IoStore stores a block) of exactly
// dst_len bytes into dst. The decoder writes up to OOZ_SAFE_SPACE bytes past dst_len and may read past the end of src:
// the caller sizes dst with that space and pads src with zeros. Returns dst_len, or -1 on a malformed stream. ooz is
// not hardened against hostile input; the data here is the player's own game files.
#define OOZ_SAFE_SPACE 64
int ooz_decompress(const unsigned char *src, size_t src_len, unsigned char *dst, size_t dst_len);

#ifdef __cplusplus
}
#endif
