// #include <stdio.h>
// #include <math.h>
// #include <string.h>
// #include <stdlib.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "driver/i2s_std.h"
// #include "driver/gpio.h"
// #include "esp_log.h"
// #include "esp_dsp.h"
// #include "sdkconfig.h"

// // --- 1. Configuration Parameters ---
// // Increase sample rate to 32kHz to achieve a 2.048MHz I2S clock, 
// #define SAMPLE_RATE     (32000)
// #define FFT_SIZE        (1024)

// // ESP32-C6 Pin Configuration
// #define I2S_BCK_IO      (GPIO_NUM_19)
// #define I2S_WS_IO       (GPIO_NUM_18)
// #define I2S_DO_IO       (GPIO_NUM_23)

// // --- 2. Global Variables ---
// i2s_chan_handle_t rx_handle;
// float fft_input[FFT_SIZE * 2];
// float window[FFT_SIZE];

// // --- 3. I2S Initialization ---
// void init_i2s() {
//     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
//     ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

//     // Core Fix: Force STEREO mode and operate with 32-bit slot width.
//     i2s_std_config_t std_cfg = {
//         .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
//         .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
//         .gpio_cfg = {
//             .mclk = I2S_GPIO_UNUSED,
//             .bclk = I2S_BCK_IO,
//             .ws = I2S_WS_IO,
//             .dout = I2S_GPIO_UNUSED,
//             .din = I2S_DO_IO,
//         },
//     };
    
//     std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT; 
    
//     ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
//     ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
// }

// // --- 4. Main Task ---
// void app_main(void) {
//     init_i2s();

//     if (dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE) != ESP_OK) {
//         ESP_LOGE("FFT", "FFT init failed");
//         return;
//     }

//     dsps_wind_hann_f32(window, FFT_SIZE);

//     // Important: Allocate a buffer twice the size because we are receiving STEREO data.
//     int32_t *raw_buffer = malloc(FFT_SIZE * 2 * sizeof(int32_t));
//     size_t bytes_read;

//     ESP_LOGI("Mic", "Audio acquisition system started, Sample Rate: %d Hz", SAMPLE_RATE);

//     while (1) {
//         // Read 1024 frames of stereo data (1 frame = 32-bit Left + 32-bit Right).
//         if (i2s_channel_read(rx_handle, raw_buffer, FFT_SIZE * 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) == ESP_OK) {
            
//             float mean = 0;
//             for (int i = 0; i < FFT_SIZE; i++) {
//                 // SPH0645 outputs on the left channel by default (SELECT pin to GND), 
//                 int32_t L_channel = raw_buffer[i * 2]; 
                
//                 // Arithmetic right shift by 14 bits: Remove invalid low-bit noise 
//                 L_channel >>= 14; 
//                 mean += (float)L_channel;
//             }
//             mean /= FFT_SIZE;

//             // Step B: Populate FFT input array (apply Hann window).
//             for (int i = 0; i < FFT_SIZE; i++) {
//                 int32_t L_channel = raw_buffer[i * 2] >> 14;
//                 float sample = (float)L_channel - mean; // Remove DC bias
                
//                 fft_input[i * 2]     = sample * window[i]; // Real part
//                 fft_input[i * 2 + 1] = 0;                  // Imaginary part
//             }

//             // Step C: Execute FFT and calculate frequency.
//             dsps_fft2r_fc32(fft_input, FFT_SIZE);
//             dsps_bit_rev_fc32(fft_input, FFT_SIZE);

//             float max_mag = 0;
//             int max_index = 0;
            
//             // Skip 0 Hz (DC) and iterate through the first half of the spectrum.
//             for (int i = 1; i < FFT_SIZE / 2; i++) {
//                 float real = fft_input[i * 2];
//                 float imag = fft_input[i * 2 + 1];
                
//                 // Calculate magnitude and apply standard FFT normalization (multiply by 2.0 / N).
//                 float mag = sqrtf(real * real + imag * imag) * 2.0f / FFT_SIZE;
                
//                 if (mag > max_mag) {
//                     max_mag = mag;
//                     max_index = i;
//                 }
//             }

//             // Calculate actual frequency.
//             float frequency = ((float)max_index * SAMPLE_RATE) / FFT_SIZE;

//             // The threshold here is evaluated after normalization 
//             // (max amplitude for 18-bit audio is around 131072).
//             // Set a reasonable noise floor filtering threshold (e.g., 200, adjust based on environmental noise).
//             if (max_mag > 200) { 
//                 ESP_LOGI("Mic", "Detected Freq: %6.1f Hz | Peak Mag: %.0f", frequency, max_mag);
//             }
//         }
        
//         vTaskDelay(pdMS_TO_TICKS(100)); // Increase response speed to 100ms.
//     }
    
//     free(raw_buffer);
// }

#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_dsp.h"
#include "sdkconfig.h"

// --- 1. Configuration Parameters ---
// 32kHz sample rate ensures a 2.048MHz BCLK, keeping the microphone stable.
#define SAMPLE_RATE     (32000)
#define FFT_SIZE        (1024)

// ESP32-C6 Pin Configuration
#define I2S_BCK_IO      (GPIO_NUM_19)
#define I2S_WS_IO       (GPIO_NUM_18)
#define I2S_DO_IO       (GPIO_NUM_23)

// --- CHANNEL SELECTION MACRO ---
// Set to 0: Read Left Channel (Assume SEL pin is connected to GND)
// Set to 1: Read Right Channel (Assume SEL pin is connected to VDD or floating high)
#define MIC_CHANNEL_OFFSET 0 

// --- 2. Global Variables ---
i2s_chan_handle_t rx_handle;
float fft_input[FFT_SIZE * 2];
float window[FFT_SIZE];

// --- 3. I2S Initialization ---
void init_i2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // Force STEREO mode and operate with 32-bit slot width to satisfy OSR=64 requirement.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DO_IO,
        },
    };
    
    // Enforce 64 OSR requirement (32 bits per channel).
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT; 
    
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
}

// --- 4. Main Task ---
void app_main(void) {
    init_i2s();

    if (dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE) != ESP_OK) {
        ESP_LOGE("FFT", "FFT init failed");
        return;
    }

    dsps_wind_hann_f32(window, FFT_SIZE);

    // Allocate a buffer twice the size because we are receiving STEREO data.
    int32_t *raw_buffer = malloc(FFT_SIZE * 2 * sizeof(int32_t));
    size_t bytes_read;

    ESP_LOGI("Mic", "Audio acquisition system started, Sample Rate: %d Hz", SAMPLE_RATE);

    while (1) {
        // Read 1024 frames of stereo data (1 frame = 32-bit Left + 32-bit Right).
        if (i2s_channel_read(rx_handle, raw_buffer, FFT_SIZE * 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) == ESP_OK) {
            
            // Step A: Extract single channel data and calculate DC offset.
            float mean = 0;
            for (int i = 0; i < FFT_SIZE; i++) {
                // Fetch data based on the channel offset macro
                int32_t channel_data = raw_buffer[i * 2 + MIC_CHANNEL_OFFSET]; 
                
                // Arithmetic right shift by 14 bits: Remove invalid low-bit noise 
                // and preserve the sign of the 18-bit two's complement data.
                channel_data >>= 14; 
                mean += (float)channel_data;
            }
            mean /= FFT_SIZE;

            // Step B: Populate FFT input array (apply Hann window).
            for (int i = 0; i < FFT_SIZE; i++) {
                int32_t channel_data = raw_buffer[i * 2 + MIC_CHANNEL_OFFSET] >> 14;
                float sample = (float)channel_data - mean; // Remove DC bias
                
                fft_input[i * 2]     = sample * window[i]; // Real part
                fft_input[i * 2 + 1] = 0;                  // Imaginary part
            }

            // Step C: Execute FFT.
            dsps_fft2r_fc32(fft_input, FFT_SIZE);
            dsps_bit_rev_fc32(fft_input, FFT_SIZE);

            // Step D: Output spectrum data to serial port for Python visualization.
            // Frequency resolution = 32000 / 1024 = 31.25 Hz/bin.
            // Output the first 127 bins (approx 0 ~ 4000 Hz) to cover the human voice range.
            printf("FFT_DATA:"); 
            for (int i = 1; i < 128; i++) { // Skip 0Hz (DC component)
                float real = fft_input[i * 2];
                float imag = fft_input[i * 2 + 1];
                
                // Calculate magnitude and apply standard FFT normalization
                float mag = sqrtf(real * real + imag * imag) * 2.0f / FFT_SIZE;
                
                // Output integer magnitude to speed up serial transmission
                printf("%.0f,", mag); 
            }
            printf("\n"); // End of data packet
        }
        
        // Short delay to maintain a high refresh rate for the Python plot
        vTaskDelay(pdMS_TO_TICKS(50)); 
    }
    
    free(raw_buffer);
}

// #include <stdio.h>
// #include <stdlib.h>
// #include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "driver/i2s_std.h"
// #include "driver/gpio.h"
// #include "esp_log.h"

// #define SAMPLE_RATE 16000
// #define RECORD_SECONDS 4
// #define THRESHOLD 300

// #define I2S_BCK_IO GPIO_NUM_19
// #define I2S_WS_IO GPIO_NUM_18
// #define I2S_DIN_IO GPIO_NUM_23

// #define MIC_CHANNEL_OFFSET 0

// i2s_chan_handle_t rx_handle;
// int16_t *audio_buffer;
// size_t buffer_len = SAMPLE_RATE * RECORD_SECONDS;

// void init_i2s() {
//     i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
//     ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

//     i2s_std_config_t std_cfg = {
//         .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
//         .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
//         .gpio_cfg = {
//             .mclk = I2S_GPIO_UNUSED,
//             .bclk = I2S_BCK_IO,
//             .ws = I2S_WS_IO,
//             .dout = I2S_GPIO_UNUSED,
//             .din = I2S_DIN_IO,
//         },
//     };
//     std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;

//     ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
//     ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
// }

// void app_main(void) {
//     init_i2s();

//     audio_buffer = malloc(buffer_len * sizeof(int16_t));
//     if (!audio_buffer) {
//         printf("CRITICAL ERROR: MALLOC FAILED!\n");
//         return;
//     }

//     int32_t raw_buffer[64];
//     size_t bytes_read;
//     float ema_average = 0;

//     for(int i = 0; i < 32000; i += 32) {
//         if (i2s_channel_read(rx_handle, raw_buffer, sizeof(raw_buffer), &bytes_read, portMAX_DELAY) == ESP_OK) {
//             for (int j = 0; j < 32; j++) {
//                 int32_t raw_val = raw_buffer[j * 2 + MIC_CHANNEL_OFFSET] >> 14;
//                 ema_average = (ema_average * 0.9999f) + (raw_val * 0.0001f);
//             }
//         }
//     }

//     printf("System Ready. Waiting for trigger...\n");

//     bool triggered = false;
//     int max_val_this_second = 0;
//     int samples_checked = 0;

//     while (!triggered) {
//         if (i2s_channel_read(rx_handle, raw_buffer, sizeof(raw_buffer), &bytes_read, portMAX_DELAY) == ESP_OK) {
//             for (int i = 0; i < 32; i++) {
//                 int32_t raw_val = raw_buffer[i * 2 + MIC_CHANNEL_OFFSET] >> 14;
//                 ema_average = (ema_average * 0.9999f) + (raw_val * 0.0001f);
//                 int32_t val = raw_val - (int32_t)ema_average;
                
//                 if (abs(val) > max_val_this_second) {
//                     max_val_this_second = abs(val);
//                 }

//                 if (abs(val) > THRESHOLD) {
//                     triggered = true;
//                     break;
//                 }

//                 samples_checked++;
//                 if (samples_checked >= SAMPLE_RATE) {
//                     printf("Current Max Volume: %d\n", max_val_this_second);
//                     max_val_this_second = 0;
//                     samples_checked = 0;
//                 }
//             }
//         }
//     }

//     printf(">>> VOICE DETECTED! RECORDING %d SECONDS... <<<\n", RECORD_SECONDS);
//     fflush(stdout);
//     vTaskDelay(pdMS_TO_TICKS(10));

//     size_t samples_recorded = 0;
//     while (samples_recorded < buffer_len) {
//         if (i2s_channel_read(rx_handle, raw_buffer, sizeof(raw_buffer), &bytes_read, portMAX_DELAY) == ESP_OK) {
//             for (int i = 0; i < 32 && samples_recorded < buffer_len; i++) {
//                 int32_t raw_val = raw_buffer[i * 2 + MIC_CHANNEL_OFFSET] >> 14;
//                 ema_average = (ema_average * 0.9999f) + (raw_val * 0.0001f);
//                 int32_t val = raw_val - (int32_t)ema_average;
                
//                 int32_t amplified_val = val * 30;
                
//                 if (amplified_val > 32767) amplified_val = 32767;
//                 if (amplified_val < -32768) amplified_val = -32768;
                
//                 audio_buffer[samples_recorded++] = (int16_t)amplified_val;
//             }
//         }
//     }

//     printf("---WAV_START---\n");
//     for (size_t i = 0; i < buffer_len; i++) {
//         printf("%d\n", audio_buffer[i]);
//         if (i % 1000 == 0) {
//             vTaskDelay(pdMS_TO_TICKS(5));
//         }
//     }
//     printf("---WAV_END---\n");
//     printf("Recording and transmission finished. System halted.\n");

//     while (1) {
//         vTaskDelay(pdMS_TO_TICKS(1000));
//     }
// }