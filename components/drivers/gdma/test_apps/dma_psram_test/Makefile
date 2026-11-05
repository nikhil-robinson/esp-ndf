# dma_psram_test: DMA with PSRAM buffers: SPI2 master writes 8 KB from a PSRAM buffer the CPU just filled (lines dirty in the data cache) to the SPI3 HD slave, whose DMA writes into a...
NDF_PATH ?= ../../../../..
TEST_SOC = esp32s3             # radio / Xtensa / S3 registers: not for the P4
TEST_KIND = unit
TEST_PASS = dma_psram_test PASS done
include $(NDF_PATH)/make/project.mk
