/*
 * Storage checksum boundary. It borrows input bytes. Combined builds use the
 * SDK's CRC32C implementation, standalone Storage uses its bundled Infer dependency.
 * Neither path takes any Storage locks.
 */
#ifndef MINIINFER_STORAGE_CRC_H
#define MINIINFER_STORAGE_CRC_H

#ifdef MINIINFER_WITH_KVFABRIC
#include "kvfabric/kvfabric.h"
#define mi_crc32c kvf_crc32c_extend
#else
#include "infer/base.h"
#define mi_crc32c if_crc32c
#endif

#endif
