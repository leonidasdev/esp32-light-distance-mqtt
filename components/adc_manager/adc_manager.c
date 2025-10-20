/*
 * adc_manager.c
 *
 * Small wrapper around the ADC oneshot and calibration APIs. Provides a
 * simple handle-based lifecycle and helpers to read raw/calibrated voltages
 * and compute resistance for an LDR circuit.
 */
#include "adc_manager.h"
#include "esp_log.h"
#include <stdlib.h>

static const char *TAG = "adc_manager";

adc_manager_handle_t *adc_manager_init(adc_channel_t channel, adc_atten_t atten)
{
    adc_manager_handle_t *handle = calloc(1, sizeof(adc_manager_handle_t));
    if (handle == NULL)
    {
        ESP_LOGE(TAG, "Failed to allocate memory for handle");
        return NULL;
    }

    handle->channel = channel;
    handle->calibrated = false;

    // Initialize ADC
    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = ADC_UNIT_1,
    };
    if (adc_oneshot_new_unit(&init_config, &handle->adc_handle) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to create ADC unit");
        free(handle);
        return NULL;
    }

    // Configure ADC
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_DEFAULT,
        .atten = atten,
    };
    if (adc_oneshot_config_channel(handle->adc_handle, channel, &config) != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to configure ADC channel");
        adc_oneshot_del_unit(handle->adc_handle);
        free(handle);
        return NULL;
    }

    // Initialize calibration
    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = ADC_UNIT_1,
        .atten = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_line_fitting(&cali_config, &handle->cali_handle) == ESP_OK)
    {
        handle->calibrated = true;
        ESP_LOGI(TAG, "ADC calibration enabled");
    }
    else
    {
        ESP_LOGW(TAG, "ADC calibration not available");
    }

    return handle;
}

esp_err_t adc_manager_read_raw(adc_manager_handle_t *handle, int *raw_value)
{
    if (handle == NULL || raw_value == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    if (handle->adc_handle == NULL) return ESP_ERR_INVALID_STATE;
    return adc_oneshot_read(handle->adc_handle, handle->channel, raw_value);
}

esp_err_t adc_manager_read_voltage(adc_manager_handle_t *handle, int *voltage)
{
    if (handle == NULL || voltage == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }

    int raw_value;
    esp_err_t ret = adc_manager_read_raw(handle, &raw_value);
    if (ret != ESP_OK)
    {
        return ret;
    }

    if (handle->calibrated)
    {
        return adc_cali_raw_to_voltage(handle->cali_handle, raw_value, voltage);
    }
    else
    {
        *voltage = raw_value;
        return ESP_OK;
    }
}

int adc_manager_calc_ohm(int raw_value)
{
    /* Ensure floating point division and clamp raw range */
    if (raw_value < 0) raw_value = 0;
    if (raw_value > 4095) raw_value = 4095;
    double frac = ((double)raw_value) / 4095.0;
    return (int)(ADC_LDR_MAX_OHM - (ADC_LDR_MAX_OHM - ADC_LDR_MIN_OHM) * frac);
}

void adc_manager_deinit(adc_manager_handle_t *handle)
{
    if (handle == NULL)
    {
        return;
    }

    if (handle->calibrated)
    {
        adc_cali_delete_scheme_line_fitting(handle->cali_handle);
    }

    if (handle->adc_handle) adc_oneshot_del_unit(handle->adc_handle);
    free(handle);
}