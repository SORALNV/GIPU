#include "gipu/checksum.hpp"
#ifdef GIPU_HAVE_LIBDEFLATE
#include <libdeflate.h>
#else
#include <zlib.h>
#endif

namespace gipu {
uint32_t cpu_crc32(uint32_t previous, const void* data, size_t bytes) {
#ifdef GIPU_HAVE_LIBDEFLATE
  return libdeflate_crc32(previous, data, bytes);
#else
  return static_cast<uint32_t>(crc32_z(previous, static_cast<const Bytef*>(data), bytes));
#endif
}
}
