#pragma once
#include <stddef.h>
#include <stdint.h>
uint32_t crc32_le(uint32_t crc, const void *buf, size_t len);
