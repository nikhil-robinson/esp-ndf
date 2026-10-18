#!/usr/bin/env python3
"""Inventory of ESP-IDF menuconfig (Kconfig) options and where esp-ndf stands
on each one. Parses every Kconfig file of an IDF tree, keeps the options a
user can set on the ESP32-S3 (visible prompt, not SOC_* capabilities, not
other targets) and writes docs/IDF_CONFIG_INVENTORY.md.

  idf_kconfig_inventory.py <idf_path> [out.md]
"""
import os, re, sys, collections

# component -> (status, note). Options of components not listed: "planned".
COMPONENT_STATUS = {
    'freertos': ('n/a', 'no RTOS by design: bare-metal loops, xcore.h for core-to-core'),
    'pthread': ('n/a', 'no RTOS'), 'esp_ringbuf': ('n/a', 'xc_fifo / driver rings instead'),
    'esp_event': ('planned', 'lightweight event dispatch (TODO 8)'),
    'esp_wifi': ('wireless later', 'rule 1'), 'bt': ('wireless later', 'rule 1'), 'esp_phy': ('wireless later', 'rule 1'),
    'esp_coex': ('wireless later', 'rule 1'), 'lwip': ('wireless later', 'networking after radios'),
    'esp_netif': ('wireless later', ''), 'esp_netif_stack': ('wireless later', ''), 'wpa_supplicant': ('wireless later', ''),
    'mbedtls': ('wireless later', 'TLS; hardware AES done, SHA/RSA planned (TODO 5)'), 'esp-tls': ('wireless later', ''),
    'tcp_transport': ('wireless later', ''), 'esp_http_client': ('wireless later', ''), 'esp_http_server': ('wireless later', ''),
    'esp_https_ota': ('wireless later', ''), 'esp_https_server': ('wireless later', ''), 'mqtt': ('wireless later', ''),
    'openthread': ('n/a', 'S3 has no 802.15.4'), 'ieee802154': ('n/a', 'S3 has no 802.15.4'),
    'esp_eth': ('n/a', 'S3 has no Ethernet MAC'), 'protocomm': ('wireless later', ''), 'wifi_provisioning': ('wireless later', ''),
    'esp_local_ctrl': ('wireless later', ''), 'esp_hid': ('wireless later', 'BLE/USB HID'),
    'unity': ('n/a', 'test framework: own CHECK/METRIC runner'), 'cmock': ('n/a', ''),
    'esp_driver_dac': ('n/a', 'S3 has no DAC'), 'esp_driver_sdio': ('n/a', 'S3 has no SDIO slave'),
    'esp_driver_ana_cmpr': ('n/a', 'S3 has no analog comparator'), 'esp_driver_isp': ('n/a', 'not on S3'),
    'esp_driver_jpeg': ('n/a', 'not on S3'), 'esp_driver_ppa': ('n/a', 'not on S3'), 'esp_driver_parlio': ('n/a', 'not on S3'),
    'linux': ('n/a', 'host target'), 'riscv': ('n/a', 'S3 is Xtensa'),
    'bootloader': ('n/a', 'no second-stage bootloader: the ROM loads the esp-ndf image directly'),
    'bootloader_support': ('planned', 'image verification / secure boot pieces (TODO 3, 5)'),
    'app_trace': ('planned', 'fast trace over JTAG (TODO 8)'), 'esp_gdbstub': ('planned', 'debugging (TODO 8)'),
}

# individual IDF option -> (status, esp-ndf equivalent)
OPTION_MAP = {
    'ESP_DEFAULT_CPU_FREQ_MHZ': ('done', 'NDF_CPU_MHZ (ndf_config.h)'),
    'ESP_DEFAULT_CPU_FREQ_MHZ_80': ('done', 'NDF_CPU_MHZ = 80'), 'ESP_DEFAULT_CPU_FREQ_MHZ_160': ('done', 'NDF_CPU_MHZ = 160'),
    'ESP_DEFAULT_CPU_FREQ_MHZ_240': ('done', 'NDF_CPU_MHZ = 240'),
    'ESPTOOLPY_FLASHMODE_QIO': ('done', 'FLASH_MODE = qio (app.mk); runtime QIO is automatic'),
    'ESPTOOLPY_FLASHMODE_QOUT': ('done', 'FLASH_MODE (app.mk)'), 'ESPTOOLPY_FLASHMODE_DIO': ('done', 'FLASH_MODE = dio (app.mk)'),
    'ESPTOOLPY_FLASHMODE_DOUT': ('done', 'FLASH_MODE (app.mk)'),
    'ESPTOOLPY_FLASHFREQ_80M': ('done', 'FLASH_FREQ = 80m (app.mk)'), 'ESPTOOLPY_FLASHFREQ_40M': ('done', 'FLASH_FREQ = 40m (app.mk)'),
    'ESPTOOLPY_FLASHFREQ_20M': ('done', 'FLASH_FREQ = 20m (app.mk)'), 'ESPTOOLPY_FLASHFREQ_120M': ('planned', 'needs MSPI timing tuning'),
    'ESPTOOLPY_FLASHSIZE_16MB': ('done', 'detected from JEDEC id at run time'),
    'ESPTOOLPY_FLASHSIZE_2MB': ('done', 'detected at run time'), 'ESPTOOLPY_FLASHSIZE_4MB': ('done', 'detected at run time'),
    'ESPTOOLPY_FLASHSIZE_8MB': ('done', 'detected at run time'), 'ESPTOOLPY_FLASHSIZE_32MB': ('planned', '4-byte addressing'),
    'PARTITION_TABLE_OFFSET': ('done', 'PART_TABLE_ADDR (app.mk)'),
    'PARTITION_TABLE_CUSTOM_FILENAME': ('done', 'PARTS=<csv> (make partition)'), 'PARTITION_TABLE_CUSTOM': ('done', 'PARTS=<csv>'),
    'PARTITION_TABLE_MD5': ('done', 'always on (mkpart.py)'),
    'ESP_CONSOLE_USB_SERIAL_JTAG': ('done', 'NDF_CONSOLE = NDF_CONSOLE_USJ'), 'ESP_CONSOLE_NONE': ('done', 'NDF_CONSOLE = NDF_CONSOLE_NONE'),
    'ESP_CONSOLE_UART_DEFAULT': ('planned', 'UART console'), 'ESP_CONSOLE_UART_CUSTOM': ('planned', 'UART console'),
    'ESP_MAIN_TASK_STACK_SIZE': ('done', 'STACK0_SIZE (app.mk)'), 'ESP_INT_WDT': ('planned', 'watchdogs (TODO 3); NDF_BOOT_WDT_OFF'),
    'ESP_TASK_WDT_EN': ('planned', 'watchdogs (TODO 3)'), 'ESP_BROWNOUT_DET': ('planned', 'brownout (TODO 3)'),
    'ESP_SYSTEM_PANIC_PRINT_HALT': ('done', 'ndf_fault() prints and halts'), 'ESP_SYSTEM_PANIC_PRINT_REBOOT': ('planned', 'fault -> restart option'),
    'ESP32S3_REV_MIN_0': ('done', 'any revision; sys_info() reports it'),
    'NVS_ENCRYPTION': ('done', 'nvs_mount_secure() (keys partition); HMAC scheme planned'),
    'FATFS_LFN_HEAP': ('done', 'long names always on, no heap'), 'FATFS_LFN_STACK': ('done', 'long names always on'),
    'FATFS_CODEPAGE_437': ('done', 'ASCII/UTF-16 names'), 'WL_SECTOR_SIZE_4096': ('done', 'wl.c (4 KB, IDF v2 format)'),
    'WL_SECTOR_SIZE_512': ('planned', '512-byte WL sectors'), 'SPIFFS_PAGE_SIZE': ('done', '256 (IDF default format)'),
    'SPIFFS_OBJ_NAME_LEN': ('done', '32 (IDF default format)'), 'SPIFFS_USE_MAGIC': ('done', 'always (IDF default)'),
    'SPIFFS_USE_MAGIC_LENGTH': ('done', 'always (IDF default)'), 'SPIFFS_META_LENGTH': ('done', '4 (IDF default)'),
    'SPI_FLASH_ROM_DRIVER_PATCH': ('n/a', 'no ROM calls'), 'SPI_FLASH_ENABLE_ENCRYPTED_READ_WRITE': ('planned', 'flash encryption'),
    'LOG_DEFAULT_LEVEL': ('planned', 'logging (TODO 8)'), 'COMPILER_OPTIMIZATION_PERF': ('done', '-O2 always'),
    'COMPILER_OPTIMIZATION_SIZE': ('planned', 'OPT=size build option'), 'ESP_TIMER_TASK_STACK_SIZE': ('n/a', 'no timer task'),
    'SPIRAM': ('planned', 'PSRAM (TODO 2)'), 'ULP_COPROC_ENABLED': ('planned', 'ULP (TODO 4)'), 'PM_ENABLE': ('planned', 'power management (TODO 4)'),
    'SECURE_BOOT': ('planned', 'irreversible eFuse; plan only (STORAGE_SECURITY.md)'),
    'SECURE_FLASH_ENC_ENABLED': ('planned', 'irreversible eFuse; plan only'),
    'BOOTLOADER_LOG_LEVEL': ('n/a', 'no second-stage bootloader (ROM loads the image)'),
    'APP_ROLLBACK_ENABLE': ('planned', 'OTA (TODO 3)'), 'ESP_COREDUMP_ENABLE': ('planned', 'crash record (TODO 3)'),
}


# name patterns -> (status, note), checked after OPTION_MAP and before the component default
PATTERNS = [
    (r'(ISR_IRAM_SAFE|_IN_IRAM|IRAM_SAFE|_PLACE_.*IRAM|_IRAM_OPT|CTRL_FUNC_IN_IRAM|ISR_CACHE_SAFE)',
     ('done', 'always: the whole esp-ndf image runs from internal RAM (no cache-disable windows)')),
    (r'(SUPPRESS_DEPRECATE_WARN|ENABLE_DEBUG_LOG|_DEBUG_LOG$)', ('n/a', 'IDF logging/deprecation switches')),
    (r'^ESPTOOLPY_(BEFORE|AFTER|MONITOR|NO_STUB|FLASH_MODE_AUTO_DETECT|HEADER_FLASHSIZE_UPDATE)',
     ('done', 'make flash / PORT=; esptool called directly')),
    (r'^ESP_ROM_', ('n/a', 'no ROM functions are called')),
]


def parse(path):
    """yield dicts: name, type, prompt, menu path, depends, file"""
    menus, cur = [], None
    in_help, help_indent = False, None
    with open(path, errors='replace') as f:
        lines = f.read().splitlines()
    for raw in lines:
        line = raw.strip()
        if in_help:                            # help ends where indentation drops below the help text's
            if not line:
                continue
            indent = len(raw) - len(raw.lstrip())
            if help_indent is None:
                help_indent = indent
            if indent >= help_indent:
                if cur is not None and len(cur['help']) < 300:
                    cur['help'] += (' ' if cur['help'] else '') + line
                continue
            in_help = False
        if not line or line.startswith('#'):
            continue
        m = re.match(r'^(menu|menuconfig)\s+"?([^"]*)"?', line)
        if line.startswith('menu ') or line.startswith('menu"'):
            menus.append(re.match(r'menu\s+"([^"]*)"', line).group(1) if '"' in line else line[5:])
            cur = None
            continue
        if line == 'endmenu':
            if menus:
                menus.pop()
            cur = None
            continue
        m = re.match(r'^(config|menuconfig)\s+(\w+)', line)
        if m:
            cur = {'name': m.group(2), 'type': '', 'prompt': '', 'menu': ' > '.join(menus), 'depends': '', 'help': '',
                   'file': path}
            yield cur
            continue
        if cur is None:
            continue
        m = re.match(r'^(bool|int|hex|string|tristate)\s*(?:"([^"]*)")?', line)
        if m:
            cur['type'] = m.group(1)
            if m.group(2):
                cur['prompt'] = m.group(2)
            continue
        m = re.match(r'^prompt\s+"([^"]*)"', line)
        if m:
            cur['prompt'] = m.group(1)
            continue
        m = re.match(r'^depends on\s+(.*)', line)
        if m:
            dep = m.group(1).split('#')[0].strip()
            cur['depends'] += (' && ' if cur['depends'] else '') + dep
            continue
        if line in ('help', '---help---'):
            in_help, help_indent = True, None
            cur['help'] = ''
            continue


OTHER_TARGET = re.compile(r'IDF_TARGET_(ESP32(?!S3\b)\w*|LINUX)\b')


def relevant(o):
    if not o['prompt'] or o['name'].startswith('SOC_'):
        return False
    d = o['depends']
    if OTHER_TARGET.search(d) and 'IDF_TARGET_ESP32S3' not in d:
        return False
    if re.search(r'!\s*IDF_TARGET_ESP32S3\b', d):
        return False
    return True


def component_of(path, idf):
    rel = os.path.relpath(path, os.path.join(idf, 'components'))
    return rel.split(os.sep)[0] if not rel.startswith('..') else os.path.basename(os.path.dirname(path))


def main():
    idf = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else 'docs/IDF_CONFIG_INVENTORY.md'
    files = []
    for root, _, names in os.walk(os.path.join(idf, 'components')):
        files += [os.path.join(root, n) for n in names if n.startswith('Kconfig') and 'test' not in root]
    files += [os.path.join(idf, n) for n in ('Kconfig',) if os.path.exists(os.path.join(idf, n))]
    seen, opts = set(), []
    for p in sorted(files):
        for o in list(parse(p)):                 # parse the whole file first: entries fill in after "config"
            o['component'] = component_of(p, idf)
            if relevant(o) and o['name'] not in seen:
                seen.add(o['name'])
                opts.append(o)
    by_comp = collections.defaultdict(list)
    counts = collections.Counter()
    for o in opts:
        st, note = OPTION_MAP.get(o['name'], (None, None))
        if st is None:
            for pat, res in PATTERNS:
                if re.search(pat, o['name']):
                    st, note = res
                    break
        if st is None:
            st, note = COMPONENT_STATUS.get(o['component'], ('planned', ''))
            note = note or ''
        o['status'], o['ndf'] = st, note
        by_comp[o['component']].append(o)
        counts[st] += 1
    total = len(opts)
    with open(out, 'w') as f:
        f.write('# ESP-IDF menuconfig options vs esp-ndf\n\n')
        f.write('Generated by `tools/idf_kconfig_inventory.py` from %s (ESP-IDF v5.3.5): every option a user can set\n'
                'for the ESP32-S3. It excludes hidden symbols, SOC_* capabilities and options for other chips.\n\n' % 'the local IDF tree')
        f.write('| Status | Options | Meaning |\n|---|---|---|\n')
        meaning = {'done': 'esp-ndf has an equivalent (named in the table)', 'planned': 'on the TODO list (docs/TODO.md)',
                   'wireless later': 'radio / network stacks, after peripherals (rule 1)', 'n/a': 'not applicable by design or no such hardware on S3'}
        for st in ('done', 'planned', 'wireless later', 'n/a'):
            f.write('| %s | %d | %s |\n' % (st, counts[st], meaning[st]))
        f.write('| **total** | **%d** | |\n\n' % total)
        f.write('## By component\n\n| Component | Options | done | planned | wireless later | n/a |\n|---|---|---|---|---|---|\n')
        for comp in sorted(by_comp, key=lambda c: -len(by_comp[c])):
            c = collections.Counter(o['status'] for o in by_comp[comp])
            f.write('| %s | %d | %d | %d | %d | %d |\n' % (comp, len(by_comp[comp]), c['done'], c['planned'], c['wireless later'], c['n/a']))
        f.write('\n## All options\n')
        for comp in sorted(by_comp):
            f.write('\n### %s\n\n| Option | Menu | Type | Status | esp-ndf |\n|---|---|---|---|---|\n' % comp)
            for o in by_comp[comp]:
                f.write('| `%s` %s | %s | %s | %s | %s |\n' % (o['name'], o['prompt'].replace('|', '/'), o['menu'].replace('|', '/'),
                                                          o['type'], o['status'], o['ndf'].replace('|', '/')))
    print('%d options -> %s' % (total, out), dict(counts))


if __name__ == '__main__':
    main()
