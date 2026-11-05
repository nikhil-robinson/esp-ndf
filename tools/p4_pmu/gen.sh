#!/bin/sh
# Regenerate components/soc/esp32p4/pmu_img.h from the IDF tree (needs gcc and ~/esp-idf-v6.1).
set -e
D=$(cd "$(dirname "$0")" && pwd); I=${IDF:-$HOME/esp-idf-v6.1}/components; P=$I/esp_hw_support/port/esp32p4
STUB=$(mktemp -d); : > $STUB/sdkconfig.h
FLAGS="-w -DHAL_CONFIG_CHIP_SUPPORT_MIN_REV=300 -I$STUB $(sed "s|~|$HOME|g" $D/cflags.txt | sed 's|^-w -DHAL_CONFIG_CHIP_SUPPORT_MIN_REV=300 -I/tmp/claude-1000/stub||')"
gcc $FLAGS -I$P -o $STUB/gen $D/gen.c $I/esp_hal_pmu/esp32p4/pmu_hal.c $P/pmu_param.c -include stdlib.h
$STUB/gen > $D/../../components/soc/esp32p4/pmu_img.h
rm -rf $STUB
