// /* Blink Example

//    This example code is in the Public Domain (or CC0 licensed, at your option.)

//    Unless required by applicable law or agreed to in writing, this
//    software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
//    CONDITIONS OF ANY KIND, either express or implied.
// */
// #include <stdio.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "driver/gpio.h"
// #include "esp_log.h"
// #include "led_strip.h"
// #include "sdkconfig.h"

// static const char *TAG = "example";

// /* Use project configuration menu (idf.py menuconfig) to choose the GPIO to blink,
//    or you can edit the following line and set a number here.
// */
// #define BLINK_GPIO CONFIG_BLINK_GPIO

// static uint8_t s_led_state = 0;

// #ifdef CONFIG_BLINK_LED_STRIP

// static led_strip_handle_t led_strip;

// static void blink_led(void)
// {
//     /* If the addressable LED is enabled */
//     if (s_led_state) {
//         /* Set the LED pixel using RGB from 0 (0%) to 255 (100%) for each color */
//         led_strip_set_pixel(led_strip, 0, 16, 16, 16);
//         /* Refresh the strip to send data */
//         led_strip_refresh(led_strip);
//     } else {
//         /* Set all LED off to clear all pixels */
//         led_strip_clear(led_strip);
//     }
// }

// static void configure_led(void)
// {
//     ESP_LOGI(TAG, "Example configured to blink addressable LED!");
//     /* LED strip initialization with the GPIO and pixels number*/
//     led_strip_config_t strip_config = {
//         .strip_gpio_num = BLINK_GPIO,
//         .max_leds = 1, // at least one LED on board
//     };
// #if CONFIG_BLINK_LED_STRIP_BACKEND_RMT
//     led_strip_rmt_config_t rmt_config = {
//         .resolution_hz = 10 * 1000 * 1000, // 10MHz
//         .flags.with_dma = false,
//     };
//     ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
// #elif CONFIG_BLINK_LED_STRIP_BACKEND_SPI
//     led_strip_spi_config_t spi_config = {
//         .spi_bus = SPI2_HOST,
//         .flags.with_dma = true,
//     };
//     ESP_ERROR_CHECK(led_strip_new_spi_device(&strip_config, &spi_config, &led_strip));
// #else
// #error "unsupported LED strip backend"
// #endif
//     /* Set all LED off to clear all pixels */
//     led_strip_clear(led_strip);
// }

// #elif CONFIG_BLINK_LED_GPIO

// static void blink_led(void)
// {
//     /* Set the GPIO level according to the state (LOW or HIGH)*/
//     gpio_set_level(BLINK_GPIO, s_led_state);
// }

// static void configure_led(void)
// {
//     ESP_LOGI(TAG, "Example configured to blink GPIO LED!");
//     gpio_reset_pin(BLINK_GPIO);
//     /* Set the GPIO as a push/pull output */
//     gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);
// }

// #else
// #error "unsupported LED type"
// #endif

// void app_main(void)
// {

//     /* Configure the peripheral according to the LED type */
//     configure_led();

//     while (1) {
//         ESP_LOGI(TAG, "r   the LED %s!", s_led_state == true ? "ON" : "OFF");
//         blink_led();
//         /* Toggle the LED state */
//         s_led_state = !s_led_state;
//         vTaskDelay(CONFIG_BLINK_PERIOD / portTICK_PERIOD_MS);
//     }
// }

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "led_strip.h" // Required: Must add dependency first

// Hardware configuration
#define PIR_SENSOR_GPIO 2
#define RGB_LED_GPIO    8   // Standard for ESP32-C6 DevKitC-1
static const char *TAG = "RGB_SMART_LIGHT";

// Hold time (10 seconds)
#define LIGHT_HOLD_TIME_MS 5000 

static led_strip_handle_t led_strip;

void configure_led(void)
{
    /* LED strip initialization with the GPIO and pixels number*/
    led_strip_config_t strip_config = {
        .strip_gpio_num = RGB_LED_GPIO,
        .max_leds = 1, // Board usually has only 1 RGB LED
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000, // 10MHz
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    /* Total clear led strip */
    led_strip_clear(led_strip);
}

void app_main(void)
{
    // 1. Initialize PIR Sensor
    gpio_reset_pin(PIR_SENSOR_GPIO);
    gpio_set_direction(PIR_SENSOR_GPIO, GPIO_MODE_INPUT);
    gpio_pulldown_en(PIR_SENSOR_GPIO); 

    // 2. Initialize RGB LED
    configure_led();

    ESP_LOGI(TAG, "System started. Waiting for sensor stabilization (approx. 30s)...");
    
    int countdown_ms = 0;

    while (1) {
        int motion = gpio_get_level(PIR_SENSOR_GPIO);

        if (motion == 1) {
            if (countdown_ms <= 0) {
                printf("[EVENT] Motion detected! Turning on RGB LED.\n");
                // Set RGB color
                led_strip_set_pixel(led_strip, 0, 255, 255, 255);
                led_strip_refresh(led_strip);
            }
            countdown_ms = LIGHT_HOLD_TIME_MS; 
        } else {
            if (countdown_ms > 0) {
                countdown_ms -= 100;
                if (countdown_ms <= 0) {
                    printf("[EVENT] Area vacant. Turning RGB LED OFF.\n");
                    led_strip_clear(led_strip);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }
}