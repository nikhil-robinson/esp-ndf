/* Event Task Matrix (ESP32-P4 only, TRM chapter 14): 50 channels, each wires one peripheral event to one peripheral task in
 * hardware (no CPU in the path). Event and task ids: soc/esp32p4/regs/etm_src.h (ETM_GPIO_EVT_CH0_RISE_EDGE, ETM_GPTIMER_TASK_...,
 * ETM_GPIO_EVT_ZERO_DET_POS0 for the comparators, ...). Configuring the peripheral side of an event / task is the peripheral's. */
#pragma once
#include "ndf.h"
#include "regs/etm_src.h"

#define ETM_CHANNELS 50
int  etm_init(void);
int  etm_connect(int ch, int event, int task);       /* enables the channel */
void etm_disconnect(int ch);
bool etm_enabled(int ch);
