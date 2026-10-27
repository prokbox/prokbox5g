#ifndef RBCODECPLATFORM_H_INCLUDED
#define RBCODECPLATFORM_H_INCLUDED

/* assert */
#include <assert.h>

/* O_RDONLY, O_WRONLY, O_CREAT, O_APPEND */
#include <fcntl.h>

/* isdigit, islower, isprint, isspace, toupper */
#include <ctype.h>

/* memchr, memcmp, memcpy, memmove, memset, strcat, strchr, strcmp, strcpy,
 * strlen, strncmp, strrchr */
#include <string.h>

/* strcasecmp */
#include <strings.h>

/* abs, atoi, strtol, strtoul, labs, rand */
#include <stdlib.h>

#ifdef __APPLE__
/* macOS has neither <byteswap.h> nor <endian.h> */
#include <libkern/OSByteOrder.h>
#include <machine/endian.h>
#ifndef bswap_16
#define bswap_16(x) OSSwapInt16(x)
#endif
#ifndef bswap_32
#define bswap_32(x) OSSwapInt32(x)
#endif
#ifndef htobe16
#define htobe16(x) OSSwapHostToBigInt16(x)
#endif
#ifndef htole16
#define htole16(x) OSSwapHostToLittleInt16(x)
#endif
#ifndef be16toh
#define be16toh(x) OSSwapBigToHostInt16(x)
#endif
#ifndef le16toh
#define le16toh(x) OSSwapLittleToHostInt16(x)
#endif
#ifndef htobe32
#define htobe32(x) OSSwapHostToBigInt32(x)
#endif
#ifndef htole32
#define htole32(x) OSSwapHostToLittleInt32(x)
#endif
#ifndef be32toh
#define be32toh(x) OSSwapBigToHostInt32(x)
#endif
#ifndef le32toh
#define le32toh(x) OSSwapLittleToHostInt32(x)
#endif
#ifndef htobe64
#define htobe64(x) OSSwapHostToBigInt64(x)
#endif
#ifndef htole64
#define htole64(x) OSSwapHostToLittleInt64(x)
#endif
#ifndef be64toh
#define be64toh(x) OSSwapBigToHostInt64(x)
#endif
#ifndef le64toh
#define le64toh(x) OSSwapLittleToHostInt64(x)
#endif
#else
#include <byteswap.h>
#include <endian.h>
#endif

/* swap16, swap32 */
#ifndef swap16
#define swap16(x) bswap_16(x)
#endif
#ifndef swap32
#define swap32(x) bswap_32(x)
#endif

/* hto{be,le}{16,32}, {be,le}toh{16,32}, ROCKBOX_{BIG,LITTLE}_ENDIAN */
#ifndef betoh16
#define betoh16 be16toh
#endif
#ifndef betoh32
#define betoh32 be32toh
#endif
#ifndef letoh16
#define letoh16 le16toh
#endif
#ifndef letoh32
#define letoh32 le32toh
#endif
#if BYTE_ORDER == LITTLE_ENDIAN
#define ROCKBOX_LITTLE_ENDIAN 1
#else
#define ROCKBOX_BIG_ENDIAN 1
#endif

/* filesize */
off_t ffilesize(int fd);

/* snprintf */
#include <stdio.h>

/* debugf, logf */
/*
#ifdef DEBUG
#define debugf(...) fprintf(stderr, __VA_ARGS__)
#ifndef logf
#define logf(...) do { fprintf(stderr, __VA_ARGS__); \
                       putc('\n', stderr);           \
                  } while (0)
#endif
#ifndef panicf
#define panicf(...) do { fprintf(stderr, __VA_ARGS__); \
                         putc('\n', stderr);           \
                         exit(-1);                     \
                    } while (0)
#endif
#endif
*/

static inline bool tdspeed_alloc_buffers(int32_t **buffers,
    const int *buf_s, int nbuf)
{
    int i;
    for (i = 0; i < nbuf; i++)
    {
        buffers[i] = malloc(buf_s[i]);
        if (!buffers[i])
            return false;
    }
    return true;
}

static inline void tdspeed_free_buffers(int32_t **buffers, int nbuf)
{
    int i;
    for (i = 0; i < nbuf; i++)
    {
        free(buffers[i]);
    }
}

#endif
