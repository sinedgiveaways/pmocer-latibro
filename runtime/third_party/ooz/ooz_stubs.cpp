// LZNA and Bitknit decoders of ooz (kraken.cpp calls them for those two Oodle formats). Orbitals' data is Kraken,
// Mermaid, Selkie and Leviathan only, and ooz's lzna.cpp and bitknit.cpp carry no license notice, so they are not
// taken (AGENTS.md: unlicensed code is ideas only). A stream of these two types fails instead of decoding.
#include "ooz_compat.h"

struct LznaState;
struct BitknitState;

int LZNA_DecodeQuantum(byte *, byte *, byte *, const byte *, const byte *, LznaState *) { return -1; }
void LZNA_InitLookup(LznaState *) {}
void BitknitState_Init(BitknitState *) {}
size_t Bitknit_Decode(const byte *, const byte *, byte *, byte *, byte *, BitknitState *) { return (size_t)-1; }
