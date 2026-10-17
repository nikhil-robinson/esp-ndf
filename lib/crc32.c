/* CRC-32 (IEEE, reflected). crc32_le follows the ROM convention the NVS format
 * uses: crc32_le(0xffffffff, ...) == standard CRC of the data with seed 0 inverted. */
#include "crc32.h"

uint32_t crc32_le(uint32_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;
    crc = ~crc;
    while (len--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
    }
    return ~crc;
}
