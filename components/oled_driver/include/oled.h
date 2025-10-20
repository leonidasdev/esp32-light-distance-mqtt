/*
 * oled.h
 *
 * Small wrapper around LVGL to initialize a tiny OLED display and provide a
 * couple of helpers to update on-screen values.
 */

#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Configuration required to initialize the OLED display. */
struct oled_init_config {
    int i2c_bus_port;
    int sda_pin;
    int scl_pin;
    int i2c_device_address;
    int width;  /* pixels */
    int height; /* pixels */
};

/** Initialize display and return LVGL display handle. Caller keeps ownership. */
lv_disp_t *init_oled(struct oled_init_config init_config);

/** Small collection of LVGL elements created by init helper. */
struct oled_lvgl_elements {
    lv_obj_t *voltage_label;
    lv_obj_t *ohm_label;
};

/** Create LVGL elements for the supplied display and return them. */
struct oled_lvgl_elements init_oled_lvl(lv_disp_t *display);

/** Update displayed voltage (integer millivolts or units used by app). */
void oled_set_voltage(struct oled_lvgl_elements elements, int voltage);

/** Update displayed resistance/ohms value. */
void oled_set_ohms(struct oled_lvgl_elements elements, int ohms);

#ifdef __cplusplus
}
#endif

#endif /* OLED_H */