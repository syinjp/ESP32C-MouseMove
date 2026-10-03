/*
 * BLE HID mouse for ESP32-C3.
 *
 * Based on the ESP-IDF esp_hid_device example. This application uses only
 * Bluetooth Low Energy, which is supported by ESP32-C3.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "freertos/timers.h"

#include "esp_err.h"
#include "esp_hid_common.h"
#include "esp_hidd.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "nimble/ble.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"

#define HID_SERVICE_UUID                 0x1812
#define HID_BATTERY_LEVEL                100
#define HID_REPORT_MAP_INDEX             0
#define HID_MOUSE_REPORT_ID              0
#define HID_MOUSE_REPORT_LENGTH          4

#if !defined(CONFIG_BT_NIMBLE_NVS_PERSIST) || !CONFIG_BT_NIMBLE_NVS_PERSIST
#error "Enable CONFIG_BT_NIMBLE_NVS_PERSIST in menuconfig to retain pairing keys."
#endif

#define MOUSE_ENCRYPTED_BIT              BIT0
#define MOUSE_HOST_ACTIVE_BIT            BIT1
#define MOUSE_READY_BITS                 (MOUSE_ENCRYPTED_BIT | MOUSE_HOST_ACTIVE_BIT)

static const char *TAG = "mouse_move";

static esp_hidd_dev_t *s_hid_device;
static EventGroupHandle_t s_mouse_events;
static TimerHandle_t s_advertising_timer;
static uint8_t s_own_addr_type;

/* Three buttons, relative X/Y movement, and a vertical wheel. */
static const uint8_t s_mouse_report_map[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */
    0x05, 0x09,       /*     Usage Page (Button) */
    0x19, 0x01,       /*     Usage Minimum (Button 1) */
    0x29, 0x03,       /*     Usage Maximum (Button 3) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x25, 0x01,       /*     Logical Maximum (1) */
    0x95, 0x03,       /*     Report Count (3) */
    0x75, 0x01,       /*     Report Size (1) */
    0x81, 0x02,       /*     Input (Data, Variable, Absolute) */
    0x95, 0x01,       /*     Report Count (1) */
    0x75, 0x05,       /*     Report Size (5) */
    0x81, 0x03,       /*     Input (Constant) */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x09, 0x30,       /*     Usage (X) */
    0x09, 0x31,       /*     Usage (Y) */
    0x09, 0x38,       /*     Usage (Wheel) */
    0x15, 0x81,       /*     Logical Minimum (-127) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x95, 0x03,       /*     Report Count (3) */
    0x81, 0x06,       /*     Input (Data, Variable, Relative) */
    0xC0,             /*   End Collection */
    0xC0              /* End Collection */
};

static esp_hid_raw_report_map_t s_report_maps[] = {
    {
        .data = s_mouse_report_map,
        .len = sizeof(s_mouse_report_map),
    },
};

static const esp_hid_device_config_t s_hid_config = {
    .vendor_id = 0x303A,
    .product_id = 0x4001,
    .version = 0x0100,
    .device_name = CONFIG_MOUSE_DEVICE_NAME,
    .manufacturer_name = "Espressif",
    .serial_number = "C3-MouseMove-01",
    .report_maps = s_report_maps,
    .report_maps_len = 1,
};

static ble_uuid16_t s_hid_service_uuid = BLE_UUID16_INIT(HID_SERVICE_UUID);

static bool mouse_is_ready(void)
{
    return s_mouse_events != NULL &&
           (xEventGroupGetBits(s_mouse_events) & MOUSE_READY_BITS) == MOUSE_READY_BITS &&
           s_hid_device != NULL &&
           esp_hidd_dev_connected(s_hid_device);
}

static esp_err_t mouse_send_report(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel)
{
    uint8_t report[HID_MOUSE_REPORT_LENGTH] = {
        buttons,
        (uint8_t)dx,
        (uint8_t)dy,
        (uint8_t)wheel,
    };

    return esp_hidd_dev_input_set(s_hid_device,
                                  HID_REPORT_MAP_INDEX,
                                  HID_MOUSE_REPORT_ID,
                                  report,
                                  sizeof(report));
}

static void mouse_move_square(void)
{
    const int8_t distance = (int8_t)CONFIG_MOUSE_MOVE_DISTANCE;
    const int8_t movement[][2] = {
        { distance, 0 },
        { 0, distance },
        { -distance, 0 },
        { 0, -distance },
    };

    for (size_t i = 0; i < sizeof(movement) / sizeof(movement[0]); ++i) {
        if (!mouse_is_ready()) {
            return;
        }

        esp_err_t err = mouse_send_report(0, movement[i][0], movement[i][1], 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Mouse report failed: %s", esp_err_to_name(err));
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(75));
    }

    ESP_LOGI(TAG, "Mouse movement sent");
}

static bool wait_while_ready(uint32_t delay_ms)
{
    const uint32_t check_period_ms = 250;

    while (delay_ms > 0) {
        if (!mouse_is_ready()) {
            return false;
        }

        uint32_t wait_ms = delay_ms < check_period_ms ? delay_ms : check_period_ms;
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        delay_ms -= wait_ms;
    }

    return mouse_is_ready();
}

static void mouse_move_task(void *arg)
{
    (void)arg;

    while (true) {
        xEventGroupWaitBits(s_mouse_events,
                            MOUSE_READY_BITS,
                            pdFALSE,
                            pdTRUE,
                            portMAX_DELAY);

        /* Make the first movement soon after pairing so operation is visible. */
        if (!wait_while_ready(3000)) {
            /* Also yield if the asynchronous HID state is not ready yet. */
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        while (mouse_is_ready()) {
            mouse_move_square();
            if (!wait_while_ready((uint32_t)CONFIG_MOUSE_MOVE_INTERVAL_SEC * 1000U)) {
                break;
            }
        }
    }
}

static int gap_event_handler(struct ble_gap_event *event, void *arg);
static void schedule_advertising(void);

static esp_err_t start_advertising(void)
{
    static const struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .tx_pwr_lvl_is_present = 1,
        .tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO,
        .appearance = ESP_HID_APPEARANCE_MOUSE,
        .appearance_is_present = 1,
        .uuids16 = &s_hid_service_uuid,
        .num_uuids16 = 1,
        .uuids16_is_complete = 1,
    };
    static const struct ble_hs_adv_fields response_fields = {
        .name = (uint8_t *)CONFIG_MOUSE_DEVICE_NAME,
        .name_len = sizeof(CONFIG_MOUSE_DEVICE_NAME) - 1,
        .name_is_complete = 1,
    };
    struct ble_gap_adv_params params = {0};

    if ((s_hid_device != NULL && esp_hidd_dev_connected(s_hid_device)) ||
        ble_gap_adv_active()) {
        return ESP_OK;
    }

    int rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "Inferring BLE address type failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Setting advertisement data failed: %d", rc);
        return ESP_FAIL;
    }

    rc = ble_gap_adv_rsp_set_fields(&response_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Setting scan response data failed: %d", rc);
        return ESP_FAIL;
    }

    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    params.itvl_min = BLE_GAP_ADV_ITVL_MS(30);
    params.itvl_max = BLE_GAP_ADV_ITVL_MS(50);

    rc = ble_gap_adv_start(s_own_addr_type,
                           NULL,
                           BLE_HS_FOREVER,
                           &params,
                           gap_event_handler,
                           NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Starting advertisement failed: %d", rc);
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Advertising as '%s'", CONFIG_MOUSE_DEVICE_NAME);
    return ESP_OK;
}

static void advertising_timer_callback(TimerHandle_t timer)
{
    (void)timer;

    if (start_advertising() != ESP_OK) {
        ESP_LOGW(TAG, "Advertising will be retried");
        xTimerReset(s_advertising_timer, 0);
    }
}

static void schedule_advertising(void)
{
    if (s_advertising_timer != NULL) {
        if (xTimerReset(s_advertising_timer, 0) != pdPASS) {
            ESP_LOGE(TAG, "Could not schedule advertising retry");
        }
    }
}

static void log_bond_count(const char *stage)
{
    ble_addr_t peers[CONFIG_BT_NIMBLE_MAX_BONDS];
    int count = 0;
    int rc = ble_store_util_bonded_peers(peers, &count, CONFIG_BT_NIMBLE_MAX_BONDS);
    if (rc == 0) {
        ESP_LOGI(TAG, "Bond store (%s): %d/%d peers; NVS persistence enabled",
                 stage, count, CONFIG_BT_NIMBLE_MAX_BONDS);
    } else {
        ESP_LOGW(TAG, "Reading bond count failed: rc=%d (0x%x)", rc, (unsigned)rc);
    }
}

static void log_connection_state(const char *stage, const struct ble_gap_conn_desc *desc)
{
    const uint8_t *id = desc->peer_id_addr.val;
    const uint8_t *ota = desc->peer_ota_addr.val;
    /* Addresses identify the peer; never log bonding keys or key material. */
    ESP_LOGI(TAG,
             "%s: handle=%u peer_id=%02x:%02x:%02x:%02x:%02x:%02x(type=%u) "
             "peer_ota=%02x:%02x:%02x:%02x:%02x:%02x(type=%u)",
             stage, (unsigned)desc->conn_handle,
             id[5], id[4], id[3], id[2], id[1], id[0], desc->peer_id_addr.type,
             ota[5], ota[4], ota[3], ota[2], ota[1], ota[0], desc->peer_ota_addr.type);
    ESP_LOGI(TAG, "Security: encrypted=%u bonded=%u authenticated=%u key_size=%u",
             (unsigned)desc->sec_state.encrypted, (unsigned)desc->sec_state.bonded,
             (unsigned)desc->sec_state.authenticated, (unsigned)desc->sec_state.key_size);
}

static void disconnect_for_security_recovery(uint16_t conn_handle)
{
    ESP_LOGW(TAG, "Ending unusable connection: handle=%u", (unsigned)conn_handle);
    int rc = ble_gap_terminate(conn_handle, BLE_ERR_AUTH_FAIL);
    if (rc == BLE_HS_ENOTCONN) {
        schedule_advertising();
    } else if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "Disconnect request failed: rc=%d (0x%x)", rc, (unsigned)rc);
    }
    /* A successful request resumes advertising from the GAP disconnect event. */
}

static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT: {
        if (event->connect.status != 0) {
            /* This SDK can report a raw HCI status from peer feature exchange. */
            ESP_LOGW(TAG, "Connection completion failed: handle=%u status=%d (0x%x)",
                     (unsigned)event->connect.conn_handle, event->connect.status,
                     (unsigned)event->connect.status);
            schedule_advertising();
            break;
        }

        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGW(TAG, "Connected link no longer available: rc=%d (0x%x)", rc, (unsigned)rc);
            break;
        }
        log_connection_state("BLE connected", &desc);
        xEventGroupSetBits(s_mouse_events, MOUSE_HOST_ACTIVE_BIT);

        /* Peer-initiated encryption may finish before this delayed CONNECT event. */
        if (desc.sec_state.encrypted) {
            xEventGroupSetBits(s_mouse_events, MOUSE_ENCRYPTED_BIT);
            break;
        }
        xEventGroupClearBits(s_mouse_events, MOUSE_ENCRYPTED_BIT);
        rc = ble_gap_security_initiate(event->connect.conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_ENOTCONN) {
            ESP_LOGW(TAG, "Starting security failed: rc=%d (0x%x)", rc, (unsigned)rc);
            disconnect_for_security_recovery(event->connect.conn_handle);
        }
        break;
    }

    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.status == 0) {
            struct ble_gap_conn_desc desc;
            int rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            if (rc == 0) {
                log_connection_state("Security changed", &desc);
                if (desc.sec_state.encrypted) {
                    xEventGroupSetBits(s_mouse_events, MOUSE_ENCRYPTED_BIT);
                    log_bond_count("encrypted");
                    break;
                }
                xEventGroupClearBits(s_mouse_events, MOUSE_ENCRYPTED_BIT);
                disconnect_for_security_recovery(event->enc_change.conn_handle);
            } else {
                xEventGroupClearBits(s_mouse_events, MOUSE_ENCRYPTED_BIT);
            }
        } else {
            xEventGroupClearBits(s_mouse_events, MOUSE_ENCRYPTED_BIT);
            if (event->enc_change.status == BLE_HS_ENOTCONN) {
                ESP_LOGI(TAG, "Security procedure ended because the link disconnected");
            } else {
                ESP_LOGW(TAG, "Security failed: handle=%u status=%d (0x%x)",
                         (unsigned)event->enc_change.conn_handle, event->enc_change.status,
                         (unsigned)event->enc_change.status);
                /* Includes the NimBLE SMP timeout; do not keep the only link occupied. */
                disconnect_for_security_recovery(event->enc_change.conn_handle);
            }
        }
        break;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        xEventGroupClearBits(s_mouse_events, MOUSE_READY_BITS);
        log_connection_state("BLE disconnected", &event->disconnect.conn);
        ESP_LOGI(TAG, "Disconnect reason=%d (0x%x)", event->disconnect.reason,
                 (unsigned)event->disconnect.reason);
        /* Also covers links that ended before the HID CONNECT event. */
        schedule_advertising();
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        schedule_advertising();
        break;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        struct ble_gap_conn_desc desc;
        int rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc == 0) {
            log_connection_state("Peer requested fresh pairing", &desc);
            rc = ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        if (rc != 0) {
            ESP_LOGW(TAG, "Replacing peer bond failed: rc=%d (0x%x)", rc, (unsigned)rc);
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }
        log_bond_count("re-pairing");
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_IDENTITY_RESOLVED: {
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->identity_resolved.conn_handle, &desc) == 0) {
            log_connection_state("Peer identity resolved", &desc);
        }
        break;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "Subscription: handle=%u attr=%u notify=%u indicate=%u",
                 (unsigned)event->subscribe.conn_handle, (unsigned)event->subscribe.attr_handle,
                 (unsigned)event->subscribe.cur_notify, (unsigned)event->subscribe.cur_indicate);
        break;

    default:
        break;
    }

    return 0;
}

static void hid_event_handler(void *handler_arg,
                              esp_event_base_t base,
                              int32_t event_id,
                              void *event_data)
{
    (void)handler_arg;
    (void)base;
    esp_hidd_event_t event = (esp_hidd_event_t)event_id;
    esp_hidd_event_data_t *data = (esp_hidd_event_data_t *)event_data;

    switch (event) {
    case ESP_HIDD_START_EVENT:
        ESP_LOGI(TAG, "BLE HID service started");
        schedule_advertising();
        break;

    case ESP_HIDD_CONNECT_EVENT:
        ESP_LOGI(TAG, "HID host connected");
        break;

    case ESP_HIDD_PROTOCOL_MODE_EVENT:
        ESP_LOGI(TAG,
                 "Protocol mode: %s",
                 data->protocol_mode.protocol_mode ? "report" : "boot");
        break;

    case ESP_HIDD_CONTROL_EVENT:
        if (data->control.control == 0) {
            xEventGroupClearBits(s_mouse_events, MOUSE_HOST_ACTIVE_BIT);
            ESP_LOGI(TAG, "HID suspended");
        } else {
            xEventGroupSetBits(s_mouse_events, MOUSE_HOST_ACTIVE_BIT);
            ESP_LOGI(TAG, "HID resumed");
        }
        break;

    case ESP_HIDD_DISCONNECT_EVENT:
        /* GAP already cleared state and scheduled advertising. This queued HID
         * event can arrive late; never clear a newer connection's ready bits. */
        ESP_LOGI(TAG, "HID host disconnected");
        break;

    default:
        break;
    }
}

static void nimble_host_task(void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_store_config_init(void);

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    s_mouse_events = xEventGroupCreate();
    if (s_mouse_events == NULL) {
        ESP_LOGE(TAG, "Could not create mouse event group");
        return;
    }

    s_advertising_timer = xTimerCreate("ble_adv_retry",
                                       pdMS_TO_TICKS(500),
                                       pdFALSE,
                                       NULL,
                                       advertising_timer_callback);
    if (s_advertising_timer == NULL) {
        ESP_LOGE(TAG, "Could not create advertising timer");
        return;
    }

    ESP_ERROR_CHECK(nimble_port_init());

    /* Headless Just Works pairing with bonding and Secure Connections. */
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    ble_store_config_init();
    ESP_LOGI(TAG, "Reset reason=%d; reconnect recovery enabled", (int)esp_reset_reason());
    log_bond_count("boot");

    ESP_ERROR_CHECK(esp_hidd_dev_init(&s_hid_config,
                                      ESP_HID_TRANSPORT_BLE,
                                      hid_event_handler,
                                      &s_hid_device));
    ESP_ERROR_CHECK(esp_hidd_dev_battery_set(s_hid_device, HID_BATTERY_LEVEL));

    int rc = ble_svc_gap_device_name_set(CONFIG_MOUSE_DEVICE_NAME);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);
    rc = ble_svc_gap_device_appearance_set(ESP_HID_APPEARANCE_MOUSE);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);
    BaseType_t task_created = xTaskCreate(mouse_move_task,
                                         "mouse_move",
                                         3072,
                                         NULL,
                                         5,
                                         NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "Could not create mouse movement task");
        return;
    }

    ESP_LOGI(TAG,
             "Starting BLE mouse (first move: 3 s, interval: %d s, distance: %d)",
             CONFIG_MOUSE_MOVE_INTERVAL_SEC,
             CONFIG_MOUSE_MOVE_DISTANCE);
    nimble_port_freertos_init(nimble_host_task);
}
