/*
 * ir_core0.c — MicroPython user C module dla ESP32-S3
 * ESP-IDF v5.x, MicroPython v1.28
 *
 * UWAGA: Na ESP32-S3 z ESP-IDF 5.3.x istnieje bug (issue #17811):
 * kanał RMT samoczynnie traci stan "enabled" po odebraniu ramki.
 * Workaround: rmt_disable() + rmt_enable() przed każdym rmt_receive().
 */

#include "py/runtime.h"
#include "py/obj.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "ir_core1";

#define IR_RESOLUTION_HZ   1000000
#define IR_RX_BUFFER_SIZE  64
#define IR_RANGE_MIN_NS    1250UL
#define IR_RANGE_MAX_NS    12000000UL

static volatile bool     ir_ready        = false;
static volatile bool     ir_task_running = false;
static volatile uint32_t ir_raw_data     = 0;
static volatile uint8_t  ir_protocol     = 0;
static volatile bool     ir_long_press   = false;
static volatile uint32_t ir_first_press_t = 0;

static volatile uint32_t last_led_raw    = 0;
static volatile uint32_t last_led_time   = 0;
static volatile uint32_t last_nec_raw    = 0;
static volatile uint32_t last_nec_time   = 0;

static uint32_t          long_press_ms   = 600;

static rmt_channel_handle_t rx_channel  = NULL;
static TaskHandle_t          ir_task_h  = NULL;
static QueueHandle_t         recv_queue = NULL;

/* ================================================================
 * CALLBACK & DEKODERY
 * ================================================================ */

static bool IRAM_ATTR rmt_rx_done_cb(rmt_channel_handle_t chan,
                                      const rmt_rx_done_event_data_t *edata,
                                      void *user_ctx) {
    BaseType_t wakeup = pdFALSE;
    QueueHandle_t q   = (QueueHandle_t)user_ctx;
    xQueueSendFromISR(q, edata, &wakeup);   /* kopia struktury, nie wskaźnik */
    return wakeup == pdTRUE;
}

static bool decode_nec(const rmt_symbol_word_t *syms, int n, uint32_t *out_data) {
    if (n < 34) return false;
    uint32_t h_hi = syms[0].duration0;
    uint32_t h_lo = syms[0].duration1;
    if (h_hi < 8000 || h_hi > 10000) return false;
    if (h_lo < 3500 || h_lo > 5000)  return false;
    uint32_t data = 0;
    for (int i = 1; i <= 32; i++) {
        uint32_t mark  = syms[i].duration0;
        uint32_t space = syms[i].duration1;
        if (mark < 300 || mark > 900) return false;
        data |= ((space > 1000 ? 1U : 0U) << (i - 1));
    }
    uint8_t addr = (data >> 0) & 0xFF; uint8_t addr_inv = (data >> 8) & 0xFF;
    uint8_t cmd = (data >> 16) & 0xFF; uint8_t cmd_inv = (data >> 24) & 0xFF;
    if ((addr ^ addr_inv) != 0xFF || (cmd ^ cmd_inv) != 0xFF) return false;
    *out_data = data;
    return true;
}

static bool decode_nec_repeat(const rmt_symbol_word_t *syms, int n) {
    if (n < 2) return false;
    uint32_t h_hi = syms[0].duration0;
    uint32_t h_lo = syms[0].duration1;
    return (h_hi >= 8000 && h_hi <= 10000 && h_lo >= 1500 && h_lo <= 2800);
}

static bool decode_led(const rmt_symbol_word_t *syms, int n, uint32_t *out_data) {
    if (n < 34) return false;
    uint32_t h_hi = syms[0].duration0;
    uint32_t h_lo = syms[0].duration1;
    if (h_hi < 8000 || h_hi > 10000 || h_lo < 1500 || h_lo > 5000) return false;
    uint32_t data = 0;
    for (int i = 1; i <= 32; i++) {
        uint32_t mark = syms[i].duration0; uint32_t space = syms[i].duration1;
        if (mark < 200 || mark > 900) return false;
        data |= ((space > 1000 ? 1U : 0U) << (i - 1));
    }
    *out_data = data;
    return true;
}

/* ================================================================
 * WORKAROUND ESP32-S3 bug #17811
 *
 * Na ESP32-S3 / ESP-IDF 5.3.x kanał RMT traci stan "enabled" po
 * odebraniu każdej ramki (nie dzieje się tak na ESP32 / ESP32-C3).
 * Jedyne skuteczne obejście: wymuszone disable→enable przed każdym
 * wywołaniem rmt_receive(). rmt_disable() ignoruje błąd, bo kanał
 * może być już wyłączony.
 * ================================================================ */
static esp_err_t ir_arm_receive(rmt_symbol_word_t *buf, size_t buf_size,
                                 const rmt_receive_config_t *cfg) {
    rmt_disable(rx_channel);          /* ignorujemy błąd — może być już off */
    esp_err_t err = rmt_enable(rx_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_enable: %s", esp_err_to_name(err));
        return err;
    }
    return rmt_receive(rx_channel, buf, buf_size, cfg);
}

/* ================================================================
 * ZADANIE NA RDZENIU 0
 * ================================================================ */

static void ir_core1_task(void *arg) {
    ir_task_running = true;
    vTaskDelay(pdMS_TO_TICKS(50));

    static rmt_symbol_word_t raw_symbols[IR_RX_BUFFER_SIZE];
    rmt_receive_config_t rx_cfg = {
        .signal_range_min_ns = IR_RANGE_MIN_NS,
        .signal_range_max_ns = IR_RANGE_MAX_NS,
    };

    while (ir_task_running) {
        if (rx_channel == NULL) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        esp_err_t err = ir_arm_receive(raw_symbols, sizeof(raw_symbols), &rx_cfg);
        if (err != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        rmt_rx_done_event_data_t rx_data;
        if (!xQueueReceive(recv_queue, &rx_data, pdMS_TO_TICKS(200))) {
            /* 200 ms silence = button released; reset state so next press is fresh */
            ir_long_press = false;
            last_led_raw  = 0;
            last_led_time = 0;
            last_nec_raw  = 0;
            last_nec_time = 0;
            continue;
        }
        if (!ir_task_running) break;

        int n = (int)rx_data.num_symbols;
        const rmt_symbol_word_t *syms = rx_data.received_symbols;
        uint32_t decoded = 0;

        if (decode_nec(syms, n, &decoded)) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            last_nec_raw = decoded; last_nec_time = now;
            ir_first_press_t = now; ir_long_press = false;
            ir_raw_data = decoded; ir_protocol = 1; ir_ready = true;
            continue;
        }

        if (decode_nec_repeat(syms, n)) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            if (last_nec_raw != 0 && (now - last_nec_time) < 500) {
                last_nec_time = now;
                if (!ir_long_press && (now - ir_first_press_t) >= long_press_ms) {
                    ir_long_press = true;
                    ir_raw_data = last_nec_raw; ir_protocol = 1; ir_ready = true;
                }
            }
            continue;
        }

        if (decode_led(syms, n, &decoded)) {
            uint32_t now = xTaskGetTickCount() * portTICK_PERIOD_MS;
            bool is_repeat = (decoded == last_led_raw && (now - last_led_time) < 600);
            last_led_time = now;
            if (!is_repeat) {
                last_led_raw = decoded;
                ir_first_press_t = now; ir_long_press = false;
                ir_raw_data = decoded; ir_protocol = 2; ir_ready = true;
            } else if (!ir_long_press && (now - ir_first_press_t) >= long_press_ms) {
                ir_long_press = true;
                ir_raw_data = decoded; ir_protocol = 2; ir_ready = true;
            }
        }
    }
    ir_task_h = NULL;
    vTaskDelete(NULL);
}

/* ================================================================
 * CLEANUP
 * ================================================================ */

static void ir_cleanup(void) {
    if (ir_task_h != NULL) {
        ir_task_running = false;
        while (ir_task_h != NULL) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (rx_channel != NULL) {
        rmt_disable(rx_channel);
        rmt_del_channel(rx_channel);
        rx_channel = NULL;
    }
    if (recv_queue != NULL) {
        vQueueDelete(recv_queue);
        recv_queue = NULL;
    }
    ir_ready = false;
    ir_long_press = false;
    ir_first_press_t = 0;
    last_nec_raw = 0; last_nec_time = 0;
}

/* ================================================================
 * PYTHON API
 * ================================================================ */

static mp_obj_t ir_start(mp_obj_t pin_obj) {
    int pin = mp_obj_get_int(pin_obj);
    ir_cleanup();

    recv_queue = xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));

    rmt_rx_channel_config_t rx_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = IR_RESOLUTION_HZ,
        .mem_block_symbols = IR_RX_BUFFER_SIZE,
        .gpio_num = pin,
    };

    if (rmt_new_rx_channel(&rx_cfg, &rx_channel) != ESP_OK) {
        mp_raise_msg(&mp_type_RuntimeError, MP_ERROR_TEXT("RMT init failed"));
    }

    rmt_rx_event_callbacks_t cbs = { .on_recv_done = rmt_rx_done_cb };
    rmt_rx_register_event_callbacks(rx_channel, &cbs, recv_queue);
    rmt_enable(rx_channel);

    xTaskCreatePinnedToCore(ir_core1_task, "ir_c1", 4096, NULL, 5, &ir_task_h, 1);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ir_start_obj, ir_start);

static mp_obj_t ir_stop(void) {
    ir_cleanup();
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ir_stop_obj, ir_stop);

static mp_obj_t ir_available(void) {
    return mp_obj_new_bool(ir_ready);
}
static MP_DEFINE_CONST_FUN_OBJ_0(ir_available_obj, ir_available);

static mp_obj_t ir_read(void) {
    if (!ir_ready) return mp_const_none;
    mp_obj_t d = mp_obj_new_dict(4);
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_raw), mp_obj_new_int_from_uint(ir_raw_data));

    if (ir_protocol == 1) {
        uint8_t addr = (ir_raw_data >> 0) & 0xFF;
        uint8_t cmd  = (ir_raw_data >> 16) & 0xFF;
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_protocol), MP_OBJ_NEW_QSTR(MP_QSTR_NEC));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_address),  mp_obj_new_int(addr));
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_command),  mp_obj_new_int(cmd));
    } else {
        mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_protocol), MP_OBJ_NEW_QSTR(MP_QSTR_LED));
    }
    mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_long_press), mp_obj_new_bool(ir_long_press));

    ir_ready = false;
    return d;
}
static MP_DEFINE_CONST_FUN_OBJ_0(ir_read_obj, ir_read);

static mp_obj_t ir_set_long_press_ms(mp_obj_t ms_obj) {
    long_press_ms = (uint32_t)mp_obj_get_int(ms_obj);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ir_set_long_press_ms_obj, ir_set_long_press_ms);

/* ================================================================
 * REJESTRACJA MODUŁU
 * ================================================================ */

static const mp_rom_map_elem_t ir_globals[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ir_core1) },
    { MP_ROM_QSTR(MP_QSTR_start),    MP_ROM_PTR(&ir_start_obj) },
    { MP_ROM_QSTR(MP_QSTR_stop),     MP_ROM_PTR(&ir_stop_obj) },
    { MP_ROM_QSTR(MP_QSTR_available),          MP_ROM_PTR(&ir_available_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),               MP_ROM_PTR(&ir_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_long_press_ms),  MP_ROM_PTR(&ir_set_long_press_ms_obj) },
};
static MP_DEFINE_CONST_DICT(ir_globals_dict, ir_globals);

const mp_obj_module_t ir_core1_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&ir_globals_dict,
};
MP_REGISTER_MODULE(MP_QSTR_ir_core1, ir_core1_module);
