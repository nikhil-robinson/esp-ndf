/* sys_test needs the panic path to restart (it checks the report, then the next boot) */
#pragma once
#define NDF_PANIC_RESTART 1
