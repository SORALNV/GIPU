#pragma once
#include <cstddef>
#include <cstdint>

namespace gipu {
// CRC-32/ISO-HDLC（ZIPと同じ）。初回は0、以降は前回の戻り値を渡す。
uint32_t cpu_crc32(uint32_t previous, const void* data, size_t bytes);
}
