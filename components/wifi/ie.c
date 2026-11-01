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
    if ((l = wifi_ie_find(ie, n, 61, &v)) >= 2 && (v[1] & 4) && ((v[1] & 3) == 1 || (v[1] & 3) == 3)) ap->sec40 = v[1] & 3;   /* HT Operation: STA channel width any + secondary offset */
    ap->sec = (f[34] & 0x10) != 0;                              /* capability: privacy: 1 = WEP / WPA / WPA2 */
    if ((l = wifi_ie_find(ie, n, 48, &v)) >= 8 && v[0] == 1) {        /* RSN element: version, group suite, pairwise count + list, AKM count + list, capabilities */
        ap->sec = 2; unsigned o = 6, c;
        if (o + 2 > (unsigned)l) return 0;
        c = v[o] | v[o + 1] << 8; o += 2 + 4 * c;
        if (o + 2 > (unsigned)l) return 0;
        c = v[o] | v[o + 1] << 8; o += 2;
        for (unsigned i = 0; i < c; i++, o += 4) if (o + 4 <= (unsigned)l && !memcmp(v + o, "\x00\x0f\xac", 3) && v[o + 3] < 16) ap->akm |= (uint16_t)(1u << v[o + 3]);
        if (o + 2 <= (unsigned)l) ap->rsn_caps = (uint16_t)(v[o] | v[o + 1] << 8);
    }
    return 0;
}
