#ifndef HCSR04_H
#define HCSR04_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize HC-SR04 pins and internal state.
 * trigger_gpio: GPIO number connected to TRIG (output)
 * echo_gpio: GPIO number connected to ECHO (input)
 * Returns true on success.
 */
bool hcsr04_init(int trigger_gpio, int echo_gpio);

/**
 * Perform a single distance measurement. Blocks up to a timeout.
 * Returns true on success and writes distance in millimeters to out_mm.
 * If the measurement times out or fails, returns false.
 */
bool hcsr04_read_mm(uint32_t *out_mm);

#ifdef __cplusplus
}
#endif

#endif // HCSR04_H
