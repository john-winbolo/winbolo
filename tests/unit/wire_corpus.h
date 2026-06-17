/*
 * Test-only sink for the WIRE_CORPUS_TAP hook in wire_codec.h.
 *
 * A test calls wireCorpusReset() to start collecting; every tapped packer
 * call then appends its (label, bytes) here until wireCorpusStop(). With
 * collection disabled the tap is a no-op, so linking this in is harmless to
 * any test that never turns it on.
 */
#ifndef WINBOLO_TEST_WIRE_CORPUS_H
#define WINBOLO_TEST_WIRE_CORPUS_H

#include <stddef.h>
#include <stdint.h>

/* Clear the buffer and begin collecting tapped instances. */
void wireCorpusReset(void);
/* Stop collecting; the buffer is left intact for reading. */
void wireCorpusStop(void);

/* The sink wire_codec.h's WIRE_CORPUS_TAP expands to under capture builds.
 * Appends one instance when collecting; a no-op otherwise. */
void wireCorpusTap(const char *label, const uint8_t *bytes, size_t len);

/* Number of instances collected since the last reset. */
size_t wireCorpusCount(void);
/* Label / bytes of instance i (0 <= i < wireCorpusCount()). */
const char *wireCorpusLabelAt(size_t i);
const uint8_t *wireCorpusBytesAt(size_t i, size_t *len);

#endif /* WINBOLO_TEST_WIRE_CORPUS_H */
