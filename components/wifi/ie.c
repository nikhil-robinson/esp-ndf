/* Information elements and beacon parsing: pure, every length is checked against the frame. */
#include <string.h>
#include "wifi.h"

int wifi_ie_find(const uint8_t *ies, unsigned n, uint8_t id, const uint8_t **val)
{
    unsigned i = 0;
    while (i + 2 <= n) {
        unsigned l = ies[i + 1];
        if (i + 2 + l > n) return -1;                           /* element runs past the frame */
        if (ies[i] == id) { *val = ies + i + 2; return (int)l; }
        i += 2 + l;
    }
    return -1;
}

int wifi_parse_beacon(const uint8_t *f, unsigned len, wifi_ap_t *ap)
{
    if (len < 36 || (f[0] != 0x80 && f[0] != 0x50)) return -1;
    memset(ap, 0, sizeof *ap);
    memcpy(ap->bssid, f + 16, 6);
    ap->beacon_tu = (uint16_t)(f[32] | f[33] << 8);
    const uint8_t *ie = f + 36, *v; unsigned n = len - 36;
    for (unsigned i = 0; i < n; i += 2 + ie[i + 1]) if (i + 2 > n || i + 2 + ie[i + 1] > n) return -1;   /* every element must fit, not just the ones we read */
    int l = wifi_ie_find(ie, n, 0, &v);
    if (l < 0 || l > 32) return -1;                             /* a beacon without a valid SSID element is malformed */
    memcpy(ap->ssid, v, (unsigned)l);
    ap->chan = (l = wifi_ie_find(ie, n, 3, &v)) == 1 ? v[0] : 0;
    ap->sec = (f[34] & 0x10) != 0;                              /* capability: privacy */
    return 0;
}
