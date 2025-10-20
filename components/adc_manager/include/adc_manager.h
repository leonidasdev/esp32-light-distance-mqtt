#ifndef ADC_MANAGER_H
#define ADC_MANAGER_H

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ADC_LDR_MIN_OHM 1000
#define ADC_LDR_MAX_OHM 1000000

/**
 * @brief ADC manager handle structure
 */
typedef struct
{
    adc_oneshot_unit_handle_t adc_handle;
    adc_channel_t channel;
    adc_cali_handle_t cali_handle;
    bool calibrated;
} adc_manager_handle_t;

/**
 * @brief Initialize ADC manager
 * @param channel ADC channel to use
 * @param atten ADC attenuation
 * @return ADC manager handle
 */
adc_manager_handle_t *adc_manager_init(adc_channel_t channel, adc_atten_t atten);

/**
 * @brief Read raw ADC value
 * @param handle ADC manager handle
 * @param raw_value Pointer to store raw ADC value
 * @return ESP_OK on success
 */
esp_err_t adc_manager_read_raw(adc_manager_handle_t *handle, int *raw_value);

/**
 * @brief Read calibrated voltage value in mV
 * @param handle ADC manager handle
 * @param voltage Pointer to store voltage value
 * @return ESP_OK on success
 */
esp_err_t adc_manager_read_voltage(adc_manager_handle_t *handle, int *voltage);

/**
 * @brief Calculate resistance from raw ADC value for LDR
 * @param raw_value Raw ADC value
 * @return Calculated resistance in ohms
 */
int adc_manager_calc_ohm(int raw_value);

/**
 * @brief Deinitialize ADC manager and free resources
 * @param handle ADC manager handle
 */
void adc_manager_deinit(adc_manager_handle_t *handle);

#endif /* ADC_MANAGER_H */

#ifdef __cplusplus
}
#endif