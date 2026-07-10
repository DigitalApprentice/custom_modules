#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "py/runtime.h"
#include "py/obj.h"
#include "py/objarray.h"
#include "py/binary.h"

// Forward declaration of the type
extern const mp_obj_type_t aleds_rgb_type;

// ROM timing and order tuples
static const mp_rom_obj_tuple_t order_rgb_rom = {
    {&mp_type_tuple}, 3, { MP_ROM_INT(0), MP_ROM_INT(1), MP_ROM_INT(2) }
};
static const mp_rom_obj_tuple_t order_rbg_rom = {
    {&mp_type_tuple}, 3, { MP_ROM_INT(0), MP_ROM_INT(2), MP_ROM_INT(1) }
};
static const mp_rom_obj_tuple_t order_grb_rom = {
    {&mp_type_tuple}, 3, { MP_ROM_INT(1), MP_ROM_INT(0), MP_ROM_INT(2) }
};
static const mp_rom_obj_tuple_t order_gbr_rom = {
    {&mp_type_tuple}, 3, { MP_ROM_INT(2), MP_ROM_INT(0), MP_ROM_INT(1) }
};
static const mp_rom_obj_tuple_t order_brg_rom = {
    {&mp_type_tuple}, 3, { MP_ROM_INT(1), MP_ROM_INT(2), MP_ROM_INT(0) }
};
static const mp_rom_obj_tuple_t order_bgr_rom = {
    {&mp_type_tuple}, 3, { MP_ROM_INT(2), MP_ROM_INT(1), MP_ROM_INT(0) }
};
static const mp_rom_obj_tuple_t timing_800khz_rom = {
    {&mp_type_tuple}, 4, { MP_ROM_INT(400), MP_ROM_INT(850), MP_ROM_INT(800), MP_ROM_INT(450) }
};
static const mp_rom_obj_tuple_t timing_400khz_rom = {
    {&mp_type_tuple}, 4, { MP_ROM_INT(800), MP_ROM_INT(1700), MP_ROM_INT(1600), MP_ROM_INT(900) }
};

// Instance structure definition
typedef struct _mp_obj_aleds_rgb_t {
    mp_obj_base_t base;
    mp_obj_t pin;
    mp_int_t n;
    mp_int_t bpp;
    mp_obj_t led_type;
    mp_obj_t timing;
    mp_obj_t order;
    mp_obj_t order_buf;
    mp_obj_t tmp;
    mp_obj_t aled_buffer;
    
    // Cached raw pointers for maximum performance
    uint8_t *order_buf_ptr;
    size_t order_buf_len;
    uint8_t *tmp_ptr;
    size_t tmp_len;
} mp_obj_aleds_rgb_t;

// Constructor
static mp_obj_t aleds_rgb_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)type;
    
    // Parse arguments: pin, buffer, n, bpp=3, order=ORDER_RGB, timing=TIMING_800KHZ, led_type='ws2812'
    mp_arg_check_num(n_args, n_kw, 0, 7, true);
    
    mp_obj_t pin_obj = mp_const_none;
    mp_obj_t buffer_obj = mp_const_none;
    mp_obj_t n_obj = mp_const_none;
    mp_obj_t bpp_obj = mp_const_none;
    mp_obj_t order_obj = mp_const_none;
    mp_obj_t timing_obj = mp_const_none;
    mp_obj_t led_type_obj = mp_const_none;
    
    if (n_args > 0) pin_obj = args[0];
    if (n_args > 1) buffer_obj = args[1];
    if (n_args > 2) n_obj = args[2];
    if (n_args > 3) bpp_obj = args[3];
    if (n_args > 4) order_obj = args[4];
    if (n_args > 5) timing_obj = args[5];
    if (n_args > 6) led_type_obj = args[6];
    
    for (size_t i = 0; i < n_kw; i++) {
        mp_obj_t key = args[n_args + 2 * i];
        mp_obj_t value = args[n_args + 2 * i + 1];
        if (key == MP_OBJ_NEW_QSTR(MP_QSTR_pin)) {
            pin_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_buffer)) {
            buffer_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_n)) {
            n_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_bpp)) {
            bpp_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_order)) {
            order_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_timing)) {
            timing_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_led_type)) {
            led_type_obj = value;
        }
    }
    
    // Default values
    if (bpp_obj == mp_const_none) {
        bpp_obj = MP_OBJ_NEW_SMALL_INT(3);
    }
    if (order_obj == mp_const_none) {
        order_obj = MP_OBJ_FROM_PTR(&order_rgb_rom);
    }
    if (timing_obj == mp_const_none) {
        timing_obj = MP_OBJ_FROM_PTR(&timing_800khz_rom);
    }
    if (led_type_obj == mp_const_none) {
        led_type_obj = MP_OBJ_NEW_QSTR(MP_QSTR_ws2812);
    }
    
    if (pin_obj == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("missing required argument 'pin'"));
    }
    if (buffer_obj == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("missing required argument 'buffer'"));
    }
    if (n_obj == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("missing required argument 'n'"));
    }
    
    // self.pin = Pin(pin, Pin.OUT)
    mp_obj_t machine_module = mp_import_name(MP_QSTR_machine, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t pin_class = mp_load_attr(machine_module, MP_QSTR_Pin);
    mp_obj_t pin_out_mode = mp_load_attr(pin_class, MP_QSTR_OUT);
    mp_obj_t pin_args[2] = { pin_obj, pin_out_mode };
    mp_obj_t initialized_pin = mp_call_function_n_kw(pin_class, 2, 0, pin_args);
    
    // bytearray(order) and bytearray(bpp)
    mp_obj_t bytearray_class = MP_OBJ_FROM_PTR(&mp_type_bytearray);
    mp_obj_t order_buf = mp_call_function_1(bytearray_class, order_obj);
    mp_obj_t tmp_buf = mp_call_function_1(bytearray_class, bpp_obj);
    
    // Retrieve buffer information
    mp_buffer_info_t order_buf_info;
    mp_get_buffer_raise(order_buf, &order_buf_info, MP_BUFFER_READ | MP_BUFFER_WRITE);
    mp_buffer_info_t tmp_buf_info;
    mp_get_buffer_raise(tmp_buf, &tmp_buf_info, MP_BUFFER_READ | MP_BUFFER_WRITE);
    
    mp_obj_aleds_rgb_t *self = m_new_obj(mp_obj_aleds_rgb_t);
    self->base.type = &aleds_rgb_type;
    self->pin = initialized_pin;
    self->n = mp_obj_get_int(n_obj);
    self->bpp = mp_obj_get_int(bpp_obj);
    self->led_type = led_type_obj;
    self->timing = timing_obj;
    self->order = order_obj;
    self->order_buf = order_buf;
    self->tmp = tmp_buf;
    self->aled_buffer = buffer_obj;
    
    self->order_buf_ptr = (uint8_t *)order_buf_info.buf;
    self->order_buf_len = order_buf_info.len;
    self->tmp_ptr = (uint8_t *)tmp_buf_info.buf;
    self->tmp_len = tmp_buf_info.len;
    
    return MP_OBJ_FROM_PTR(self);
}

// set_buffer(buffer)
static mp_obj_t aleds_rgb_set_buffer(mp_obj_t self_in, mp_obj_t buffer) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    self->aled_buffer = buffer;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(aleds_rgb_set_buffer_obj, aleds_rgb_set_buffer);

// Subscript interface (__setitem__ / __getitem__)
static mp_obj_t aleds_rgb_subscr(mp_obj_t self_in, mp_obj_t index, mp_obj_t value) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t idx = mp_obj_get_int(index);
    if (idx < 0) {
        idx += self->n;
    }
    if (idx < 0 || idx >= self->n) {
        mp_raise_msg(&mp_type_IndexError, MP_ERROR_TEXT("LED index out of range"));
    }
    
    if (value == MP_OBJ_SENTINEL) {
        // Read LED (__getitem__)
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_READ);
        uint8_t *buf = (uint8_t *)bufinfo.buf;
        
        mp_int_t bpp = self->bpp;
        mp_int_t base = idx * bpp;
        
        self->tmp_ptr[self->order_buf_ptr[0]] = buf[base];
        self->tmp_ptr[self->order_buf_ptr[1]] = buf[base + 1];
        self->tmp_ptr[self->order_buf_ptr[2]] = buf[base + 2];
        if (bpp == 4) {
            self->tmp_ptr[self->order_buf_ptr[3]] = buf[base + 3];
        }
        
        mp_obj_t vals[4];
        for (mp_int_t j = 0; j < bpp; j++) {
            vals[j] = MP_OBJ_NEW_SMALL_INT(self->tmp_ptr[j]);
        }
        return mp_obj_new_tuple(bpp, vals);
    } else if (value == MP_OBJ_NULL) {
        return MP_OBJ_NULL; // Delete not supported
    } else {
        // Write LED (__setitem__)
        size_t len;
        mp_obj_t *items;
        mp_obj_get_array(value, &len, &items);
        if (len < 3) {
            mp_raise_ValueError(MP_ERROR_TEXT("color must have at least 3 elements"));
        }
        mp_int_t c0 = mp_obj_get_int(items[0]);
        mp_int_t c1 = mp_obj_get_int(items[1]);
        mp_int_t c2 = mp_obj_get_int(items[2]);
        
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
        uint8_t *buf = (uint8_t *)bufinfo.buf;
        
        mp_int_t base = idx * 3;
        buf[base + self->order_buf_ptr[0]] = c0;
        buf[base + self->order_buf_ptr[1]] = c1;
        buf[base + self->order_buf_ptr[2]] = c2;
        
        return mp_const_none;
    }

}

// _set_led_rgb(i, c0, c1, c2)
static mp_obj_t aleds_rgb_set_led_rgb(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t idx = mp_obj_get_int(args[1]);
    mp_int_t c0 = mp_obj_get_int(args[2]);
    mp_int_t c1 = mp_obj_get_int(args[3]);
    mp_int_t c2 = mp_obj_get_int(args[4]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    
    mp_int_t base = idx * 3;
    buf[base + self->order_buf_ptr[0]] = c0;
    buf[base + self->order_buf_ptr[1]] = c1;
    buf[base + self->order_buf_ptr[2]] = c2;
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_set_led_rgb_obj, 5, 5, aleds_rgb_set_led_rgb);

// _read_led(i)
static mp_obj_t aleds_rgb_read_led(mp_obj_t self_in, mp_obj_t i_obj) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t idx = mp_obj_get_int(i_obj);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_READ);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    
    mp_int_t bpp = self->bpp;
    mp_int_t base = idx * bpp;
    
    self->tmp_ptr[self->order_buf_ptr[0]] = buf[base];
    self->tmp_ptr[self->order_buf_ptr[1]] = buf[base + 1];
    self->tmp_ptr[self->order_buf_ptr[2]] = buf[base + 2];
    if (bpp == 4) {
        self->tmp_ptr[self->order_buf_ptr[3]] = buf[base + 3];
    }
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(aleds_rgb_read_led_obj, aleds_rgb_read_led);

// Unary operations (len())
static mp_obj_t aleds_rgb_unary_op(mp_unary_op_t op, mp_obj_t self_in) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    switch (op) {
        case MP_UNARY_OP_LEN:
            return MP_OBJ_NEW_SMALL_INT(self->n);
        default:
            return MP_OBJ_NULL;
    }
}

// _calc_brightness_to_tmp_rgb(r, g, b, bv)
static mp_obj_t aleds_rgb_calc_brightness_to_tmp_rgb(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t r = mp_obj_get_int(args[1]);
    mp_int_t g = mp_obj_get_int(args[2]);
    mp_int_t b = mp_obj_get_int(args[3]);
    mp_int_t bv = mp_obj_get_int(args[4]);
    
    self->tmp_ptr[0] = (r * bv) >> 8;
    self->tmp_ptr[1] = (g * bv) >> 8;
    self->tmp_ptr[2] = (b * bv) >> 8;
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_calc_brightness_to_tmp_rgb_obj, 5, 5, aleds_rgb_calc_brightness_to_tmp_rgb);

// change_brightness(color, brightness)
static mp_obj_t aleds_rgb_change_brightness(mp_obj_t self_in, mp_obj_t color, mp_obj_t br_obj) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t brightness = mp_obj_get_int(br_obj);
    if (brightness <= 0) {
        mp_obj_t zero_vals[3] = { MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_NEW_SMALL_INT(0) };
        return mp_obj_new_tuple(3, zero_vals);
    }
    if (brightness >= 255) {
        return color;
    }
    
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(color, &len, &items);
    if (len < 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("color must have at least 3 elements"));
    }
    mp_int_t r = mp_obj_get_int(items[0]);
    mp_int_t g = mp_obj_get_int(items[1]);
    mp_int_t b = mp_obj_get_int(items[2]);
    
    self->tmp_ptr[0] = (r * brightness) >> 8;
    self->tmp_ptr[1] = (g * brightness) >> 8;
    self->tmp_ptr[2] = (b * brightness) >> 8;
    
    mp_obj_t res_vals[3] = {
        MP_OBJ_NEW_SMALL_INT(self->tmp_ptr[0]),
        MP_OBJ_NEW_SMALL_INT(self->tmp_ptr[1]),
        MP_OBJ_NEW_SMALL_INT(self->tmp_ptr[2])
    };
    return mp_obj_new_tuple(3, res_vals);
}
static MP_DEFINE_CONST_FUN_OBJ_3(aleds_rgb_change_brightness_obj, aleds_rgb_change_brightness);

// apply_brightness_to_buffer(brightness)
static mp_obj_t aleds_rgb_apply_brightness_to_buffer(mp_obj_t self_in, mp_obj_t br_obj) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t b = mp_obj_get_int(br_obj);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    size_t length = bufinfo.len;
    
    for (size_t i = 0; i < length; i++) {
        buf[i] = (uint8_t)(((mp_int_t)buf[i] * b) >> 8);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(aleds_rgb_apply_brightness_to_buffer_obj, aleds_rgb_apply_brightness_to_buffer);

// _apply_gamma_to_tmp_rgb(r, g, b, brightness, gamma_table)
static mp_obj_t aleds_rgb_apply_gamma_to_tmp_rgb(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t r = mp_obj_get_int(args[1]);
    mp_int_t g = mp_obj_get_int(args[2]);
    mp_int_t b = mp_obj_get_int(args[3]);
    mp_int_t brightness = mp_obj_get_int(args[4]);
    mp_obj_t gamma_table = args[5];
    
    mp_buffer_info_t gammainfo;
    mp_get_buffer_raise(gamma_table, &gammainfo, MP_BUFFER_READ);
    uint8_t *gamma = (uint8_t *)gammainfo.buf;
    
    mp_int_t b_corr = gamma[brightness];
    self->tmp_ptr[0] = (r * b_corr) >> 8;
    self->tmp_ptr[1] = (g * b_corr) >> 8;
    self->tmp_ptr[2] = (b * b_corr) >> 8;
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_apply_gamma_to_tmp_rgb_obj, 6, 6, aleds_rgb_apply_gamma_to_tmp_rgb);

// apply_gamma(color, brightness, gamma_table)
static mp_obj_t aleds_rgb_apply_gamma(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_obj_t color = args[1];
    mp_int_t brightness = mp_obj_get_int(args[2]);
    mp_obj_t gamma_table = args[3];
    
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(color, &len, &items);
    if (len < 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("color must have at least 3 elements"));
    }
    mp_int_t r = mp_obj_get_int(items[0]);
    mp_int_t g = mp_obj_get_int(items[1]);
    mp_int_t b = mp_obj_get_int(items[2]);
    
    mp_buffer_info_t gammainfo;
    mp_get_buffer_raise(gamma_table, &gammainfo, MP_BUFFER_READ);
    uint8_t *gamma = (uint8_t *)gammainfo.buf;
    
    mp_int_t b_corr = gamma[brightness];
    self->tmp_ptr[0] = (r * b_corr) >> 8;
    self->tmp_ptr[1] = (g * b_corr) >> 8;
    self->tmp_ptr[2] = (b * b_corr) >> 8;
    
    mp_obj_t res_vals[3] = {
        MP_OBJ_NEW_SMALL_INT(self->tmp_ptr[0]),
        MP_OBJ_NEW_SMALL_INT(self->tmp_ptr[1]),
        MP_OBJ_NEW_SMALL_INT(self->tmp_ptr[2])
    };
    return mp_obj_new_tuple(3, res_vals);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_apply_gamma_obj, 4, 4, aleds_rgb_apply_gamma);

// apply_br_to_buffer(gamma_table, brightness)
static mp_obj_t aleds_rgb_apply_br_to_buffer(mp_obj_t self_in, mp_obj_t gamma_table, mp_obj_t br_obj) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    mp_int_t brightness = mp_obj_get_int(br_obj);
    
    mp_buffer_info_t gammainfo;
    mp_get_buffer_raise(gamma_table, &gammainfo, MP_BUFFER_READ);
    uint8_t *gamma = (uint8_t *)gammainfo.buf;
    mp_int_t b_corr = gamma[brightness];
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    size_t length = bufinfo.len;
    
    for (size_t i = 0; i < length; i++) {
        buf[i] = (uint8_t)(((mp_int_t)buf[i] * b_corr) >> 8);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(aleds_rgb_apply_br_to_buffer_obj, aleds_rgb_apply_br_to_buffer);

// apply_br_per_pixel(gamma, buffer, modulator)
static mp_obj_t aleds_rgb_apply_br_per_pixel(size_t n_args, const mp_obj_t *args) {
    mp_obj_t gamma = args[1];
    mp_obj_t buffer = args[2];
    mp_obj_t modulator = args[3];
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    
    mp_buffer_info_t modinfo;
    mp_get_buffer_raise(modulator, &modinfo, MP_BUFFER_READ);
    uint8_t *mod = (uint8_t *)modinfo.buf;
    size_t num_pixels = modinfo.len;
    
    mp_buffer_info_t gammainfo;
    mp_get_buffer_raise(gamma, &gammainfo, MP_BUFFER_READ);
    uint8_t *gamma_buf = (uint8_t *)gammainfo.buf;
    
    for (size_t i = 0; i < num_pixels; i++) {
        size_t idx = i * 3;
        mp_int_t b_corr = gamma_buf[mod[i]];
        buf[idx] = (uint8_t)(((mp_int_t)buf[idx] * b_corr) >> 8);
        buf[idx + 1] = (uint8_t)(((mp_int_t)buf[idx + 1] * b_corr) >> 8);
        buf[idx + 2] = (uint8_t)(((mp_int_t)buf[idx + 2] * b_corr) >> 8);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_apply_br_per_pixel_obj, 4, 4, aleds_rgb_apply_br_per_pixel);

// clear()
static mp_obj_t aleds_rgb_clear(mp_obj_t self_in) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    memset(bufinfo.buf, 0, self->n * self->bpp);
    
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(aleds_rgb_clear_obj, aleds_rgb_clear);

// aled_fast_fill(ba, ba_len, value)
static mp_obj_t aleds_rgb_aled_fast_fill(size_t n_args, const mp_obj_t *args) {
    mp_obj_t ba = args[1];
    mp_int_t ba_len = mp_obj_get_int(args[2]);
    mp_uint_t value = mp_obj_get_int(args[3]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(ba, &bufinfo, MP_BUFFER_WRITE);
    uint32_t *buf32 = (uint32_t *)bufinfo.buf;
    
    for (mp_int_t i = 0; i < ba_len; i++) {
        buf32[i] = value;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_aled_fast_fill_obj, 4, 4, aleds_rgb_aled_fast_fill);

// aled_fill(color)
static mp_obj_t aleds_rgb_aled_fill(mp_obj_t self_in, mp_obj_t color) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(color, &len, &items);
    if (len < 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("color must have at least 3 elements"));
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    
    mp_int_t c0 = mp_obj_get_int(items[self->order_buf_ptr[0]]);
    mp_int_t c1 = mp_obj_get_int(items[self->order_buf_ptr[1]]);
    mp_int_t c2 = mp_obj_get_int(items[self->order_buf_ptr[2]]);
    
    mp_int_t n = self->n;
    for (mp_int_t i = 0; i < n; i++) {
        mp_int_t idx = i * 3;
        buf[idx] = c0;
        buf[idx + 1] = c1;
        buf[idx + 2] = c2;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(aleds_rgb_aled_fill_obj, aleds_rgb_aled_fill);

// _aled_fill_rgb(c0, c1, c2)
static mp_obj_t aleds_rgb_aled_fill_rgb(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t c0 = mp_obj_get_int(args[1]);
    mp_int_t c1 = mp_obj_get_int(args[2]);
    mp_int_t c2 = mp_obj_get_int(args[3]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    
    mp_int_t n = self->n;
    for (mp_int_t i = 0; i < n; i++) {
        mp_int_t idx = i * 3;
        buf[idx] = c0;
        buf[idx + 1] = c1;
        buf[idx + 2] = c2;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_aled_fill_rgb_obj, 4, 4, aleds_rgb_aled_fill_rgb);

// set_all(color, brightness)
static mp_obj_t aleds_rgb_set_all(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_obj_t color = args[1];
    mp_int_t bv = (n_args >= 3) ? mp_obj_get_int(args[2]) : 255;
    
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(color, &len, &items);
    if (len < 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("color must have at least 3 elements"));
    }
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    
    mp_int_t c0 = mp_obj_get_int(items[self->order_buf_ptr[0]]);
    mp_int_t c1 = mp_obj_get_int(items[self->order_buf_ptr[1]]);
    mp_int_t c2 = mp_obj_get_int(items[self->order_buf_ptr[2]]);
    
    mp_int_t r = (c0 * bv) >> 8;
    mp_int_t g = (c1 * bv) >> 8;
    mp_int_t b = (c2 * bv) >> 8;
    
    mp_int_t n = self->n;
    for (mp_int_t i = 0; i < n; i++) {
        mp_int_t idx = i * 3;
        buf[idx] = r;
        buf[idx + 1] = g;
        buf[idx + 2] = b;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_set_all_obj, 2, 3, aleds_rgb_set_all);

// _set_all_rgb(c0, c1, c2, bv)
static mp_obj_t aleds_rgb_set_all_rgb(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t c0 = mp_obj_get_int(args[1]);
    mp_int_t c1 = mp_obj_get_int(args[2]);
    mp_int_t c2 = mp_obj_get_int(args[3]);
    mp_int_t bv = mp_obj_get_int(args[4]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *buf = (uint8_t *)bufinfo.buf;
    
    mp_int_t r = (c0 * bv) >> 8;
    mp_int_t g = (c1 * bv) >> 8;
    mp_int_t b = (c2 * bv) >> 8;
    
    mp_int_t n = self->n;
    for (mp_int_t i = 0; i < n; i++) {
        mp_int_t idx = i * 3;
        buf[idx] = r;
        buf[idx + 1] = g;
        buf[idx + 2] = b;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_set_all_rgb_obj, 5, 5, aleds_rgb_set_all_rgb);

// fast_gradient(color1, color2)
static mp_obj_t aleds_rgb_fast_gradient(mp_obj_t self_in, mp_obj_t color1, mp_obj_t color2) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    
    size_t len1, len2;
    mp_obj_t *items1, *items2;
    mp_obj_get_array(color1, &len1, &items1);
    mp_obj_get_array(color2, &len2, &items2);
    if (len1 < 3 || len2 < 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("colors must have at least 3 elements"));
    }
    
    mp_int_t r1 = mp_obj_get_int(items1[self->order_buf_ptr[0]]);
    mp_int_t g1 = mp_obj_get_int(items1[self->order_buf_ptr[1]]);
    mp_int_t b1 = mp_obj_get_int(items1[self->order_buf_ptr[2]]);
    
    mp_int_t r2 = mp_obj_get_int(items2[self->order_buf_ptr[0]]);
    mp_int_t g2 = mp_obj_get_int(items2[self->order_buf_ptr[1]]);
    mp_int_t b2 = mp_obj_get_int(items2[self->order_buf_ptr[2]]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *ba = (uint8_t *)bufinfo.buf;
    
    mp_int_t n_leds = self->n;
    mp_int_t n_minus_1 = n_leds - 1;
    if (n_minus_1 <= 0) {
        n_minus_1 = 1;
    }
    
    mp_int_t idx = 0;
    for (mp_int_t i = 0; i < n_leds; i++) {
        mp_int_t t = (i * 256) / n_minus_1;
        ba[idx]     = (uint8_t)(r1 + (((r2 - r1) * t) >> 8));
        ba[idx + 1] = (uint8_t)(g1 + (((g2 - g1) * t) >> 8));
        ba[idx + 2] = (uint8_t)(b1 + (((b2 - b1) * t) >> 8));
        idx += 3;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(aleds_rgb_fast_gradient_obj, aleds_rgb_fast_gradient);

// _aled_fast_gradient_rgb(ba, n_leds, r1, g1, b1, r2, g2, b2)
static mp_obj_t aleds_rgb_aled_fast_gradient_rgb(size_t n_args, const mp_obj_t *args) {
    mp_obj_t ba_obj = args[1];
    mp_int_t n_leds = mp_obj_get_int(args[2]);
    mp_int_t r1 = mp_obj_get_int(args[3]);
    mp_int_t g1 = mp_obj_get_int(args[4]);
    mp_int_t b1 = mp_obj_get_int(args[5]);
    mp_int_t r2 = mp_obj_get_int(args[6]);
    mp_int_t g2 = mp_obj_get_int(args[7]);
    mp_int_t b2 = mp_obj_get_int(args[8]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(ba_obj, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *ba = (uint8_t *)bufinfo.buf;
    
    mp_int_t n_minus_1 = n_leds - 1;
    if (n_minus_1 <= 0) {
        n_minus_1 = 1;
    }
    
    mp_int_t idx = 0;
    for (mp_int_t i = 0; i < n_leds; i++) {
        mp_int_t t = (i * 256) / n_minus_1;
        ba[idx]     = (uint8_t)(r1 + (((r2 - r1) * t) >> 8));
        ba[idx + 1] = (uint8_t)(g1 + (((g2 - g1) * t) >> 8));
        ba[idx + 2] = (uint8_t)(b1 + (((b2 - b1) * t) >> 8));
        idx += 3;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_aled_fast_gradient_rgb_obj, 9, 9, aleds_rgb_aled_fast_gradient_rgb);

// fast_fill_segment(start, end, color)
static mp_obj_t aleds_rgb_fast_fill_segment(size_t n_args, const mp_obj_t *args) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_int_t start = mp_obj_get_int(args[1]);
    mp_int_t end = mp_obj_get_int(args[2]);
    mp_obj_t color = args[3];
    
    if (start < 0 || end > self->n || start >= end) {
        mp_raise_ValueError(MP_ERROR_TEXT("Invalid segment"));
    }
    
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(color, &len, &items);
    if (len < 3) {
        mp_raise_ValueError(MP_ERROR_TEXT("color must have at least 3 elements"));
    }
    
    mp_int_t r = mp_obj_get_int(items[self->order_buf_ptr[0]]);
    mp_int_t g = mp_obj_get_int(items[self->order_buf_ptr[1]]);
    mp_int_t b = mp_obj_get_int(items[self->order_buf_ptr[2]]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(self->aled_buffer, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *ba = (uint8_t *)bufinfo.buf;
    
    mp_int_t idx = start * 3;
    mp_int_t end_idx = end * 3;
    while (idx < end_idx) {
        ba[idx]     = r;
        ba[idx + 1] = g;
        ba[idx + 2] = b;
        idx += 3;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_fast_fill_segment_obj, 4, 4, aleds_rgb_fast_fill_segment);

// _aled_fast_fill_segment_rgb(ba, start, end, r, g, b)
static mp_obj_t aleds_rgb_aled_fast_fill_segment_rgb(size_t n_args, const mp_obj_t *args) {
    mp_obj_t ba_obj = args[1];
    mp_int_t start = mp_obj_get_int(args[2]);
    mp_int_t end = mp_obj_get_int(args[3]);
    mp_int_t r = mp_obj_get_int(args[4]);
    mp_int_t g = mp_obj_get_int(args[5]);
    mp_int_t b = mp_obj_get_int(args[6]);
    
    mp_buffer_info_t bufinfo;
    mp_get_buffer_raise(ba_obj, &bufinfo, MP_BUFFER_WRITE);
    uint8_t *ba = (uint8_t *)bufinfo.buf;
    
    mp_int_t idx = start * 3;
    mp_int_t end_idx = end * 3;
    while (idx < end_idx) {
        ba[idx]     = r;
        ba[idx + 1] = g;
        ba[idx + 2] = b;
        idx += 3;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(aleds_rgb_aled_fast_fill_segment_rgb_obj, 7, 7, aleds_rgb_aled_fast_fill_segment_rgb);

// write()
static mp_obj_t aleds_rgb_write(mp_obj_t self_in) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    
    mp_obj_t machine_module = mp_import_name(MP_QSTR_machine, mp_const_none, MP_OBJ_NEW_SMALL_INT(0));
    mp_obj_t bitstream_fun = mp_load_attr(machine_module, MP_QSTR_bitstream);
    
    mp_obj_t bitstream_args[4] = {
        self->pin,
        MP_OBJ_NEW_SMALL_INT(0),
        self->timing,
        self->aled_buffer
    };
    
    mp_call_function_n_kw(bitstream_fun, 4, 0, bitstream_args);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(aleds_rgb_write_obj, aleds_rgb_write);

// round_coordinates(coordinates)
static mp_obj_t aleds_rgb_round_coordinates(mp_obj_t self_in, mp_obj_t coordinates) {
    (void)self_in;
    
    size_t len;
    mp_obj_t *items;
    mp_obj_get_array(coordinates, &len, &items);
    
    mp_obj_t rounded_list = mp_obj_new_list(len, NULL);
    
    for (size_t i = 0; i < len; i++) {
        size_t tuple_len;
        mp_obj_t *coords;
        mp_obj_get_array(items[i], &tuple_len, &coords);
        if (tuple_len < 2) {
            mp_raise_ValueError(MP_ERROR_TEXT("Coordinate pair must have x and y"));
        }
        
        mp_float_t x = mp_obj_get_float(coords[0]);
        mp_float_t y = mp_obj_get_float(coords[1]);
        
        mp_int_t rx = (mp_int_t)(x + 0.5);
        mp_int_t ry = (mp_int_t)(y + 0.5);
        
        mp_obj_t r_tuple_vals[2] = { MP_OBJ_NEW_SMALL_INT(rx), MP_OBJ_NEW_SMALL_INT(ry) };
        mp_obj_t r_tuple = mp_obj_new_tuple(2, r_tuple_vals);
        
        mp_obj_list_store(rounded_list, MP_OBJ_NEW_SMALL_INT(i), r_tuple);
    }
    
    return rounded_list;
}
static MP_DEFINE_CONST_FUN_OBJ_2(aleds_rgb_round_coordinates_obj, aleds_rgb_round_coordinates);

// Class locals dictionary definition
static const mp_rom_map_elem_t aleds_rgb_locals_dict_table[] = {
    // ROM tuples / timings
    { MP_ROM_QSTR(MP_QSTR_ORDER_RGB), MP_ROM_PTR(&order_rgb_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_RBG), MP_ROM_PTR(&order_rbg_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_GRB), MP_ROM_PTR(&order_grb_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_GBR), MP_ROM_PTR(&order_gbr_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_BRG), MP_ROM_PTR(&order_brg_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_BGR), MP_ROM_PTR(&order_bgr_rom) },
    { MP_ROM_QSTR(MP_QSTR_TIMING_800KHZ), MP_ROM_PTR(&timing_800khz_rom) },
    { MP_ROM_QSTR(MP_QSTR_TIMING_400KHZ), MP_ROM_PTR(&timing_400khz_rom) },
    { MP_ROM_QSTR(MP_QSTR_TYPE_WS2812), MP_ROM_QSTR(MP_QSTR_ws2812) },

    // Methods
    { MP_ROM_QSTR(MP_QSTR_set_buffer), MP_ROM_PTR(&aleds_rgb_set_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR__set_led_rgb), MP_ROM_PTR(&aleds_rgb_set_led_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR__read_led), MP_ROM_PTR(&aleds_rgb_read_led_obj) },
    { MP_ROM_QSTR(MP_QSTR__calc_brightness_to_tmp_rgb), MP_ROM_PTR(&aleds_rgb_calc_brightness_to_tmp_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_change_brightness), MP_ROM_PTR(&aleds_rgb_change_brightness_obj) },
    { MP_ROM_QSTR(MP_QSTR_apply_brightness_to_buffer), MP_ROM_PTR(&aleds_rgb_apply_brightness_to_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR__apply_gamma_to_tmp_rgb), MP_ROM_PTR(&aleds_rgb_apply_gamma_to_tmp_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_apply_gamma), MP_ROM_PTR(&aleds_rgb_apply_gamma_obj) },
    { MP_ROM_QSTR(MP_QSTR_apply_br_to_buffer), MP_ROM_PTR(&aleds_rgb_apply_br_to_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_apply_br_per_pixel), MP_ROM_PTR(&aleds_rgb_apply_br_per_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear), MP_ROM_PTR(&aleds_rgb_clear_obj) },
    { MP_ROM_QSTR(MP_QSTR_aled_fast_fill), MP_ROM_PTR(&aleds_rgb_aled_fast_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR_aled_fill), MP_ROM_PTR(&aleds_rgb_aled_fill_obj) },
    { MP_ROM_QSTR(MP_QSTR__aled_fill_rgb), MP_ROM_PTR(&aleds_rgb_aled_fill_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_all), MP_ROM_PTR(&aleds_rgb_set_all_obj) },
    { MP_ROM_QSTR(MP_QSTR__set_all_rgb), MP_ROM_PTR(&aleds_rgb_set_all_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_fast_gradient), MP_ROM_PTR(&aleds_rgb_fast_gradient_obj) },
    { MP_ROM_QSTR(MP_QSTR__aled_fast_gradient_rgb), MP_ROM_PTR(&aleds_rgb_aled_fast_gradient_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_fast_fill_segment), MP_ROM_PTR(&aleds_rgb_fast_fill_segment_obj) },
    { MP_ROM_QSTR(MP_QSTR__aled_fast_fill_segment_rgb), MP_ROM_PTR(&aleds_rgb_aled_fast_fill_segment_rgb_obj) },
    { MP_ROM_QSTR(MP_QSTR_write), MP_ROM_PTR(&aleds_rgb_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_round_coordinates), MP_ROM_PTR(&aleds_rgb_round_coordinates_obj) },
};
static MP_DEFINE_CONST_DICT(aleds_rgb_locals_dict, aleds_rgb_locals_dict_table);

// Attribute load and store handlers
static void aleds_rgb_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_aleds_rgb_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) { // Load
        if (attr == MP_QSTR_pin) {
            dest[0] = self->pin;
        } else if (attr == MP_QSTR_n) {
            dest[0] = mp_obj_new_int(self->n);
        } else if (attr == MP_QSTR_bpp) {
            dest[0] = mp_obj_new_int(self->bpp);
        } else if (attr == MP_QSTR_led_type) {
            dest[0] = self->led_type;
        } else if (attr == MP_QSTR_timing) {
            dest[0] = self->timing;
        } else if (attr == MP_QSTR_order) {
            dest[0] = self->order;
        } else if (attr == MP_QSTR__order_buf) {
            dest[0] = self->order_buf;
        } else if (attr == MP_QSTR__tmp) {
            dest[0] = self->tmp;
        } else if (attr == MP_QSTR_aled_buffer) {
            dest[0] = self->aled_buffer;
        } else {
            // Fallback to locals_dict for methods
            mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&aleds_rgb_locals_dict.map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
            if (elem != NULL) {
                mp_convert_member_lookup(self_in, mp_obj_get_type(self_in), elem->value, dest);
            }
        }
    } else if (dest[1] != MP_OBJ_NULL) { // Store
        if (attr == MP_QSTR_aled_buffer) {
            self->aled_buffer = dest[1];
            dest[0] = MP_OBJ_NULL; // Success
        } else if (attr == MP_QSTR_pin) {
            self->pin = dest[1];
            dest[0] = MP_OBJ_NULL;
        } else if (attr == MP_QSTR_n) {
            self->n = mp_obj_get_int(dest[1]);
            dest[0] = MP_OBJ_NULL;
        } else if (attr == MP_QSTR_bpp) {
            self->bpp = mp_obj_get_int(dest[1]);
            dest[0] = MP_OBJ_NULL;
        } else if (attr == MP_QSTR_led_type) {
            self->led_type = dest[1];
            dest[0] = MP_OBJ_NULL;
        } else if (attr == MP_QSTR_timing) {
            self->timing = dest[1];
            dest[0] = MP_OBJ_NULL;
        } else if (attr == MP_QSTR_order) {
            self->order = dest[1];
            dest[0] = MP_OBJ_NULL;
        } else if (attr == MP_QSTR__order_buf) {
            self->order_buf = dest[1];
            mp_buffer_info_t bufinfo;
            if (mp_get_buffer(self->order_buf, &bufinfo, MP_BUFFER_READ)) {
                self->order_buf_ptr = (uint8_t *)bufinfo.buf;
                self->order_buf_len = bufinfo.len;
            }
            dest[0] = MP_OBJ_NULL;
        } else if (attr == MP_QSTR__tmp) {
            self->tmp = dest[1];
            mp_buffer_info_t bufinfo;
            if (mp_get_buffer(self->tmp, &bufinfo, MP_BUFFER_READ)) {
                self->tmp_ptr = (uint8_t *)bufinfo.buf;
                self->tmp_len = bufinfo.len;
            }
            dest[0] = MP_OBJ_NULL;
        }
    }
}

// Define the class type
MP_DEFINE_CONST_OBJ_TYPE(
    aleds_rgb_type,
    MP_QSTR_AledsRgb,
    MP_TYPE_FLAG_NONE,
    make_new, aleds_rgb_make_new,
    attr, aleds_rgb_attr,
    subscr, aleds_rgb_subscr,
    unary_op, aleds_rgb_unary_op,
    locals_dict, &aleds_rgb_locals_dict
);

// Module registration table
static const mp_rom_map_elem_t aleds_rgb_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_aleds_rgb) },
    { MP_ROM_QSTR(MP_QSTR_AledsRgb), MP_ROM_PTR(&aleds_rgb_type) },
    
    // Class constants exposed at the module level
    { MP_ROM_QSTR(MP_QSTR_ORDER_RGB), MP_ROM_PTR(&order_rgb_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_RBG), MP_ROM_PTR(&order_rbg_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_GRB), MP_ROM_PTR(&order_grb_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_GBR), MP_ROM_PTR(&order_gbr_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_BRG), MP_ROM_PTR(&order_brg_rom) },
    { MP_ROM_QSTR(MP_QSTR_ORDER_BGR), MP_ROM_PTR(&order_bgr_rom) },
    { MP_ROM_QSTR(MP_QSTR_TIMING_800KHZ), MP_ROM_PTR(&timing_800khz_rom) },
    { MP_ROM_QSTR(MP_QSTR_TIMING_400KHZ), MP_ROM_PTR(&timing_400khz_rom) },
    { MP_ROM_QSTR(MP_QSTR_TYPE_WS2812), MP_ROM_QSTR(MP_QSTR_ws2812) },
};
static MP_DEFINE_CONST_DICT(aleds_rgb_module_globals, aleds_rgb_module_globals_table);

const mp_obj_module_t aleds_rgb_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&aleds_rgb_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_aleds_rgb, aleds_rgb_module);
