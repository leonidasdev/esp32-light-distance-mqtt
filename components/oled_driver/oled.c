/*
 * OLED driver wrapper using LVGL. The helpers initialize the display and
 * provide a tiny API to update on-screen labels. The implementation keeps
 * behaviour minimal and uses LVGL locking helpers already provided by the
 * project's LVGL port.
 */
#include "oled.h"

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_err.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "esp_lcd_sh1107.h"
#include "esp_lcd_panel_vendor.h"

#include "misc/lv_anim.h"
#include "misc/lv_area.h"
#include "misc/lv_color.h"
#include "misc/lv_txt.h"

static const char *TAG = "oled";

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//////////////////// Please update the following configuration according to your LCD spec //////////////////////////////
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
#define LCD_PIXEL_CLOCK_HZ (400 * 1000)
#define LCD_CMD_BITS 8

lv_disp_t *init_oled(struct oled_init_config init_config)
{
    ESP_LOGI(TAG, "Initializing OLED (i2c_port=%d, sda=%d, scl=%d, addr=0x%02x, %dx%d)",
             init_config.i2c_bus_port, init_config.sda_pin, init_config.scl_pin,
             init_config.i2c_device_address & 0xff, init_config.width, init_config.height);

    i2c_master_bus_handle_t i2c_bus = NULL;
    i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .i2c_port = init_config.i2c_bus_port,
        .sda_io_num = init_config.sda_pin,
        .scl_io_num = init_config.scl_pin,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = init_config.i2c_device_address,
        .scl_speed_hz = LCD_PIXEL_CLOCK_HZ,
        .control_phase_bytes = 1,
        .lcd_cmd_bits = LCD_CMD_BITS,
        .lcd_param_bits = LCD_CMD_BITS,
        .dc_bit_offset = 0,
        .flags = {
            .disable_control_phase = 1,
        }};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &io_config, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_sh1107(io_handle, &panel_config, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_port_init(&lvgl_cfg);

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = panel_handle,
        .buffer_size = init_config.width * init_config.height,
        .double_buffer = true,
        .hres = init_config.width,
        .vres = init_config.height,
        .monochrome = true,
        .rotation = {
            .swap_xy = false,
            .mirror_x = true,
            .mirror_y = true,
        }};
    lv_disp_t *disp = lvgl_port_add_disp(&disp_cfg);

    if (disp == NULL) {
        ESP_LOGE(TAG, "Failed to add LVGL display");
        return NULL;
    }

    /* Rotation of the screen */
    lv_disp_set_rotation(disp, LV_DISP_ROT_NONE);

    ESP_LOGI(TAG, "OLED initialized successfully");
    return disp;
}

struct oled_lvgl_elements init_oled_lvl(lv_disp_t *display)
{
    struct oled_lvgl_elements elements = {.ohm_label = NULL, .voltage_label = NULL};

    if (display == NULL) {
        ESP_LOGW(TAG, "init_oled_lvl called with NULL display");
        return elements;
    }

    if (!lvgl_port_lock(0))
    {
        ESP_LOGW(TAG, "Failed to lock LVGL for element creation");
        return elements;
    }

    lv_obj_t *scr = lv_disp_get_scr_act(display);
    if (scr == NULL) {
        ESP_LOGW(TAG, "No active screen available");
        lvgl_port_unlock();
        return elements;
    }

    lv_obj_t *voltage_label = lv_label_create(scr);
    lv_obj_t *ohm_label = lv_label_create(scr);

    if (voltage_label == NULL || ohm_label == NULL) {
        ESP_LOGW(TAG, "Failed to create LVGL labels");
        if (voltage_label) lv_obj_del(voltage_label);
        if (ohm_label) lv_obj_del(ohm_label);
        lvgl_port_unlock();
        return elements;
    }

    lv_label_set_text(voltage_label, "0 mV");
    lv_label_set_text(ohm_label, "0 Ohm");

    /* Size of the screen (if you use rotation 90 or 270, please set disp->driver->ver_res) */
    lv_obj_set_width(voltage_label, display->driver->hor_res);
    lv_obj_set_width(ohm_label, display->driver->hor_res);

    lv_obj_align(voltage_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_align(ohm_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    elements.voltage_label = voltage_label;
    elements.ohm_label = ohm_label;
    lvgl_port_unlock();

    return elements;
}

void oled_set_voltage(struct oled_lvgl_elements elements, int voltage)
{
    if (elements.voltage_label == NULL) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text_fmt(elements.voltage_label, "%d mV", voltage);
    lvgl_port_unlock();
}

void oled_set_ohms(struct oled_lvgl_elements elements, int ohms)
{
    if (elements.ohm_label == NULL) return;
    if (!lvgl_port_lock(0)) return;
    lv_label_set_text_fmt(elements.ohm_label, "%d Ohm", ohms);
    lvgl_port_unlock();
}