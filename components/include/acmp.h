/* Analog voltage comparators (ESP32-P4 only, TRM chapter 66): comparator 0 on GPIO51 (reference pad) / GPIO52 (main), comparator
 * 1 on GPIO53 / GPIO54. The output is internal: the driver exposes the edge events (rising / falling / either) as flags, ETM
 * events (etm.h) and the GPIO-matrix comparator interrupt. Reference: external pad, or internal 0.1 .. 0.7 x VDD_IO_6. */
#pragma once
#include "ndf.h"

typedef enum { ACMP_REF_INTERNAL, ACMP_REF_EXTERNAL } acmp_ref_t;
enum { ACMP_EV_NEG = 1, ACMP_EV_POS = 2, ACMP_EV_ANY = 4 };
int      acmp_enable(int unit, acmp_ref_t ref, int ref_tenths);   /* ref_tenths 0..7: internal reference = ref_tenths / 10 * VDD_IO_6 */
void     acmp_disable(int unit);
uint32_t acmp_events(int unit);                                      /* pending ACMP_EV_* flags */
void     acmp_clear(int unit, uint32_t ev);
