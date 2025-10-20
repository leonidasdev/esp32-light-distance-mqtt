#include "hcsr04.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "hcsr04";
static int s_trigger = -1;
static int s_echo = -1;

bool hcsr04_init(int trigger_gpio, int echo_gpio)
{
    s_trigger = trigger_gpio;
    s_echo = echo_gpio;

    gpio_config_t trg_cfg = {
        .pin_bit_mask = (1ULL << trigger_gpio),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&trg_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure trigger gpio %d", trigger_gpio);
        return false;
    }

    gpio_config_t echo_cfg = {
        .pin_bit_mask = (1ULL << echo_gpio),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&echo_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure echo gpio %d", echo_gpio);
        return false;
    }

    // ensure trigger low
    gpio_set_level(trigger_gpio, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "Initialized HC-SR04 (trig=%d echo=%d)", trigger_gpio, echo_gpio);
    return true;
}

bool hcsr04_read_mm(uint32_t *out_mm)
{
    if (s_trigger < 0 || s_echo < 0) {
        ESP_LOGW(TAG, "hcsr04 not initialized");
        return false;
    }
    if (!out_mm) return false;

    // send 10us pulse
    gpio_set_level(s_trigger, 1);
    esp_rom_delay_us(10);
    gpio_set_level(s_trigger, 0);

    // wait for echo to go high (timeout 30ms)
    int64_t start = esp_timer_get_time();
    while (gpio_get_level(s_echo) == 0) {
        if ((esp_timer_get_time() - start) > 30000) {
            ESP_LOGW(TAG, "hcsr04 timeout waiting for echo high");
            return false;
        }
    // small yield
    esp_rom_delay_us(10);
    }
    int64_t t0 = esp_timer_get_time();
    // wait for echo low
    while (gpio_get_level(s_echo) == 1) {
        if ((esp_timer_get_time() - t0) > 30000) {
            ESP_LOGW(TAG, "hcsr04 timeout waiting for echo low");
            return false;
        }
    esp_rom_delay_us(10);
    }
    int64_t t1 = esp_timer_get_time();
    int64_t pulse_us = t1 - t0;
    // distance (mm) = pulse_us * 0.343 / 2
    double dist_mm = ((double)pulse_us) * 0.343 / 2.0;
    *out_mm = (uint32_t)dist_mm;
    ESP_LOGI(TAG, "hcsr04 pulse=%lld us dist=%lu mm", (long long)pulse_us, (unsigned long)*out_mm);
    return true;
}
