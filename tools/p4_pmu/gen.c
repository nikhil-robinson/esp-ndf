/* Host generator for components/soc/esp32p4/pmu_img.h.
 * Runs ESP-IDF v6.1's own PMU code (esp_hw_support/port/esp32p4 pmu_param.c defaults, the init and sleep-config
 * sequences of pmu_init.c / pmu_sleep.c, hal pmu_ll.h) against two fake PMU register blocks (all zeros, all ones); a bit
 * that is the same in both images was written by the sequence, so each image becomes (offset, mask, value) triples.
 * Build / run: tools/p4_pmu/gen.sh  (needs the IDF tree at ~/esp-idf-v6.1; the output is committed). */
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "esp_private/esp_pmu.h"
#include "soc/rtc.h"
#include "pmu_param.h"
#include "hal/pmu_ll.h"
#include "hal/pmu_hal.h"

pmu_dev_t PMU_HOST;
#include "soc/efuse_struct.h"
efuse_dev_t EFUSE;                                    /* pmu_param.c reads trims from it: block version 0 = the defaults */
uint32_t efuse_hal_blk_version(void) { return 0; }
int esp_rom_printf(const char *f, ...) { return 0; }
#define HP(s) (PMU_MODE_HP_ ## s)
#define LP(s) (PMU_MODE_LP_ ## s)

/* flags: the IDF deep / light sleep choice of this port (docs/P4.md): XTAL, RC_FAST off in deep sleep; the HP top
 * domain and CPU off in deep sleep; LP peripherals, RTC memory and the flash supply stay on in both */
#define F_DEEP  (PMU_SLEEP_PD_TOP | PMU_SLEEP_PD_CNNT | PMU_SLEEP_PD_XTAL | PMU_SLEEP_PD_RC_FAST | PMU_SLEEP_PD_CPU | PMU_SLEEP_PD_RC32K | PMU_SLEEP_PD_XTAL32K)
#define F_LIGHT 0u
#define RC_SLOW_HZ 136000u
#define US2SLOW(us) ((uint32_t)((uint64_t)(us) * RC_SLOW_HZ / 1000000u))
#define XTAL_FAST_HZ 40000000u
#define US2FAST(us) ((uint32_t)((uint64_t)(us) * XTAL_FAST_HZ / 1000000u))

static void hp_mode_init(pmu_dev_t *d, pmu_hp_mode_t mode)
{
    const pmu_hp_system_power_param_t *power = pmu_hp_system_power_param_default(mode);
    const pmu_hp_system_clock_param_t *clock = pmu_hp_system_clock_param_default(mode);
    const pmu_hp_system_digital_param_t *dig = pmu_hp_system_digital_param_default(mode);
    const pmu_hp_system_analog_param_t *anlg = pmu_hp_system_analog_param_default(mode);
    const pmu_hp_system_retention_param_t *ret = pmu_hp_system_retention_param_default(mode);
    pmu_ll_hp_set_dig_power(d, mode, power->dig_power.val);
    pmu_ll_hp_set_clk_power(d, mode, power->clk_power.val);
    pmu_ll_hp_set_xtal_xpd(d, mode, power->xtal.xpd_xtal);
    pmu_ll_hp_set_icg_func(d, mode, clock->icg_func);
    pmu_ll_hp_set_icg_apb(d, mode, clock->icg_apb);
    pmu_ll_hp_set_sysclk_nodiv(d, mode, clock->sysclk.dig_sysclk_nodiv);
    pmu_ll_hp_set_icg_sysclk_enable(d, mode, clock->sysclk.icg_sysclk_en);
    pmu_ll_hp_set_sysclk_slp_sel(d, mode, clock->sysclk.sysclk_slp_sel);
    pmu_ll_hp_set_icg_sysclk_slp_sel(d, mode, clock->sysclk.icg_slp_sel);
    pmu_ll_hp_set_dig_sysclk(d, mode, clock->sysclk.dig_sysclk_sel);
    pmu_ll_hp_set_uart_wakeup_enable(d, mode, dig->syscntl.uart_wakeup_en);
    pmu_ll_hp_set_hold_all_lp_pad(d, mode, dig->syscntl.lp_pad_hold_all);
    pmu_ll_hp_set_hold_all_hp_pad(d, mode, dig->syscntl.hp_pad_hold_all);
    pmu_ll_hp_set_dig_pad_slp_sel(d, mode, dig->syscntl.dig_pad_slp_sel);
    pmu_ll_hp_set_pause_watchdog(d, mode, dig->syscntl.dig_pause_wdt);
    pmu_ll_hp_set_cpu_stall(d, mode, dig->syscntl.dig_cpu_stall);
    pmu_ll_hp_set_bias_xpd(d, mode, anlg->bias.xpd_bias);
    pmu_ll_hp_set_dcm_mode(d, mode, anlg->bias.dcm_mode);
    pmu_ll_hp_set_dcm_vset(d, mode, anlg->bias.dcm_vset);
    pmu_ll_hp_set_dbg_atten(d, mode, anlg->bias.dbg_atten);
    pmu_ll_hp_set_current_power_off(d, mode, anlg->bias.pd_cur);
    pmu_ll_hp_set_bias_sleep_enable(d, mode, anlg->bias.bias_sleep);
    pmu_ll_hp_set_regulator_sleep_memory_xpd(d, mode, anlg->regulator0.slp_mem_xpd);
    pmu_ll_hp_set_regulator_sleep_logic_xpd(d, mode, anlg->regulator0.slp_logic_xpd);
    if (mode == PMU_MODE_HP_SLEEP) pmu_ll_hp_enable_sleep_flash_ldo_channel(d, anlg->regulator0.xpd_0p1a);
    pmu_ll_hp_set_regulator_sleep_logic_dbias(d, mode, anlg->regulator0.slp_logic_dbias);
    pmu_ll_hp_set_regulator_driver_bar(d, mode, anlg->regulator1.drv_b);
    pmu_ll_hp_set_retention_param(d, mode, ret->retention.val);
    pmu_ll_hp_set_backup_icg_func(d, mode, ret->backup_clk);
    pmu_ll_hp_set_sleep_protect_mode(d, PMU_SLEEP_PROTECT_HP_LP_SLEEP);
}

static void lp_mode_init(pmu_dev_t *d, pmu_lp_mode_t mode)
{
    const pmu_lp_system_power_param_t *power = pmu_lp_system_power_param_default(mode);
    const pmu_lp_system_analog_param_t *anlg = pmu_lp_system_analog_param_default(mode);
    pmu_ll_lp_set_dig_power(d, mode, power->dig_power.val);
    pmu_ll_lp_set_clk_power(d, mode, power->clk_power.val);
    pmu_ll_lp_set_xtal_xpd(d, PMU_MODE_LP_SLEEP, power->xtal.xpd_xtal);
    pmu_ll_lp_set_bias_xpd(d, PMU_MODE_LP_SLEEP, anlg->bias.xpd_bias);
    pmu_ll_lp_set_dbg_atten(d, PMU_MODE_LP_SLEEP, anlg->bias.dbg_atten);
    pmu_ll_lp_set_current_power_off(d, PMU_MODE_LP_SLEEP, anlg->bias.pd_cur);
    pmu_ll_lp_set_bias_sleep_enable(d, PMU_MODE_LP_SLEEP, anlg->bias.bias_sleep);
    pmu_ll_lp_set_regulator_slp_xpd(d, mode, anlg->regulator0.slp_xpd);
    pmu_ll_lp_set_regulator_sleep_dbias(d, mode, anlg->regulator0.slp_dbias);
    pmu_ll_lp_set_regulator_driver_bar(d, mode, anlg->regulator1.drv_b);
}

static void sleep_cfg(pmu_dev_t *d, uint32_t flags, bool dslp)
{
    pmu_sleep_power_config_t pw = PMU_SLEEP_POWER_CONFIG_DEFAULT(flags);
    pw.hp_sys.dig_power.cpu_pd_en = (flags & PMU_SLEEP_PD_CPU) ? 1 : 0;
    pmu_sleep_digital_config_t dg;
    pmu_sleep_analog_config_t an;
    if (dslp) {
        pmu_sleep_digital_config_t t = PMU_SLEEP_DIGITAL_DSLP_CONFIG_DEFAULT(flags, 0); dg = t;
        pmu_sleep_analog_config_t a = PMU_SLEEP_ANALOG_DSLP_CONFIG_DEFAULT(flags); an = a;
        an.hp_sys.analog.xpd_0p1a = 1;
    } else {
        pmu_sleep_digital_config_t t = PMU_SLEEP_DIGITAL_LSLP_CONFIG_DEFAULT(flags, 0); dg = t;
        pmu_sleep_analog_config_t a = PMU_SLEEP_ANALOG_LSLP_CONFIG_DEFAULT(flags); an = a;
        an.hp_sys.analog.pd_cur = PMU_PD_CUR_SLEEP_ON; an.hp_sys.analog.bias_sleep = PMU_BIASSLP_SLEEP_ON;
        an.hp_sys.analog.dbg_atten = PMU_DBG_ATTEN_ACTIVE_DEFAULT; an.hp_sys.analog.dbias = HP_CALI_ACTIVE_DBIAS_DEFAULT;
        an.lp_sys[LP(SLEEP)].analog.pd_cur = PMU_PD_CUR_SLEEP_ON; an.lp_sys[LP(SLEEP)].analog.bias_sleep = PMU_BIASSLP_SLEEP_ON;
        an.lp_sys[LP(SLEEP)].analog.dbg_atten = PMU_DBG_ATTEN_ACTIVE_DEFAULT;
        pw.hp_sys.dig_power.dcdc_switch_pd_en = 0;
        an.hp_sys.analog.dcm_vset = 27;                       /* CONFIG_ESP_SLEEP_DCM_VSET_VAL_IN_SLEEP default */
        an.hp_sys.analog.xpd_0p1a = 1;
    }
    pmu_sleep_param_config_t pa = PMU_SLEEP_PARAM_CONFIG_DEFAULT(flags);
    /* timings from the machine constants (PMU_SLEEP_MC_DEFAULT) on the nominal 136 kHz RC_SLOW / 40 MHz XTAL clocks */
    pmu_sleep_machine_constant_t mc = PMU_SLEEP_MC_DEFAULT();
    pa.hp_sys.min_slp_slow_clk_cycle = US2SLOW(mc.hp.min_slp_time_us);
    pa.hp_sys.analog_wait_target_cycle = US2SLOW(mc.hp.analog_wait_time_us);
    pa.hp_sys.digital_power_supply_wait_cycle = US2FAST(mc.hp.power_supply_wait_time_us);
    pa.hp_sys.digital_power_up_wait_cycle = US2FAST(mc.hp.power_up_wait_time_us);
    pa.hp_sys.pll_stable_wait_cycle = US2FAST(mc.hp.pll_wait_stable_time_us);
    pa.lp_sys.min_slp_slow_clk_cycle = US2SLOW(mc.lp.min_slp_time_us);
    pa.lp_sys.analog_wait_target_cycle = dslp ? US2SLOW(PMU_LP_ANALOG_WAIT_TARGET_TIME_DSLP_US) : US2SLOW(mc.lp.analog_wait_time_us);
    pa.lp_sys.digital_power_supply_wait_cycle = US2FAST(mc.lp.power_supply_wait_time_us);
    pa.lp_sys.digital_power_up_wait_cycle = US2FAST(mc.lp.power_up_wait_time_us);
    if (pw.hp_sys.xtal.xpd_xtal) pa.hp_lp.xtal_stable_wait_slow_clk_cycle = US2SLOW(mc.lp.xtal_wait_stable_time_us);
    else pa.hp_lp.xtal_stable_wait_cycle = US2FAST(mc.hp.xtal_wait_stable_time_us);

    pmu_ll_hp_set_dig_power(d, HP(SLEEP), pw.hp_sys.dig_power.val);
    pmu_ll_hp_set_clk_power(d, HP(SLEEP), pw.hp_sys.clk_power.val);
    pmu_ll_hp_set_xtal_xpd(d, HP(SLEEP), pw.hp_sys.xtal.xpd_xtal);
    pmu_ll_lp_set_dig_power(d, LP(ACTIVE), pw.lp_sys[LP(ACTIVE)].dig_power.val);
    pmu_ll_lp_set_clk_power(d, LP(ACTIVE), pw.lp_sys[LP(ACTIVE)].clk_power.val);
    pmu_ll_lp_set_dig_power(d, LP(SLEEP), pw.lp_sys[LP(SLEEP)].dig_power.val);
    pmu_ll_lp_set_clk_power(d, LP(SLEEP), pw.lp_sys[LP(SLEEP)].clk_power.val);
    pmu_ll_lp_set_xtal_xpd(d, LP(SLEEP), pw.lp_sys[LP(SLEEP)].xtal.xpd_xtal);
    pmu_ll_hp_set_icg_sysclk_enable(d, HP(SLEEP), dg.icg_func[0] != 0);
    pmu_ll_hp_set_icg_func(d, HP(SLEEP), dg.icg_func[0]);
    pmu_ll_hp_set_dig_pad_slp_sel(d, HP(SLEEP), dg.syscntl.dig_pad_slp_sel);
    pmu_ll_hp_set_hold_all_lp_pad(d, HP(SLEEP), dg.syscntl.lp_pad_hold_all);
    pmu_ll_hp_set_pause_watchdog(d, HP(SLEEP), dg.syscntl.dig_pause_wdt);
    pmu_ll_hp_set_dcm_mode(d, HP(ACTIVE), dslp ? 0 : 1);
    pmu_ll_hp_set_dcm_mode(d, HP(SLEEP), an.hp_sys.analog.dcm_mode);
    pmu_ll_hp_set_dcm_vset(d, HP(SLEEP), an.hp_sys.analog.dcm_vset);
    pmu_ll_hp_set_current_power_off(d, HP(SLEEP), an.hp_sys.analog.pd_cur);
    pmu_ll_hp_set_bias_sleep_enable(d, HP(SLEEP), an.hp_sys.analog.bias_sleep);
    pmu_ll_hp_set_regulator_sleep_memory_xpd(d, HP(SLEEP), an.hp_sys.analog.slp_mem_xpd);
    pmu_ll_hp_set_regulator_sleep_logic_xpd(d, HP(SLEEP), an.hp_sys.analog.slp_logic_xpd);
    pmu_ll_hp_set_regulator_xpd(d, HP(SLEEP), an.hp_sys.analog.xpd);
    pmu_ll_hp_enable_sleep_flash_ldo_channel(d, an.hp_sys.analog.xpd_0p1a);
    pmu_ll_hp_set_regulator_sleep_logic_dbias(d, HP(SLEEP), an.hp_sys.analog.slp_logic_dbias);
    pmu_ll_hp_set_dbg_atten(d, HP(SLEEP), an.hp_sys.analog.dbg_atten);
    pmu_ll_hp_set_regulator_dbias(d, HP(SLEEP), an.hp_sys.analog.dbias);
    pmu_ll_hp_set_regulator_driver_bar(d, HP(SLEEP), an.hp_sys.analog.drv_b);
    pmu_ll_lp_set_regulator_sleep_dbias(d, LP(ACTIVE), an.lp_sys[LP(ACTIVE)].analog.slp_dbias);
    pmu_ll_lp_set_regulator_dbias(d, LP(ACTIVE), an.lp_sys[LP(ACTIVE)].analog.dbias);
    pmu_ll_lp_set_regulator_driver_bar(d, LP(ACTIVE), an.lp_sys[LP(ACTIVE)].analog.drv_b);
    pmu_ll_lp_set_current_power_off(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.pd_cur);
    pmu_ll_lp_set_bias_sleep_enable(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.bias_sleep);
    pmu_ll_lp_set_regulator_xpd(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.xpd);
    pmu_ll_lp_set_regulator_slp_xpd(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.slp_xpd);
    pmu_ll_lp_set_regulator_sleep_dbias(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.slp_dbias);
    pmu_ll_lp_set_regulator_dbias(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.dbias);
    pmu_ll_lp_set_dbg_atten(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.dbg_atten);
    pmu_ll_lp_set_regulator_driver_bar(d, LP(SLEEP), an.lp_sys[LP(SLEEP)].analog.drv_b);
    pmu_ll_lp_set_regulator_slp_xpd(d, LP(ACTIVE), an.lp_sys[LP(ACTIVE)].analog.slp_xpd);
    pmu_ll_lp_set_regulator_xpd(d, LP(ACTIVE), an.lp_sys[LP(ACTIVE)].analog.xpd);
    pmu_ll_hp_set_min_sleep_cycle(d, pa.hp_sys.min_slp_slow_clk_cycle);
    pmu_ll_lp_set_min_sleep_cycle(d, pa.lp_sys.min_slp_slow_clk_cycle);
    pmu_ll_hp_set_analog_wait_target_cycle(d, pa.hp_sys.analog_wait_target_cycle);
    pmu_ll_lp_set_analog_wait_target_cycle(d, pa.lp_sys.analog_wait_target_cycle);
    pmu_ll_hp_set_digital_power_supply_wait_cycle(d, pa.hp_sys.digital_power_supply_wait_cycle);
    pmu_ll_hp_set_digital_power_up_wait_cycle(d, pa.hp_sys.digital_power_up_wait_cycle);
    pmu_ll_lp_set_digital_power_supply_wait_cycle(d, pa.lp_sys.digital_power_supply_wait_cycle);
    pmu_ll_lp_set_digital_power_up_wait_cycle(d, pa.lp_sys.digital_power_up_wait_cycle);
    pmu_ll_set_xtal_stable_wait_cycle(d, pa.hp_lp.xtal_stable_wait_slow_clk_cycle);
    pmu_ll_set_pll_stable_wait_cycle(d, pa.hp_sys.pll_stable_wait_cycle);
}

static void run(pmu_dev_t *d, uint32_t flags, bool dslp)
{
    hp_mode_init(d, HP(ACTIVE));
    hp_mode_init(d, HP(SLEEP));
    for (int m = LP(ACTIVE); m < PMU_MODE_LP_MAX; m++) lp_mode_init(d, (pmu_lp_mode_t)m);
    sleep_cfg(d, flags, dslp);
}

static void emit(const char *name, uint32_t flags, bool dslp)
{
    uint32_t *a = calloc(1, sizeof(pmu_dev_t)), *b = malloc(sizeof(pmu_dev_t));
    memset(b, 0xFF, sizeof(pmu_dev_t));
    run((pmu_dev_t *)a, flags, dslp);
    run((pmu_dev_t *)b, flags, dslp);
    size_t skip0 = offsetof(pmu_dev_t, hp_sys[HP(ACTIVE)]), skip1 = skip0 + sizeof(PMU_HOST.hp_sys[0]);
    printf("static const pmu_img_t %s[] = {\n", name);
    for (size_t o = 0; o < sizeof(pmu_dev_t); o += 4) {
        if (o >= skip0 && o < skip1) continue;
        uint32_t same = ~(a[o / 4] ^ b[o / 4]);
        if (same) printf("    { 0x%03zx, 0x%08x, 0x%08x },\n", o, same, a[o / 4] & same);
    }
    printf("};\n");
}

#define P(T, name, field) do { T v; memset(&v, 0, sizeof v); v.field = -1; printf("#define %-28s 0x%08x\n", name, v.val); } while (0)
int main(void)
{
    printf("/* generated by tools/p4_pmu/gen.c from ESP-IDF v6.1 (pmu_param.c, pmu_init.c, pmu_sleep.c, pmu_ll.h): do not edit.\n"
           " * Offsets are from PMU_BASE; a word is applied as reg = (reg & ~mask) | value. HP ACTIVE-mode words are left to clk.c. */\n"
           "typedef struct { uint16_t off; uint32_t mask, val; } pmu_img_t;\n");
    emit("pmu_img_deep", F_DEEP, true);
    emit("pmu_img_light", F_LIGHT, false);
    printf("/* HP ACTIVE-mode DCM mode (set by the sleep config, not part of the images) */\n");
    printf("#define PMU_HP_ACT_BIAS_OFF       0x%zx\n", offsetof(pmu_dev_t, hp_sys[HP(ACTIVE)].bias));
    P(pmu_hp_bias_reg_t, "PMU_HP_ACT_DCM_MODE_MASK", dcm_mode);
    printf("/* sleep control */\n");
    printf("#define PMU_WAKEUP_CNTL0_OFF      0x%zx\n#define PMU_WAKEUP_CNTL1_OFF      0x%zx\n#define PMU_WAKEUP_CNTL2_OFF      0x%zx\n#define PMU_WAKEUP_CNTL3_OFF      0x%zx\n#define PMU_WAKEUP_CNTL4_OFF      0x%zx\n",
           offsetof(pmu_dev_t, wakeup.cntl0), offsetof(pmu_dev_t, wakeup.cntl1), offsetof(pmu_dev_t, wakeup.cntl2), offsetof(pmu_dev_t, wakeup.cntl3), offsetof(pmu_dev_t, wakeup.cntl4));
    printf("#define PMU_WAKEUP_STATUS0_OFF    0x%zx\n#define PMU_WAKEUP_STATUS1_OFF    0x%zx\n", offsetof(pmu_dev_t, wakeup.status0), offsetof(pmu_dev_t, wakeup.status1));
    printf("#define PMU_INT_RAW_OFF           0x%zx\n#define PMU_INT_CLR_OFF           0x%zx\n", offsetof(pmu_dev_t, hp_ext.int_raw), offsetof(pmu_dev_t, hp_ext.int_clr));
    P(pmu_slp_wakeup_cntl0_reg_t, "PMU_SLEEP_REQ", sleep_req);
    P(pmu_slp_wakeup_cntl1_reg_t, "PMU_SLP_REJECT_EN", slp_reject_en);
    P(pmu_slp_wakeup_cntl4_reg_t, "PMU_REJECT_CAUSE_CLR", slp_reject_cause_clr);
    P(pmu_hp_intr_reg_t, "PMU_INT_RAW_WAKEUP", wakeup);
    P(pmu_hp_intr_reg_t, "PMU_INT_RAW_REJECT", reject);
    P(pmu_hp_intr_reg_t, "PMU_INT_CLR_WAKEUP", wakeup);
    P(pmu_hp_intr_reg_t, "PMU_INT_CLR_REJECT", reject);
    return 0;
}
