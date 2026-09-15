#ifndef GUARD_RANDOM_H
#define GUARD_RANDOM_H

extern u32 gRngValue;
extern u32 gRng2Value;

//Returns a 16-bit pseudorandom number
u16 Random(void);

//Returns a 32-bit pseudorandom number
#define Random32() (Random() | (Random() << 16))

//Sets the initial seed value of the pseudorandom number generator
void SeedRng(u16 seed);

// Second, independent RNG stream -- see the comment on gRng2Value in random.c
u16 Random2(void);
void SeedRng2(u16 seed);

#endif // GUARD_RANDOM_H

