#include "gipu/backend.hpp"
#include <algorithm>

namespace gipu {
void add_stats(Stats& total, const Stats& other, bool concurrent) {
  total.files += other.files; total.bytes += other.bytes;
  total.batches += other.batches; total.streams += other.streams;
  total.cpu_crc_bytes += other.cpu_crc_bytes; total.gpu_crc_bytes += other.gpu_crc_bytes;
  total.lookahead_batches += other.lookahead_batches;
  total.cpu_buffered_files += other.cpu_buffered_files; total.cpu_stream_files += other.cpu_stream_files;
  total.cpu_parallel_files += other.cpu_parallel_files; total.isal_files += other.isal_files;
  total.workspace = std::max(total.workspace, other.workspace);
  total.host_buffer_bytes = concurrent ? total.host_buffer_bytes + other.host_buffer_bytes :
                                        std::max(total.host_buffer_bytes, other.host_buffer_bytes);
  total.read_seconds += other.read_seconds; total.write_seconds += other.write_seconds;
  total.decode_seconds += other.decode_seconds; total.crc_seconds += other.crc_seconds;
  total.transfer_seconds += other.transfer_seconds; total.allocation_seconds += other.allocation_seconds;
}
}
