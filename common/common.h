#ifndef __COMMON_H__
#define __COMMON_H__

#include <stdint.h>

#define MAX_STORAGE_TARGETS 56
#define TEST_BIT(x,i) ((x) & (1ULL << (i)))
#define GET_P(loc) ((int)((loc) >> 56))
#define P_MASK UINT64_C(0xFF00000000000000)
#define L_MASK UINT64_C(0x00FFFFFFFFFFFFFF)
#define WITH_P(loc, P) (((loc) & L_MASK) | (((P) << 56) & P_MASK))
#define NO_P UINT64_C(0xFF)

typedef struct {
    uint64_t max_chunk_size;
    uint64_t timestamp;
    uint64_t locations;
} FileInfo;

#define MAX(a,b) ((a) > (b)? (a) : (b))
#define MIN(a,b) ((a) < (b)? (a) : (b))

#endif
