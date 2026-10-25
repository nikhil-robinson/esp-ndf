# Security policy

## Reporting a vulnerability
Please report suspected vulnerabilities privately to the maintainer (nikhilrobinson001@gmail.com) - not in public
issues. Include the affected file / API, a description or reproducer, and the board/revision. You will get an answer
within 7 days; fixes are developed privately and released with a note in this file and the commit log.

## Supported versions
The `main` branch. Security fixes are not backported.

## Security regression tests
`make security-test` runs the host fuzzers (FAT, SPIFFS, NVS, WL, USB MSC, PFS power-cut) and the signature check;
on-target tests live in tests/runtime_test, components/soc/test_apps/crash_test, components/drivers/storage/test_apps/ota_test and components/drivers/gdma/test_apps/dma_psram_test.

## Known accepted risks
Secure boot, flash encryption and JTAG lockdown require eFuse burns and are not enabled by the SDK (docs/security/THREAT_MODEL.md).
