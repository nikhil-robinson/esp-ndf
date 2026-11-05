#!/bin/sh
# Regenerate components/soc/esp32p4/pmu_img.h from the IDF tree (needs gcc and ESP-IDF v6.1: IDF=~/esp-idf-v6.1 by default).
set -e
D=$(cd "$(dirname "$0")" && pwd); I=${IDF:-$HOME/esp-idf-v6.1}/components; P=$I/esp_hw_support/port/esp32p4; R=$I/soc/esp32p4/register/hw_ver3
STUB=$(mktemp -d); : > $STUB/sdkconfig.h
INC="-I$STUB -I$R -I$R/soc -I$I/soc/esp32p4/include -I$I/esp_common/include -I$I/hal/include -I$I/hal/esp32p4/include
 -I$I/esp_hal_pmu/esp32p4/include -I$I/esp_hal_pmu/include -I$I/soc/include -I$I/esp_rom/include -I$I/esp_hal_efuse/include
 -I$I/esp_hal_efuse/esp32p4/include -I$I/hal/platform_port/include -I$I/log/include -I$I/esp_hw_support/include -I$I/soc/esp32p4
 -I$P/private_include -I$P/include -I$P -I$I/esp_hw_support/port/include -I$I/esp_hw_support/include/esp_private
 -I$I/esp_rom/esp32p4/include/esp32p4 -I$I/esp_rom/esp32p4/include -I$I/esp_rom/include/esp32p4
 -I$I/esp_hal_regi2c/include -I$I/esp_hal_regi2c/esp32p4/include"
gcc -w -DHAL_CONFIG_CHIP_SUPPORT_MIN_REV=300 $INC -include stdlib.h -o $STUB/gen $D/gen.c $I/esp_hal_pmu/esp32p4/pmu_hal.c $P/pmu_param.c
$STUB/gen > $D/../../components/soc/esp32p4/pmu_img.h
rm -rf $STUB
