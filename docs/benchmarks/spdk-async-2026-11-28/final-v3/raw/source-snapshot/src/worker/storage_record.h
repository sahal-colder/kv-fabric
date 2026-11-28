/* Version 3, big endian: 4 KiB metadata sector + padded payload.
 * V1 compact and V2 direct records are intentionally rejected by this path. */
#ifndef MINIINFER_STORAGE_RECORD_H
#define MINIINFER_STORAGE_RECORD_H
#include "miniinfer/storage_backend.h"
#include "storage_crc.h"
#include <string.h>
enum { MI_RECORD_HEADER = 4096, MI_RECORD_VERSION = 3 };
static inline void mi_record_put(unsigned char *p, unsigned n, uint64_t value) {
    for (unsigned i=0; i<n; ++i) p[n-i-1] = (unsigned char)(value >> (8*i));
}
static inline uint64_t mi_record_get(const unsigned char *p, unsigned n) {
    uint64_t v=0;
    for (unsigned i=0; i<n; ++i) v=(v<<8)|p[i];
    return v;
}
static inline void mi_record_encode(unsigned char *p, const mi_block_meta *m, uint32_t crc) {
    memset(p,0,MI_RECORD_HEADER);
    memcpy(p,"MIFSSD3",8);
    mi_record_put(p+8,8,m->id); mi_record_put(p+16,8,m->version);
    mi_record_put(p+24,8,m->size); mi_record_put(p+32,4,m->id>>32);
    mi_record_put(p+36,4,crc);
    mi_record_put(p+44,4,MI_RECORD_VERSION); mi_record_put(p+48,4,MI_RECORD_HEADER);
    /* Header checksum covers the entire metadata sector, including padding. */
    uint32_t h=mi_crc32c(0,p,40);
    h=mi_crc32c(h,p+44,MI_RECORD_HEADER-44);
    mi_record_put(p+40,4,h);
}
static inline int mi_record_valid_payload(const unsigned char *p, const void *payload,
                                           const mi_block_meta *m, mi_ssd_verify verify) {
    uint32_t h=mi_crc32c(0,p,40);
    h=mi_crc32c(h,p+44,MI_RECORD_HEADER-44);
    return !memcmp(p,"MIFSSD3",8) && mi_record_get(p+44,4)==MI_RECORD_VERSION &&
        mi_record_get(p+48,4)==MI_RECORD_HEADER && mi_record_get(p+8,8)==m->id &&
        mi_record_get(p+16,8)==m->version && mi_record_get(p+24,8)==m->size &&
        mi_record_get(p+32,4)==(uint32_t)(m->id>>32) && mi_record_get(p+40,4)==h &&
        (verify==MI_SSD_VERIFY_NONE || mi_record_get(p+36,4)==mi_crc32c(0,payload,m->size));
}
static inline int mi_record_valid(const unsigned char *p, const mi_block_meta *m, mi_ssd_verify verify) {
    return mi_record_valid_payload(p,p+MI_RECORD_HEADER,m,verify);
}
#endif
