/* ble_gatts with advertising kept up while a link slot is free: two centrals connect to the same server at once and each gets the notifications it subscribed to. */
#define NDF_GATTS_MULTI 1
#include "../ble_gatts/main.c"
