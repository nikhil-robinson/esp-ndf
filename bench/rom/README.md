ESP32-S3 ROM, symbolised for objdump. `rom.bin` is not committed (vendor code); recreate it with
`esptool.py --chip esp32s3 -p <port> dump_mem 0x40000000 0x60000 rom.bin`, then `python3 mkelf.py`
and `xtensa-esp32s3-elf-objdump -d rom.elf > rom.dis`. ROM jump-table entries (`l32r a9; jx a9`) get
their real target named `<name>_impl`.
