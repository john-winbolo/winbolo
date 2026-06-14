/*
 * Single-definition wire codec machinery.
 *
 * Declare a message's fields once as an X-macro list (field order is wire
 * order); DEFINE_WIRE_CODEC_NAMED generates a matching pack/unpack pair and
 * WIRE_SIZE_OF computes the on-wire byte count from the same list. Because
 * encoder and decoder both walk one field list in one order, they cannot
 * drift, and every generated unpack carries an `avail` bounds check per field.
 *
 * Field type tokens are U8, U16, U32; the U16/U32 forms reuse the big-endian
 * packU16/packU32 helpers declared in transport_udp_internal.h.
 *
 * WIRE_CORPUS_TAP records the bytes a packer just wrote so a test can capture
 * a golden corpus. It expands to nothing unless WB_WIRE_CORPUS_CAPTURE is
 * defined, which no shipping build sets.
 */
#ifndef WINBOLO_WIRE_CODEC_H
#define WINBOLO_WIRE_CODEC_H

#include <stddef.h>
#include <stdint.h>

#include "transport_udp_internal.h"  /* packU16/unpackU16, packU32/unpackU32 */

/* Per-type wire primitives. */
#define WIRE_PACK_U8(b, p, v)   ((b)[p] = (uint8_t)(v))
#define WIRE_PACK_U16(b, p, v)  packU16((b) + (p), (uint16_t)(v))
#define WIRE_PACK_U32(b, p, v)  packU32((b) + (p), (uint32_t)(v))
#define WIRE_UNPACK_U8(b, p)    ((b)[p])
#define WIRE_UNPACK_U16(b, p)   unpackU16((b) + (p))
#define WIRE_UNPACK_U32(b, p)   unpackU32((b) + (p))
#define WIRE_SIZE_U8   1
#define WIRE_SIZE_U16  2
#define WIRE_SIZE_U32  4

/* Per-field iterators, applied to a field list by the codec definer and by
 * WIRE_SIZE_OF. */
#define WIRE_PACK_FIELD(type, name) \
    WIRE_PACK_##type(buf, pos, s->name); pos += WIRE_SIZE_##type;
#define WIRE_UNPACK_FIELD(type, name) \
    if (avail < pos + WIRE_SIZE_##type) return 0; \
    s->name = WIRE_UNPACK_##type(buf, pos); pos += WIRE_SIZE_##type;
#define WIRE_SIZE_FIELD(type, name) + WIRE_SIZE_##type

/* Constant on-wire size of a field list. */
#define WIRE_SIZE_OF(FIELDS) (0 FIELDS(WIRE_SIZE_FIELD))

/* Generate pack<suffix> / unpack<suffix> for StructType from FIELDS. The
 * function suffix is decoupled from the struct type so a test can instantiate
 * a generated codec under a *_gen name beside the hand-rolled functions for the
 * same struct. */
#define DEFINE_WIRE_CODEC_NAMED(suffix, StructType, FIELDS)             \
    static inline int pack##suffix(uint8_t *buf, const StructType *s) { \
        size_t pos = 0; FIELDS(WIRE_PACK_FIELD) return (int)pos; }      \
    static inline int unpack##suffix(const uint8_t *buf, size_t avail,  \
                                     StructType *s) {                   \
        size_t pos = 0; FIELDS(WIRE_UNPACK_FIELD) return (int)pos; }

#define DEFINE_WIRE_CODEC(Name, FIELDS) \
    DEFINE_WIRE_CODEC_NAMED(Name, Name, FIELDS)

/* Corpus capture hook. Compiled out (and referencing nothing) unless the
 * capture build flag is set. The implementation lives in a test TU. */
#ifdef WB_WIRE_CORPUS_CAPTURE
void wireCorpusTap(const char *label, const uint8_t *bytes, size_t len);
#define WIRE_CORPUS_TAP(label, b, n) wireCorpusTap((label), (b), (n))
#else
#define WIRE_CORPUS_TAP(label, b, n) ((void)0)
#endif

#endif /* WINBOLO_WIRE_CODEC_H */
