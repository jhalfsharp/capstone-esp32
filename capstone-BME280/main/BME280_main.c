#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/i2c_master.h"

// --- Hardware Definitions ---
#define I2C_SCL_IO           7    // SCL Pin
#define I2C_SDA_IO           6    // SDA Pin
#define BME280_ADDR          0x77 // I2C Address (Try 0x76 if not working)

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

void app_main(void) {
    init_bme280();
    read_calibration_data();
    ESP_LOGI(TAG, "I2C Bus initialized and Calibration data loaded.");

    uint8_t data[8];
    while (1) {
        // Burst read: Pressure (3 bytes), Temperature (3 bytes), Humidity (2 bytes)
        if (bme280_read_reg(0xF7, data, 8) == ESP_OK) {
            int32_t adc_P = (data[0] << 12) | (data[1] << 4) | (data[2] >> 4);
            int32_t adc_T = (data[3] << 12) | (data[4] << 4) | (data[5] >> 4);
            int32_t adc_H = (data[6] << 8) | data[7];

            float temp = compensate_T(adc_T);
            float press = compensate_P(adc_P);
            float hum = compensate_H(adc_H);

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