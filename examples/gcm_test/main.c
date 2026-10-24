/* AES-GCM against python cryptography tags (bench/gcm/vectors.h): encrypt (tag + first block), decrypt in place,
 * tampered tag rejected; timing per size (sibling: bench/idf_gcm). */
#include "console.h"
#include "aes.h"
#include "sys.h"
#include "xtensa.h"
#include <string.h>
#include "../../bench/gcm/vectors.h"

static uint8_t buf[16384], pt[16384];

int main(void)
{
    delay_ms(200);
    int fail = 0;
    for (unsigned v = 0; v < sizeof gcm_vec / sizeof gcm_vec[0]; v++) {
        const gcm_vec_t *c = &gcm_vec[v];
        uint8_t key[32], iv[16], aad[20], tag[16];
        for (int i = 0; i < c->kl; i++) key[i] = i + 1;
        for (int i = 0; i < c->il; i++) iv[i] = 0xa0 + i;
        for (int i = 0; i < c->al; i++) aad[i] = i ^ 0x5a;
        for (int i = 0; i < c->len; i++) pt[i] = (i * 7 + 3) & 255;
        uint32_t t = ccount(); aes_gcm(key, c->kl, true, iv, c->il, aad, c->al, pt, buf, c->len, tag); uint32_t te = ccount() - t;
        int ok1 = !memcmp(tag, c->tag, 16) && !memcmp(buf, c->c0, c->len < 16 ? c->len : 16);
        t = ccount(); int rd = aes_gcm(key, c->kl, false, iv, c->il, aad, c->al, buf, buf, c->len, tag); uint32_t td = ccount() - t;
        int ok2 = !rd && !memcmp(buf, pt, c->len);
        aes_gcm(key, c->kl, true, iv, c->il, aad, c->al, pt, buf, c->len, tag); tag[3] ^= 1;
        int ok3 = aes_gcm(key, c->kl, false, iv, c->il, aad, c->al, buf, buf, c->len, tag) == -1;
        printf("%s GCM-%d iv %d aad %d len %5d: tag %d, decrypt %d, tamper %d | encrypt %lu us, decrypt %lu us\n",
               ok1 && ok2 && ok3 ? "PASS" : "FAIL", c->kl * 8, c->il, c->al, c->len, ok1, ok2, ok3,
               (unsigned long)(te / 240), (unsigned long)(td / 240));
        fail |= !(ok1 && ok2 && ok3);
    }
    printf("gcm_test %s done\n", fail ? "FAIL" : "PASS");
    for (;;) {}
}
