#include "global.h"
#include "random.h"

// The number 1103515245 comes from the example implementation of rand and srand
// in the ISO C standard.

u32 gRngValue;

// Second, independent RNG stream for the RFU connection layer, so its own
// random calls (e.g. connection retry jitter) don't perturb gameplay RNG
// (wild encounters, critical hits, etc.) by consuming from the same stream.
u32 gRng2Value;

u16 Random(void)
{
    gRngValue = 1103515245 * gRngValue + 24691;
    return gRngValue >> 16;
}

void SeedRng(u16 seed)
{
    gRngValue = seed;
}

u16 Random2(void)
{
    gRng2Value = 1103515245 * gRng2Value + 24691;
    return gRng2Value >> 16;
}

void SeedRng2(u16 seed)
{
    gRng2Value = seed;
}
