// Ips3 shim for snappy: "compression" that stores the data as is.
#pragma once

#include "third_party/snappy/snappy-sinksource.h"

namespace snappy {

inline size_t Compress(Source* source, Sink* sink) {
  size_t total = 0;
  while (source->Available()) {
    size_t n;
    const char* p = source->Peek(&n);
    sink->Append(p, n);
    source->Skip(n);
    total += n;
  }
  return total;
}

inline size_t MaxCompressedLength(size_t n) { return n; }

}  // namespace snappy
