/* IDF's own chip list, with IDF's Boya driver (spi_flash_chip_boya.c, same functions) given the suspend capability
 * IDF ships it without, so the IDF sibling can run auto-suspend on this board's BY25Q128 for the comparison and as the
 * register reference. Active only with CONFIG_SPI_FLASH_OVERRIDE_CHIP_DRIVER_LIST (sdkconfig.suspend). */
#include "spi_flash_chip_driver.h"
#include "spi_flash_chip_generic.h"
#include "spi_flash_chip_issi.h"
#include "spi_flash_chip_mxic.h"
#include "spi_flash_chip_gd.h"
#include "spi_flash_chip_winbond.h"

static esp_err_t boya_probe(esp_flash_t *chip, uint32_t id)
{
    (void)chip;
    return (id >> 16) == 0x68 && (id & 0xFF00) == 0x4000 ? ESP_OK : ESP_ERR_NOT_FOUND;
}
static spi_flash_caps_t boya_caps(esp_flash_t *chip) { (void)chip; return SPI_FLASH_CHIP_CAP_SUSPEND | SPI_FLASH_CHIP_CAP_UNIQUE_ID; }

static const spi_flash_chip_t boya_sus = {
    .name = "boya+suspend", .timeout = &spi_flash_chip_generic_timeout, .probe = boya_probe,
    .reset = spi_flash_chip_generic_reset, .detect_size = spi_flash_chip_generic_detect_size,
    .erase_chip = spi_flash_chip_generic_erase_chip, .erase_sector = spi_flash_chip_generic_erase_sector,
    .erase_block = spi_flash_chip_generic_erase_block, .sector_size = 4 * 1024, .block_erase_size = 64 * 1024,
    .get_chip_write_protect = spi_flash_chip_generic_get_write_protect,
    .set_chip_write_protect = spi_flash_chip_generic_set_write_protect,
    .read = spi_flash_chip_generic_read, .write = spi_flash_chip_generic_write,
    .program_page = spi_flash_chip_generic_page_program, .page_size = 256,
    .write_encrypted = spi_flash_chip_generic_write_encrypted, .wait_idle = spi_flash_chip_generic_wait_idle,
    .set_io_mode = spi_flash_chip_generic_set_io_mode, .get_io_mode = spi_flash_chip_generic_get_io_mode,
    .read_reg = spi_flash_chip_generic_read_reg, .yield = spi_flash_chip_generic_yield,
    .sus_setup = spi_flash_chip_generic_suspend_cmd_conf, .read_unique_id = spi_flash_chip_generic_read_unique_id,
    .get_chip_caps = boya_caps, .config_host_io_mode = spi_flash_chip_generic_config_host_io_mode,
};

const spi_flash_chip_t *default_registered_chips[] = {
    &esp_flash_chip_issi, &esp_flash_chip_gd, &esp_flash_chip_mxic, &esp_flash_chip_winbond,
    &boya_sus, &esp_flash_chip_generic, NULL,
};
