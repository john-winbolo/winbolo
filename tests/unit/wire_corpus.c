/*
 * Implementation of the WIRE_CORPUS_TAP sink (see wire_corpus.h) and the
 * wireCorpusTap symbol that wire_codec.h's tap expands to under
 * WB_WIRE_CORPUS_CAPTURE. Storage is a fixed-capacity buffer; instances wider
 * than WC_BYTES or beyond WC_MAX_INSTANCES are dropped rather than grown.
 */
#include "wire_corpus.h"

#include <string.h>

#define WC_MAX_INSTANCES 4096
#define WC_LABEL_LEN     32
#define WC_BYTES         64

static int      s_enabled;
static size_t   s_count;
static char     s_label[WC_MAX_INSTANCES][WC_LABEL_LEN];
static uint8_t  s_bytes[WC_MAX_INSTANCES][WC_BYTES];
static size_t   s_len[WC_MAX_INSTANCES];

void wireCorpusReset(void) {
    s_enabled = 1;
    s_count = 0;
}

void wireCorpusStop(void) {
    s_enabled = 0;
}

size_t wireCorpusCount(void) {
    return s_count;
}

const char *wireCorpusLabelAt(size_t i) {
    return (i < s_count) ? s_label[i] : "";
}

const uint8_t *wireCorpusBytesAt(size_t i, size_t *len) {
    if (i >= s_count) {
        if (len) *len = 0;
        return NULL;
    }
    if (len) *len = s_len[i];
    return s_bytes[i];
}

void wireCorpusTap(const char *label, const uint8_t *bytes, size_t len) {
    size_t n;
    if (!s_enabled || s_count >= WC_MAX_INSTANCES || len > WC_BYTES) {
        return;
    }
    n = strlen(label);
    if (n >= WC_LABEL_LEN) n = WC_LABEL_LEN - 1;
    memcpy(s_label[s_count], label, n);
    s_label[s_count][n] = '\0';
    memcpy(s_bytes[s_count], bytes, len);
    s_len[s_count] = len;
    s_count++;
}
