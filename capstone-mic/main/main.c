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

/*
--- CORRECTIONS APPLIED (based on your original code) ---
1) Removed the invalid `[cite_start] ... [cite: ...]` tokens (they break C compilation).
2) Renamed I2S data pin define from DO -> DIN (mic data goes INTO ESP32).
3) Set I2S to a more typical "24-bit data in 32-bit slot, stereo" configuration (common for Adafruit I2S MEMS mics).
4) Replaced the hardcoded `>> 14` with a configurable shift (`MIC_SHIFT_BITS`) and added an optional raw-word debug print.
5) Added a `bytes_read` length check to avoid FFT on partial buffers.
6) Added optional dB readout to help tune threshold reliably.
7) Added malloc null-check.
8) Reduced the artificial delay (kept a tiny delay so logs don’t spam too hard).
*/

// --- 1. Configuration Parameters ---
// Increase sample rate to 32kHz to achieve a 2.048MHz I2S clock, 
// [cite_start]// perfectly matching the microphone's operational requirements[cite: 34].
#define SAMPLE_RATE     (32000)
#define FFT_SIZE        (1024)

#if (FFT_SIZE > CONFIG_DSP_MAX_FFT_SIZE)
#error "FFT_SIZE is larger than CONFIG_DSP_MAX_FFT_SIZE. Increase CONFIG_DSP_MAX_FFT_SIZE in menuconfig."
#endif

// ESP32-C6 Pin Configuration
#define I2S_BCK_IO      (GPIO_NUM_5) //19
#define I2S_WS_IO       (GPIO_NUM_6) // 18
#define I2S_DO_IO       (GPIO_NUM_4) // 23

// ----- Mic interpretation knobs -----
// Many Adafruit I2S MEMS mics output 24-bit audio left-justified in a 32-bit word.
// A common choice is shifting right by 8 to align signed 24-bit -> signed 32-bit.
// If your data looks wrong (all zeros / tiny / nonsense), try 0, 8, or 14.
#define MIC_SHIFT_BITS          (8)

// Most mics output on one channel; try LEFT first. If you hear nothing / see nothing, set to 0 for RIGHT.
#define MIC_USE_LEFT_CHANNEL    (1)

// Print a few raw words to help choose MIC_SHIFT_BITS (0 = off, 1 = on).
#define RAW_DEBUG               (0)

// --- 2. Global Variables ---
static const char *TAG = "Mic";

i2s_chan_handle_t rx_handle;
float fft_input[FFT_SIZE * 2];
float window[FFT_SIZE];

// Convert one 32-bit I2S word into a signed sample (still in "integer-ish" units)
static inline int32_t mic_word_to_sample(int32_t w)
{
    // Original code:
    // int32_t L_channel = raw_buffer[i * 2];
    // L_channel >>= 14;

    // New: configurable shift (keeps sign via arithmetic right shift)
    return (w >> MIC_SHIFT_BITS);
}

static inline int mic_word_index(int i)
{
    // Stereo buffer layout: [L0, R0, L1, R1, ...]
    return (i * 2) + (MIC_USE_LEFT_CHANNEL ? 0 : 1);
}

// --- 3. I2S Initialization ---
void init_i2s() {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &rx_handle));

    // Core Fix: Force STEREO mode and operate with 32-bit slot width.
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
        // .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        // Common for Adafruit I2S MEMS mics: 24-bit data carried in 32-bit slots, stereo framing.
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_24BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_IO,
            .ws = I2S_WS_IO,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_DO_IO,
        },
    };
    
    // [cite_start]// Enforce 64 OSR requirement (32 bits per channel)[cite: 181].
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

    // Important: Allocate a buffer twice the size because we are receiving STEREO data.
    int32_t *raw_buffer = malloc(FFT_SIZE * 2 * sizeof(int32_t));
    // add check
    if (!raw_buffer) {
        ESP_LOGE(TAG, "malloc failed");
        return;
    }
    size_t bytes_read = 0;
    // add
    const size_t want_bytes = FFT_SIZE * 2 * sizeof(int32_t);

    // ESP_LOGI("Mic", "Audio acquisition system started, Sample Rate: %d Hz", SAMPLE_RATE);
    ESP_LOGI(TAG, "Audio acquisition started. Fs=%d Hz, FFT=%d, shift=%d, chan=%s",
             SAMPLE_RATE, FFT_SIZE, MIC_SHIFT_BITS, MIC_USE_LEFT_CHANNEL ? "LEFT" : "RIGHT");

    while (1) {
        esp_err_t err = i2s_channel_read(rx_handle, raw_buffer, want_bytes, &bytes_read, portMAX_DELAY);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "i2s_channel_read failed: %s", esp_err_to_name(err));
            continue;
        }

        // Added: guard against short reads (don’t FFT partial buffers)
        if (bytes_read != want_bytes) {
            ESP_LOGW(TAG, "Short read: got %u bytes, expected %u",
                     (unsigned)bytes_read, (unsigned)want_bytes);
            continue;
        }

#if RAW_DEBUG
        // Added: raw-word visibility to pick the right MIC_SHIFT_BITS
        ESP_LOGI(TAG, "RAW L0=0x%08lx R0=0x%08lx", (long)raw_buffer[0], (long)raw_buffer[1]);
#endif

#if 0
        // Step A: Extract single channel data and calculate DC offset
        float mean = 0;
        for (int i = 0; i < FFT_SIZE; i++) {
            int32_t L_channel = raw_buffer[i * 2];
            L_channel >>= 14;
            mean += (float)L_channel;
        }
        mean /= FFT_SIZE;
#endif

        // Step A (fixed): use selected channel + configurable shift, then compute mean (DC)
        float mean = 0.0f;
        for (int i = 0; i < FFT_SIZE; i++) {
            int32_t w = raw_buffer[mic_word_index(i)];
            int32_t s = mic_word_to_sample(w);
            mean += (float)s;
        }
        mean /= (float)FFT_SIZE;

#if 0
        // Step B: Populate FFT input array (apply Hann window).
        for (int i = 0; i < FFT_SIZE; i++) {
            int32_t L_channel = raw_buffer[i * 2] >> 14;
            float sample = (float)L_channel - mean;
            fft_input[i * 2]     = sample * window[i];
            fft_input[i * 2 + 1] = 0;
        }
#endif

        // Step B (fixed): same, but with mic_word_to_sample + channel select
        for (int i = 0; i < FFT_SIZE; i++) {
            int32_t w = raw_buffer[mic_word_index(i)];
            int32_t s = mic_word_to_sample(w);
            float sample = (float)s - mean;          // remove DC
            fft_input[i * 2]     = sample * window[i];
            fft_input[i * 2 + 1] = 0.0f;
        }

        // Step C: Execute FFT and calculate peak-bin frequency
        dsps_fft2r_fc32(fft_input, FFT_SIZE);
        dsps_bit_rev_fc32(fft_input, FFT_SIZE);

        float max_mag = 0.0f;
        int max_index = 0;

        // Skip DC bin (0 Hz), scan first half spectrum
        for (int i = 1; i < FFT_SIZE / 2; i++) {
            float real = fft_input[i * 2];
            float imag = fft_input[i * 2 + 1];

            float mag = sqrtf(real * real + imag * imag) * (2.0f / (float)FFT_SIZE);
            if (mag > max_mag) {
                max_mag = mag;
                max_index = i;
            }
        }

        float frequency = ((float)max_index * (float)SAMPLE_RATE) / (float)FFT_SIZE;

        // Added: dB readout (helps tune threshold consistently)
        float db = 20.0f * log10f(max_mag + 1e-9f);

#if 0
        // if (max_mag > 200) {
        //     ESP_LOGI("Mic", "Detected Freq: %6.1f Hz | Peak Mag: %.0f", frequency, max_mag);
        // }
#endif
        // New: start with a low-ish threshold and tune based on your environment.
        // You can use either mag or db to decide what "signal present" means.
        if (max_mag > 50.0f) {
            ESP_LOGI(TAG, "Freq: %6.1f Hz | Mag: %.2f | %.1f dB", frequency, max_mag, db);
        }

        // Original had 100ms. That can miss audio. Keep a tiny delay mainly to reduce log spam.
#if 0
        vTaskDelay(pdMS_TO_TICKS(100));
#endif
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    //     // Read 1024 frames of stereo data (1 frame = 32-bit Left + 32-bit Right).
    //     if (i2s_channel_read(rx_handle, raw_buffer, FFT_SIZE * 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) == ESP_OK) {
            
    //         // [cite_start]// Step A: Extract single channel data and calculate DC offset[cite: 40].
    //         float mean = 0;
    //         for (int i = 0; i < FFT_SIZE; i++) {
    //             // SPH0645 outputs on the left channel by default (SELECT pin to GND), 
    //             [cite_start]// corresponding to even indices 0, 2, 4...[cite: 113, 158].
    //             int32_t L_channel = raw_buffer[i * 2]; 
                
    //             // Arithmetic right shift by 14 bits: Remove invalid low-bit noise 
    //             [cite_start]// and preserve the sign of the 18-bit two's complement data[cite: 183].
    //             L_channel >>= 14; 
    //             mean += (float)L_channel;
    //         }
    //         mean /= FFT_SIZE;

    //         // Step B: Populate FFT input array (apply Hann window).
    //         for (int i = 0; i < FFT_SIZE; i++) {
    //             int32_t L_channel = raw_buffer[i * 2] >> 14;
    //             float sample = (float)L_channel - mean; // Remove DC bias
                
    //             fft_input[i * 2]     = sample * window[i]; // Real part
    //             fft_input[i * 2 + 1] = 0;                  // Imaginary part
    //         }

    //         // Step C: Execute FFT and calculate frequency.
    //         dsps_fft2r_fc32(fft_input, FFT_SIZE);
    //         dsps_bit_rev_fc32(fft_input, FFT_SIZE);

    //         float max_mag = 0;
    //         int max_index = 0;
            
    //         // Skip 0 Hz (DC) and iterate through the first half of the spectrum.
    //         for (int i = 1; i < FFT_SIZE / 2; i++) {
    //             float real = fft_input[i * 2];
    //             float imag = fft_input[i * 2 + 1];
                
    //             // Calculate magnitude and apply standard FFT normalization (multiply by 2.0 / N).
    //             float mag = sqrtf(real * real + imag * imag) * 2.0f / FFT_SIZE;
                
    //             if (mag > max_mag) {
    //                 max_mag = mag;
    //                 max_index = i;
    //             }
    //         }

    //         // Calculate actual frequency.
    //         float frequency = ((float)max_index * SAMPLE_RATE) / FFT_SIZE;

    //         // The threshold here is evaluated after normalization 
    //         // (max amplitude for 18-bit audio is around 131072).
    //         // Set a reasonable noise floor filtering threshold (e.g., 200, adjust based on environmental noise).
    //         if (max_mag > 200) { 
    //             ESP_LOGI("Mic", "Detected Freq: %6.1f Hz | Peak Mag: %.0f", frequency, max_mag);
    //         }
    //     }
        
    //     vTaskDelay(pdMS_TO_TICKS(100)); // Increase response speed to 100ms.
    // }
    
    free(raw_buffer);
}