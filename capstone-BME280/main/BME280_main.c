#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_random.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_crc.h"
#include "espnow_types.h"

// --- Hardware Definitions ---
#define I2C_SCL_IO           7    // SCL Pin
#define I2C_SDA_IO           6    // SDA Pin
#define BME280_ADDR          0x77 // I2C Address (Try 0x76 if not working)

static float temp;
static float hum;
static float press;

static const char *TAG = "BME280_APP";
i2c_master_dev_handle_t dev_handle;

// --- Calibration Data Structure ---
typedef struct {
    uint16_t dig_T1; int16_t dig_T2, dig_T3;
    uint16_t dig_P1; int16_t dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
    uint8_t dig_H1, dig_H3; int16_t dig_H2, dig_H4, dig_H5; int8_t dig_H6;
    int32_t t_fine;
} bme280_calib_t;

bme280_calib_t cal;

// --- Helper Functions for I2C Read/Write ---
esp_err_t bme280_read_reg(uint8_t reg, uint8_t *data, size_t len) {
    return i2c_master_transmit_receive(dev_handle, &reg, 1, data, len, -1);
}

esp_err_t bme280_write_reg(uint8_t reg, uint8_t data) {
    uint8_t buf[2] = {reg, data};
    return i2c_master_transmit(dev_handle, buf, 2, -1);
}

// --- Fetch Calibration Parameters from Sensor NVM ---
void read_calibration_data() {
    uint8_t b[26];
    bme280_read_reg(0x88, b, 26);
    cal.dig_T1 = b[1]<<8 | b[0]; cal.dig_T2 = b[3]<<8 | b[2]; cal.dig_T3 = b[5]<<8 | b[4];
    cal.dig_P1 = b[7]<<8 | b[6]; cal.dig_P2 = b[9]<<8 | b[8]; cal.dig_P3 = b[11]<<8 | b[10];
    cal.dig_P4 = b[13]<<8 | b[12]; cal.dig_P5 = b[15]<<8 | b[14]; cal.dig_P6 = b[17]<<8 | b[16];
    cal.dig_P7 = b[19]<<8 | b[18]; cal.dig_P8 = b[21]<<8 | b[20]; cal.dig_P9 = b[23]<<8 | b[22];
    cal.dig_H1 = b[25];
    
    uint8_t h[7];
    bme280_read_reg(0xE1, h, 7);
    cal.dig_H2 = h[1]<<8 | h[0]; cal.dig_H3 = h[2];
    cal.dig_H4 = (h[3] << 4) | (h[4] & 0x0F);
    cal.dig_H5 = (h[5] << 4) | (h[4] >> 4);
    cal.dig_H6 = h[6];
}

// --- Compensation Formulas (From Bosch Datasheet) ---
float compensate_T(int32_t adc_T) {
    int32_t v1, v2, T;
    v1 = ((((adc_T >> 3) - ((int32_t)cal.dig_T1 << 1))) * ((int32_t)cal.dig_T2)) >> 11;
    v2 = (((((adc_T >> 4) - ((int32_t)cal.dig_T1)) * ((adc_T >> 4) - ((int32_t)cal.dig_T1))) >> 12) * ((int32_t)cal.dig_T3)) >> 14;
    cal.t_fine = v1 + v2;
    T = (cal.t_fine * 5 + 128) >> 8;
    return (float)T / 100.0;
}

float compensate_P(int32_t adc_P) {
    int64_t v1, v2, p;
    v1 = ((int64_t)cal.t_fine) - 128000;
    v2 = v1 * v1 * (int64_t)cal.dig_P6;
    v2 = v2 + ((v1 * (int64_t)cal.dig_P5) << 17);
    v2 = v2 + (((int64_t)cal.dig_P4) << 35);
    v1 = ((v1 * v1 * (int64_t)cal.dig_P3) >> 8) + ((v1 * (int64_t)cal.dig_P2) << 12);
    v1 = (((((int64_t)1) << 47) + v1)) * ((int64_t)cal.dig_P1) >> 33;
    if (v1 == 0) return 0;
    p = 1048576 - adc_P;
    p = (((p << 31) - v2) * 3125) / v1;
    v1 = (((int64_t)cal.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    v2 = (((int64_t)cal.dig_P8) * p) >> 19;
    p = ((p + v1 + v2) >> 8) + (((int64_t)cal.dig_P7) << 4);
    return (float)p / 256.0 / 100.0; // Unit: hPa
}

float compensate_H(int32_t adc_H) {
    int32_t v1;
    v1 = (cal.t_fine - ((int32_t)76800));
    v1 = (((((adc_H << 14) - (((int32_t)cal.dig_H4) << 20) - (((int32_t)cal.dig_H5) * v1)) +
            ((int32_t)16384)) >> 15) * (((((((v1 * ((int32_t)cal.dig_H6)) >> 10) *
            (((v1 * ((int32_t)cal.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) + ((int32_t)2097152)) *
            ((int32_t)cal.dig_H2) + 8192) >> 14));
    v1 = (v1 - (((((v1 >> 15) * (v1 >> 15)) >> 7) * ((int32_t)cal.dig_H1)) >> 4));
    v1 = (v1 < 0 ? 0 : v1);
    v1 = (v1 > 419430400 ? 419430400 : v1);
    return (float)(v1 >> 12) / 1024.0;
}

// --- Initialize I2C Bus and BME280 Device ---
void init_bme280(void) {
    i2c_master_bus_config_t bus_cfg = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = -1,
        .scl_io_num = I2C_SCL_IO,
        .sda_io_num = I2C_SDA_IO,
        .glitch_ignore_cnt = 7,
    };
    i2c_master_bus_handle_t bus_handle;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus_handle));

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = BME280_ADDR,
        .scl_speed_hz = 100000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle));

    // Configure Sensor: Humidity x1, Temp x1, Press x1, Normal Mode
    bme280_write_reg(0xF2, 0x01); // ctrl_hum
    bme280_write_reg(0xF4, 0x27); // ctrl_meas: temp x1, press x1, normal mode
    bme280_write_reg(0xF5, 0xA0); // config: standby time 1000ms
}

#define ESPNOW_MAXDELAY 512

static QueueHandle_t s_espnow_queue = NULL;

static uint8_t s_broadcast_mac[ESP_NOW_ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
static uint16_t s_espnow_seq[ESPNOW_DATA_MAX] = { 0, 0 };

static void espnow_deinit(espnow_send_param_t *send_param);

static uint16_t key_value = 0;

/* WiFi should start before using ESPNOW */
static void wifi_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK( esp_wifi_init(&cfg) );
    ESP_ERROR_CHECK( esp_wifi_set_storage(WIFI_STORAGE_RAM) );
    ESP_ERROR_CHECK( esp_wifi_set_mode(ESPNOW_WIFI_MODE) );
    ESP_ERROR_CHECK( esp_wifi_start());
    ESP_ERROR_CHECK( esp_wifi_set_channel(CONFIG_ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE));

#if CONFIG_ESPNOW_ENABLE_LONG_RANGE
    ESP_ERROR_CHECK( esp_wifi_set_protocol(ESPNOW_WIFI_IF, WIFI_PROTOCOL_11B|WIFI_PROTOCOL_11G|WIFI_PROTOCOL_11N|WIFI_PROTOCOL_LR) );
#endif
}

/* ESPNOW sending or receiving callback function is called in WiFi task.
 * Users should not do lengthy operations from this task. Instead, post
 * necessary data to a queue and handle it from a lower priority task. */
static void espnow_send_cb(const esp_now_send_info_t *tx_info, esp_now_send_status_t status)
{
    espnow_event_t evt;
    espnow_event_send_cb_t *send_cb = &evt.info.send_cb;

    if (tx_info == NULL) {
        ESP_LOGE(TAG, "Send cb arg error");
        return;
    }

    evt.id = ESPNOW_SEND_CB;
    memcpy(send_cb->mac_addr, tx_info->des_addr, ESP_NOW_ETH_ALEN);
    send_cb->status = status;
    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send send queue fail");
    }
}

static void espnow_recv_cb(const esp_now_recv_info_t *recv_info, const uint8_t *data, int len)
{
    espnow_event_t evt;
    espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;
    uint8_t * mac_addr = recv_info->src_addr;
    uint8_t * des_addr = recv_info->des_addr;

    if (mac_addr == NULL || data == NULL || len <= 0) {
        ESP_LOGE(TAG, "Receive cb arg error");
        return;
    }

    if (IS_BROADCAST_ADDR(des_addr)) {
        /* If added a peer with encryption before, the receive packets may be
         * encrypted as peer-to-peer message or unencrypted over the broadcast channel.
         * Users can check the destination address to distinguish it.
         */
        ESP_LOGD(TAG, "Receive broadcast ESPNOW data");
    } else {
        ESP_LOGD(TAG, "Receive unicast ESPNOW data");
    }

    evt.id = ESPNOW_RECV_CB;
    memcpy(recv_cb->mac_addr, mac_addr, ESP_NOW_ETH_ALEN);
    recv_cb->data = malloc(len);
    if (recv_cb->data == NULL) {
        ESP_LOGE(TAG, "Malloc receive data fail");
        return;
    }
    memcpy(recv_cb->data, data, len);
    recv_cb->data_len = len;
    if (xQueueSend(s_espnow_queue, &evt, ESPNOW_MAXDELAY) != pdTRUE) {
        ESP_LOGW(TAG, "Send receive queue fail");
        free(recv_cb->data);
    }
}

/* Parse received ESPNOW data. */
int espnow_data_parse(uint8_t *data, uint16_t data_len)
{
    espnow_data_t *buf = (espnow_data_t *)data;
    uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(espnow_data_t)) {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, data_len);

    if (crc_cal == crc) {
        return 1;
    }

    return -1;
}

int broadcast_espnow_data_parse(uint8_t *data, uint16_t data_len)
{
    espnow_broadcast_data_t *buf = (espnow_broadcast_data_t *)data;
    uint16_t crc, crc_cal = 0;

    if (data_len < sizeof(espnow_broadcast_data_t)) {
        ESP_LOGE(TAG, "Receive ESPNOW data too short, len:%d", data_len);
        return -1;
    }

    crc = buf->crc;
    buf->crc = 0;
    crc_cal = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, sizeof(espnow_broadcast_data_t));

    if (crc_cal == crc) {
        key_value = buf->key;
        return 1;
    }

    return -1;

}

/* Prepare ESPNOW data to be sent. */
void espnow_data_prepare(espnow_send_param_t *send_param)
{
    espnow_data_t *buf = (espnow_data_t *)send_param->buffer;

    assert(send_param->len >= sizeof(espnow_data_t));

    buf->crc = 0;
    /* Fill all remaining bytes after the data with random values */
    esp_fill_random(buf->payload, send_param->len - sizeof(espnow_data_t));
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, send_param->len);
}

void environment_data_prepare(espnow_send_param_t *send_param)
{
    espnow_environment_data_t *buf = (espnow_environment_data_t *)send_param->buffer;
    buf->crc = 0;
    buf->key = send_param->key;
    buf->type = UNICAST_TYPE_ENVIRONMENT;
    buf->temperature = temp;
    buf->pressure = press;
    buf->humidity = hum;
    buf->crc = esp_crc16_le(UINT16_MAX, (uint8_t const *)buf, sizeof(espnow_environment_data_t));
}

static void espnow_task(void *pvParameter)
{
    espnow_event_t evt;
    uint8_t recv_state = 0;
    uint16_t recv_seq = 0;
    uint32_t recv_magic = 0;
    bool is_broadcast = false;
    int ret;

    ESP_LOGI(TAG, "Waiting on broadcast data");

    espnow_send_param_t *send_param = (espnow_send_param_t *)pvParameter;


    while (xQueueReceive(s_espnow_queue, &evt, portMAX_DELAY) == pdTRUE) {
        switch (evt.id) {
            case ESPNOW_SEND_CB:
            {
                /*
                espnow_event_send_cb_t *send_cb = &evt.info.send_cb;
                is_broadcast = IS_BROADCAST_ADDR(send_cb->mac_addr);

                ESP_LOGD(TAG, "Send data to "MACSTR", status1: %d", MAC2STR(send_cb->mac_addr), send_cb->status);


                
                ESP_LOGI(TAG, "send data to "MACSTR"", MAC2STR(send_cb->mac_addr));
                */

                /* Send the next data after the previous data is sent. */
                
                break;
            }
            case ESPNOW_RECV_CB:
            {
                espnow_event_recv_cb_t *recv_cb = &evt.info.recv_cb;

                ret = broadcast_espnow_data_parse(recv_cb->data, recv_cb->data_len);
                free(recv_cb->data);
                if (ret == 1) {
                    ESP_LOGI(TAG, "Receive %dth broadcast data from: "MACSTR", len: %d", recv_seq, MAC2STR(recv_cb->mac_addr), recv_cb->data_len);

                    /* If MAC address does not exist in peer list, add it to peer list. */
                    if (esp_now_is_peer_exist(recv_cb->mac_addr) == false) {
                        esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
                        if (peer == NULL) {
                            ESP_LOGE(TAG, "Malloc peer information fail");
                            espnow_deinit(send_param);
                            vTaskDelete(NULL);
                        }
                        memset(peer, 0, sizeof(esp_now_peer_info_t));
                        peer->channel = CONFIG_ESPNOW_CHANNEL;
                        peer->ifidx = ESPNOW_WIFI_IF;
                        peer->encrypt = false;
                        memcpy(peer->lmk, CONFIG_ESPNOW_LMK, ESP_NOW_KEY_LEN);
                        memcpy(peer->peer_addr, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                        ESP_ERROR_CHECK( esp_now_add_peer(peer) );
                        free(peer);
                    }

                    /* Indicates that the device has received broadcast ESPNOW data. */
                    if (send_param->state == 0) {
                        send_param->state = 1;
                    }

                    
                    
                    //ESP_LOGI(TAG, "Start sending unicast data");
                    ESP_LOGI(TAG, "send data to "MACSTR"", MAC2STR(recv_cb->mac_addr));

                    /* Start sending unicast ESPNOW data. */
                    memcpy(send_param->dest_mac, recv_cb->mac_addr, ESP_NOW_ETH_ALEN);
                    send_param->key = key_value;
                    ESP_LOGI(TAG, "%d", key_value);
                    environment_data_prepare(send_param);
                    if (esp_now_send(send_param->dest_mac, send_param->buffer, send_param->len) != ESP_OK) {
                        ESP_LOGI(TAG, "Send error");
                        espnow_deinit(send_param);
                        vTaskDelete(NULL);
                    } else {
                        //ESP_LOGI(TAG, "Send success :3");
                    }
                    
                }
                else {
                    ESP_LOGI(TAG, "Receive error data from: "MACSTR"", MAC2STR(recv_cb->mac_addr));
                }
                break;
            }
            default:
                ESP_LOGE(TAG, "Callback type error: %d", evt.id);
                break;
        }
    }
}

static esp_err_t espnow_init(void)
{
    espnow_send_param_t *send_param;

    s_espnow_queue = xQueueCreate(ESPNOW_QUEUE_SIZE, sizeof(espnow_event_t));
    if (s_espnow_queue == NULL) {
        ESP_LOGE(TAG, "Create queue fail");
        return ESP_FAIL;
    }

    /* Initialize ESPNOW and register sending and receiving callback function. */
    ESP_ERROR_CHECK( esp_now_init() );
    ESP_ERROR_CHECK( esp_now_register_send_cb(espnow_send_cb) );
    ESP_ERROR_CHECK( esp_now_register_recv_cb(espnow_recv_cb) );
#if CONFIG_ESPNOW_ENABLE_POWER_SAVE
    ESP_ERROR_CHECK( esp_now_set_wake_window(CONFIG_ESPNOW_WAKE_WINDOW) );
    ESP_ERROR_CHECK( esp_wifi_connectionless_module_set_wake_interval(CONFIG_ESPNOW_WAKE_INTERVAL) );
#endif
    /* Set primary master key. */
    ESP_ERROR_CHECK( esp_now_set_pmk((uint8_t *)CONFIG_ESPNOW_PMK) );

    /* Add broadcast peer information to peer list. */
    esp_now_peer_info_t *peer = malloc(sizeof(esp_now_peer_info_t));
    if (peer == NULL) {
        ESP_LOGE(TAG, "Malloc peer information fail");
        vQueueDelete(s_espnow_queue);
        s_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(peer, 0, sizeof(esp_now_peer_info_t));
    peer->channel = CONFIG_ESPNOW_CHANNEL;
    peer->ifidx = ESPNOW_WIFI_IF;
    peer->encrypt = false;
    memcpy(peer->peer_addr, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    ESP_ERROR_CHECK( esp_now_add_peer(peer) );
    free(peer);

    /* Initialize sending parameters. */
    send_param = malloc(sizeof(espnow_send_param_t));
    if (send_param == NULL) {
        ESP_LOGE(TAG, "Malloc send parameter fail");
        vQueueDelete(s_espnow_queue);
        s_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memset(send_param, 0, sizeof(espnow_send_param_t));
    send_param->unicast = false;
    send_param->broadcast = true;
    send_param->state = 0;
    send_param->magic = esp_random();
    send_param->count = CONFIG_ESPNOW_SEND_COUNT;
    send_param->key = 0;
    send_param->len = sizeof(espnow_environment_data_t);
    send_param->buffer = malloc(CONFIG_ESPNOW_SEND_LEN);
    if (send_param->buffer == NULL) {
        ESP_LOGE(TAG, "Malloc send buffer fail");
        free(send_param);
        vQueueDelete(s_espnow_queue);
        s_espnow_queue = NULL;
        esp_now_deinit();
        return ESP_FAIL;
    }
    memcpy(send_param->dest_mac, s_broadcast_mac, ESP_NOW_ETH_ALEN);
    espnow_data_prepare(send_param);

    xTaskCreate(espnow_task, "espnow_task", 2048, send_param, 4, NULL);

    return ESP_OK;
}

static void espnow_deinit(espnow_send_param_t *send_param)
{
    free(send_param->buffer);
    free(send_param);
    vQueueDelete(s_espnow_queue);
    s_espnow_queue = NULL;
    esp_now_deinit();
}

#define ESP_INTR_FLAG_DEFAULT 0

void app_main(void) {
    init_bme280();
    read_calibration_data();
    ESP_LOGI(TAG, "I2C Bus initialized and Calibration data loaded.");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK( nvs_flash_erase() );
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK( ret );

    wifi_init();
    espnow_init();

    uint8_t data[8];
    while (1) {
        // Burst read: Pressure (3 bytes), Temperature (3 bytes), Humidity (2 bytes)
        if (bme280_read_reg(0xF7, data, 8) == ESP_OK) {
            int32_t adc_P = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
            int32_t adc_T = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);
            int32_t adc_H = (data[6] << 8) | data[7];

            temp = compensate_T(adc_T);
            press = compensate_P(adc_P);
            hum = compensate_H(adc_H);

            printf("-----------------------------\n");
            printf("Temperature: %.2f degC\n", temp);
            printf("Humidity:    %.2f %%\n", hum);
            printf("Pressure:    %.2f hPa\n", press);
        } else {
            ESP_LOGE(TAG, "Sensor communication failed!");
        }
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}