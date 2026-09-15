#ifndef GUARD_ALLOC_H
#define GUARD_ALLOC_H


#define FREE_AND_SET_NULL(ptr)          \
{                                       \
    Free(ptr);                          \
    ptr = NULL;                         \
}

#define TRY_FREE_AND_SET_NULL(ptr) if (ptr != NULL) FREE_AND_SET_NULL(ptr)

// Emerald's HEAP_SIZE is 0x1C000 (112KB), sized for a game where most large
// buffers (Union Room player lists, battle/contest scratch data, etc.) are
// heap-allocated. pokeruby has none of that -- everything still lives in
// static EWRAM_DATA globals -- and only has ~23KB of EWRAM headroom to spare
// (see CLAUDE.md). Sized down to what the RFU connection layer actually
// needs; grow this if a future Alloc() call runs out of space.
#define HEAP_SIZE 0x2000
extern u8 gHeap[HEAP_SIZE];

void *Alloc(u32 size);
void *AllocZeroed(u32 size);
void Free(void *pointer);
void InitHeap(void *heapStart, u32 heapSize);

#endif // GUARD_ALLOC_H
