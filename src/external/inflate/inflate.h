#ifndef QC_INFLATE_H
#define QC_INFLATE_H

#include "diaglogger/types.h"
#include <cstddef>
#include <cstdint>

namespace QC {

/// Inflate a zlib stream (2-byte header + DEFLATE + adler32) into `out`.
/// Returns true on success. `out` is cleared then filled.
bool zlib_inflate(const uint8_t* data, size_t len, ByteBuffer& out);

/// Inflate a raw DEFLATE stream (no zlib wrapper) into `out`.
bool raw_inflate(const uint8_t* data, size_t len, ByteBuffer& out);

} // namespace QC

#endif // QC_INFLATE_H
