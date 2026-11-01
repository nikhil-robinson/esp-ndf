/* BLE commands of the peer (NimBLE): advertise, connect as central, GATT client (discover / read / write / subscribe), and a
 * built-in GATT server (service 0xFFF0: 0xFFF1 read/write 16 B, 0xFFF2 notify counter). Addresses are printed MSB first. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "../peer.h"

static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static volatile uint32_t nnotif, last_nlen, sub_state, srv_counter, last_val, ngaps; static volatile int have_prev;
static uint16_t notify_handle;
static volatile int64_t burst_until_us;
static volatile uint32_t scan_seen, scan_match; static char scan_filter[24];
static uint8_t srv_val[16];
static SemaphoreHandle_t sem;
static int started, synced;
static uint8_t own_type;
static volatile int op_status;

static int gap_ev(struct ble_gap_event *e, void *a)
{
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status == 0) { conn = e->connect.conn_handle; printf("event connected %d\n", conn); }
        else { conn = BLE_HS_CONN_HANDLE_NONE; printf("event connect-failed %d\n", e->connect.status); }
        xSemaphoreGive(sem);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        conn = BLE_HS_CONN_HANDLE_NONE; sub_state = 0; printf("event disconnect %d\n", e->disconnect.reason);
        break;
    case BLE_GAP_EVENT_NOTIFY_RX: {
        nnotif++; last_nlen = OS_MBUF_PKTLEN(e->notify_rx.om);
        uint32_t v = 0; uint16_t got = 0;
        if (last_nlen >= 4 && !ble_hs_mbuf_to_flat(e->notify_rx.om, &v, 4, &got)) { if (have_prev && v != last_val + 1) ngaps++; last_val = v; have_prev = 1; }
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE: if (e->subscribe.attr_handle == notify_handle) sub_state = e->subscribe.cur_notify; break;
    case BLE_GAP_EVENT_DISC: {
        struct ble_hs_adv_fields f; ble_hs_adv_parse_fields(&f, e->disc.data, e->disc.length_data);
        char nm[32] = ""; if (f.name) { int k = f.name_len < 31 ? f.name_len : 31; memcpy(nm, f.name, k); nm[k] = 0; }
        scan_seen++;
        if (scan_filter[0] && !strstr(nm, scan_filter)) break;
        scan_match++;
        if (scan_match <= 3) printf("adv %02x:%02x:%02x:%02x:%02x:%02x type %d rssi %d name %s\n", e->disc.addr.val[5], e->disc.addr.val[4], e->disc.addr.val[3], e->disc.addr.val[2], e->disc.addr.val[1], e->disc.addr.val[0], e->disc.event_type, e->disc.rssi, nm);
        break;
    }
    case BLE_GAP_EVENT_MTU: printf("event mtu %d\n", e->mtu.value); break;
    default: break;
    }
    return 0;
}

static int chr_acc(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    if (ctx->op == BLE_GATT_ACCESS_OP_READ_CHR) return os_mbuf_append(ctx->om, srv_val, sizeof srv_val) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    if (ctx->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t n = OS_MBUF_PKTLEN(ctx->om); if (n > sizeof srv_val) n = sizeof srv_val;
        int rc = ble_hs_mbuf_to_flat(ctx->om, srv_val, n, &n);
        if (!rc && n && srv_val[0] == 0xB5) burst_until_us = esp_timer_get_time() + 5000000;   /* 5 s of notifications as fast as the stack takes them */
        return rc;
    }
    return BLE_ATT_ERR_UNLIKELY;
}
static int cnt_acc(uint16_t ch, uint16_t ah, struct ble_gatt_access_ctxt *ctx, void *arg)
{
    return os_mbuf_append(ctx->om, &srv_counter, 4) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
}
static const struct ble_gatt_svc_def svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0xFFF0),
      .characteristics = (struct ble_gatt_chr_def[]){
          { .uuid = BLE_UUID16_DECLARE(0xFFF1), .access_cb = chr_acc, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE },
          { .uuid = BLE_UUID16_DECLARE(0xFFF2), .access_cb = cnt_acc, .val_handle = &notify_handle, .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY },
          { 0 } } },
    { 0 } };

static void notify_task(void *a)
{
    for (;;) {
        int burst = esp_timer_get_time() < burst_until_us;
        vTaskDelay(burst ? 1 : pdMS_TO_TICKS(100));
        if (conn == BLE_HS_CONN_HANDLE_NONE || !sub_state) continue;
        uint32_t next = srv_counter + 1;                       /* the value advances only when the stack took the notification */
        struct os_mbuf *om = ble_hs_mbuf_from_flat(&next, 4);
        if (om && ble_gatts_notify_custom(conn, notify_handle, om) == 0) srv_counter = next;
    }
}

static void on_sync(void) { ble_hs_id_infer_auto(0, &own_type); synced = 1; xSemaphoreGive(sem); }
static void host_task(void *p) { nimble_port_run(); nimble_port_freertos_deinit(); }

static int ble_start(void)
{
    if (started) return 0;
    sem = xSemaphoreCreateBinary();
    if (nimble_port_init()) return -1;
    ble_hs_cfg.sync_cb = on_sync;
    ble_svc_gap_init(); ble_svc_gatt_init();
    ble_gatts_count_cfg(svcs); ble_gatts_add_svcs(svcs);
    ble_svc_gap_device_name_set("PEER");
    nimble_port_freertos_init(host_task);
    xTaskCreate(notify_task, "notify", 3072, 0, 4, 0);
    started = 1;
    return xSemaphoreTake(sem, pdMS_TO_TICKS(5000)) ? 0 : -2;
}

static int cmd_adv(int argc, char **argv)
{
    if (ble_start() || argc < 2) return -1;
    ble_gap_adv_stop();
    ble_svc_gap_device_name_set(argv[1]);
    struct ble_hs_adv_fields f = { 0 };
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (uint8_t *)argv[1]; f.name_len = strlen(argv[1]); f.name_is_complete = 1;
    struct ble_gap_adv_params ap = { .conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN };
    int rc = ble_gap_adv_set_fields(&f);
    if (!rc) rc = ble_gap_adv_start(own_type, NULL, BLE_HS_FOREVER, &ap, gap_ev, NULL);
    return rc;
}
static int cmd_adv_stop(int argc, char **argv) { if (ble_start()) return -1; int rc = ble_gap_adv_stop(); return rc == BLE_HS_EALREADY ? 0 : rc; }

static int parse_addr(const char *s, ble_addr_t *a)             /* aa:bb:cc:dd:ee:ff, printed MSB first */
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return -1;
    for (int i = 0; i < 6; i++) a->val[i] = v[5 - i];
    return 0;
}

static int cmd_connect(int argc, char **argv)                    /* ble_connect <addr> [type 0 public / 1 random] */
{
    if (ble_start() || argc < 2) return -1;
    ble_addr_t a = { .type = argc > 2 ? atoi(argv[2]) : 1 };
    if (parse_addr(argv[1], &a)) return -2;
    ble_gap_disc_cancel();
    int rc = ble_gap_connect(own_type, &a, 10000, NULL, gap_ev, NULL);
    if (rc) return rc;
    if (!xSemaphoreTake(sem, pdMS_TO_TICKS(12000))) return -3;
    return conn == BLE_HS_CONN_HANDLE_NONE ? -4 : 0;
}
static int cmd_scan(int argc, char **argv)                         /* ble_scan <seconds> [name filter] */
{
    if (ble_start() || argc < 2) return -1;
    scan_seen = scan_match = 0; strlcpy(scan_filter, argc > 2 ? argv[2] : "", sizeof scan_filter);
    struct ble_gap_disc_params dp = { .filter_duplicates = 0, .passive = 1 };
    int rc = ble_gap_disc(own_type, atoi(argv[1]) * 1000, &dp, gap_ev, NULL);
    if (rc) return rc;
    vTaskDelay(pdMS_TO_TICKS(atoi(argv[1]) * 1000 + 300));
    printf("scan seen=%u matched=%u\n", (unsigned)scan_seen, (unsigned)scan_match);
    return 0;
}
static int cmd_disconnect(int argc, char **argv)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE) return 0;
    return ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

/* discovery: services first, then per service its characteristics and descriptors; every callback gives the semaphore at EDONE */
static struct { uint16_t s, e; } svc_list[16]; static int nsvc;
static int svc_cb(uint16_t c, const struct ble_gatt_error *er, const struct ble_gatt_svc *s, void *a)
{
    if (er->status == 0 && nsvc < 16) {
        char u[BLE_UUID_STR_LEN];
        printf("svc %u %u %s\n", s->start_handle, s->end_handle, ble_uuid_to_str(&s->uuid.u, u));
        svc_list[nsvc].s = s->start_handle; svc_list[nsvc++].e = s->end_handle;
    } else { op_status = er->status; xSemaphoreGive(sem); }
    return 0;
}
static int dsc_cb(uint16_t c, const struct ble_gatt_error *er, uint16_t ch, const struct ble_gatt_dsc *d, void *a)
{
    if (er->status == 0) { char u[BLE_UUID_STR_LEN]; printf("dsc %u %s\n", d->handle, ble_uuid_to_str(&d->uuid.u, u)); }
    else { op_status = er->status; xSemaphoreGive(sem); }
    return 0;
}
static struct { uint16_t val, end; } chr_list[16]; static int nchr; static uint16_t cur_end;
static int chr_cb(uint16_t c, const struct ble_gatt_error *er, const struct ble_gatt_chr *ch, void *a)
{
    if (er->status == 0 && nchr < 16) {
        char u[BLE_UUID_STR_LEN];
        printf("chr %u %u %s props=%02x\n", ch->def_handle, ch->val_handle, ble_uuid_to_str(&ch->uuid.u, u), ch->properties);
        chr_list[nchr].val = ch->val_handle; chr_list[nchr++].end = 0;
    } else { op_status = er->status; xSemaphoreGive(sem); }
    return 0;
}
static void op_begin(void) { while (xSemaphoreTake(sem, 0) == pdTRUE) {} }              /* drop a stale completion */
static int wait_op(void) { return xSemaphoreTake(sem, pdMS_TO_TICKS(8000)) ? (op_status == BLE_HS_EDONE || op_status == 0 ? 0 : op_status) : -9; }
static int cmd_disc(int argc, char **argv)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    nsvc = 0; op_begin();
    int rc;
    if ((rc = ble_gattc_disc_all_svcs(conn, svc_cb, NULL)) || (rc = wait_op())) { printf("disc svcs rc %d\n", rc); return -2; }   /* rc: NimBLE start error, ATT status, or -9 = no answer in 8 s */
    for (int i = 0; i < nsvc; i++) {
        nchr = 0; cur_end = svc_list[i].e; op_begin();
        if ((rc = ble_gattc_disc_all_chrs(conn, svc_list[i].s, svc_list[i].e, chr_cb, NULL)) || (rc = wait_op())) { printf("disc chrs %u-%u rc %d\n", svc_list[i].s, svc_list[i].e, rc); return -3; }
        for (int k = 0; k < nchr; k++) {
            uint16_t end = k + 1 < nchr ? chr_list[k + 1].val - 2 : cur_end;
            op_begin();
            if (end > chr_list[k].val && ((rc = ble_gattc_disc_all_dscs(conn, chr_list[k].val, end, dsc_cb, NULL)) || (rc = wait_op()))) { printf("disc dscs rc %d\n", rc); return -4; }
        }
    }
    return 0;
}

static int read_cb(uint16_t c, const struct ble_gatt_error *er, struct ble_gatt_attr *at, void *a)
{
    if (er->status == 0 && at) {
        uint8_t b[64]; uint16_t n = OS_MBUF_PKTLEN(at->om); if (n > sizeof b) n = sizeof b;
        ble_hs_mbuf_to_flat(at->om, b, n, &n);
        printf("read "); for (int i = 0; i < n; i++) printf("%02x", b[i]); printf("\n");
        op_status = 0; xSemaphoreGive(sem);
    } else { op_status = er->status; xSemaphoreGive(sem); }
    return 0;
}
static int cmd_read(int argc, char **argv)
{
    if (conn == BLE_HS_CONN_HANDLE_NONE || argc < 2) return -1;
    op_begin();
    if (ble_gattc_read(conn, atoi(argv[1]), read_cb, NULL)) return -2;
    return wait_op();
}
static int write_cb(uint16_t c, const struct ble_gatt_error *er, struct ble_gatt_attr *at, void *a)
{
    op_status = er->status; xSemaphoreGive(sem); return 0;
}
static int cmd_write(int argc, char **argv)                      /* ble_write <handle> <hex> */
{
    uint8_t b[64];
    if (conn == BLE_HS_CONN_HANDLE_NONE || argc < 3) return -1;
    int n = hex2bin(argv[2], b, sizeof b); op_begin();
    if (ble_gattc_write_flat(conn, atoi(argv[1]), b, n, write_cb, NULL)) return -2;
    return wait_op();
}
static int cmd_sub(int argc, char **argv)                         /* ble_sub <cccd handle> */
{
    uint8_t on[2] = { 1, 0 };
    if (conn == BLE_HS_CONN_HANDLE_NONE || argc < 2) return -1;
    nnotif = 0; ngaps = 0; have_prev = 0; op_begin();
    if (ble_gattc_write_flat(conn, atoi(argv[1]), on, 2, write_cb, NULL)) return -2;
    return wait_op();
}
static int cmd_stats(int argc, char **argv)
{
    printf("notifications %u gaps %u last_val %u last_len %u connected %d\n", (unsigned)nnotif, (unsigned)ngaps, (unsigned)last_val, (unsigned)last_nlen, conn != BLE_HS_CONN_HANDLE_NONE);
    return 0;
}
static int cmd_gatts_start(int argc, char **argv) { return ble_start(); }       /* the server is always registered; this starts the host */

const peer_cmd_t cmds_ble[] = {
    { "ble_adv", cmd_adv }, { "ble_adv_stop", cmd_adv_stop }, { "ble_scan", cmd_scan }, { "ble_connect", cmd_connect }, { "ble_disconnect", cmd_disconnect },
    { "ble_disc", cmd_disc }, { "ble_read", cmd_read }, { "ble_write", cmd_write }, { "ble_sub", cmd_sub },
    { "ble_stats", cmd_stats }, { "ble_gatts_start", cmd_gatts_start }, { 0 } };
