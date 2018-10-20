#ifndef FU_H
#define FU_H

#ifdef RELEASE
#define DEBUG_FU 0
#else
#define DEBUG_FU 1
#endif

#define debugf(fmt, ...) \
    do { if (DEBUG_FU) fprintf(stderr, "D %s:%d: " fmt "\n", __FILE__, \
            __LINE__, __VA_ARGS__); } while (0)

#define debug(fmt) \
    do { if (DEBUG_FU) fprintf(stderr, "D %s:%d: " fmt "\n", __FILE__, \
            __LINE__); } while (0)

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;

typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;

#endif
