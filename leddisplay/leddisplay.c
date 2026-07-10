// leddisplay.c – Complete LED Display System for MicroPython (ESP32-S3)
// Fully compatible with cl_lds.py, supports font loading, matrix resize, scroll direction.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "py/runtime.h"
#include "py/obj.h"
#include "py/objlist.h"
#include "py/objtuple.h"
#include "py/objstr.h"
#include "py/binary.h"
#include "py/mphal.h"
#include "extmod/modmachine.h"
#include "font_4x7.h"

// Forward declaration of the type
extern const mp_obj_type_t led_display_type;

// ========== Constants ==========
#define PRES_ST 0
#define PRES_SQ 1
#define AN_N  0
#define AN_F  1
#define AN_FT 2
#define AN_SU 3
#define AN_SD 4
#define AN_SL 5
#define AN_SR 6
#define SCROLL_P 0
#define SCROLL_C 1
#define SCROLL_LOOP 2
#define MAX_ZONES 8
#define MAX_CHARS_PER_ZONE 64
#define MAX_MSG_PER_ZONE 16
#define MAX_MSG_LEN 64
#define MAX_BLINK_CHARS 8
#define MAX_BLINK_POS 16

// ========== Type definitions ==========
typedef struct { uint8_t r,g,b; } rgb_t;

typedef struct {
    uint16_t max_chars; uint8_t char_height; uint8_t spacing;
    uint16_t scroll_pos; int16_t pixel_offset;
    char scroll_text[MAX_MSG_LEN]; uint16_t text_len;
    bool active; bool hold_active; bool start_hold_active; bool started;
    uint32_t last_scroll_time; uint32_t scroll_delay_ms;
    uint32_t end_hold_ms; uint32_t start_hold_ms;
    uint32_t start_hold_start; uint32_t hold_start;
    uint8_t scroll_mode; uint8_t pixel_step;
    uint8_t scroll_direction; // 0=up,1=down,2=left,3=right
    char current_cache[MAX_MSG_LEN];
} scroll_manager_t;

typedef struct {
    char old_char; char new_char;
    uint8_t anim_type; uint32_t duration_ms; uint32_t start_time;
    bool active; float progress; int8_t last_step;
} char_animation_t;

// zone_type: 0=custom, 1=time
typedef struct zone_config {
    char name[16];
    uint16_t start_row; uint16_t end_row; uint16_t height;
    uint8_t brightness; uint8_t zone_type;
    rgb_t fg_color; rgb_t bg_color;
    uint8_t presentation; uint8_t anim_type;
    uint32_t anim_duration; uint16_t anim_stagger;
    bool scroll_enabled;
    scroll_manager_t scroll_mgr;
    char messages[MAX_MSG_PER_ZONE][MAX_MSG_LEN];
    uint8_t msg_count;
    uint8_t seq_index; uint32_t seq_last_change; uint32_t seq_duration;
    bool blink_enabled; bool blink_zone; bool blink_on;
    char blink_chars[MAX_BLINK_CHARS]; uint8_t blink_char_count;
    uint16_t blink_positions[MAX_BLINK_POS]; uint8_t blink_pos_count;
    uint32_t blink_period; uint32_t blink_last_toggle;
    char last_text[MAX_MSG_LEN]; char scroll_source_last[MAX_MSG_LEN];
    char_animation_t animations[MAX_CHARS_PER_ZONE];
    uint8_t text_align; // 0=top, 1=center, 2=bottom
    bool zone_enabled;
    uint8_t zone_alpha;
} zone_config_t;

typedef struct color_section {
    uint16_t start; uint16_t end; rgb_t color;
    struct color_section *next;
} color_section_t;

typedef struct _led_display_obj_t {
    mp_obj_base_t base;
    mp_obj_t led_device;
    mp_buffer_info_t led_buffer;
    bool led_buffer_valid;
    uint16_t rows; uint8_t cols; uint8_t render_direction;
    uint8_t *framebuffer;
    uint16_t fb_rows; uint8_t fb_cols;
    uint8_t brightness;
    // Font management
    uint8_t font_width; uint8_t font_height; uint8_t font_spacing;
    const uint16_t *font_data;
    struct { char name[16]; uint8_t width, height, spacing, start_char, end_char; const uint16_t *data; } fonts[8];
    uint8_t font_count;
    uint8_t current_font_idx;
    // Color management
    rgb_t *char_colors; uint16_t char_colors_len;
    color_section_t *color_sections;
    // Zones
    zone_config_t zones[MAX_ZONES];
    uint8_t zone_count;
    // Global animation
    uint8_t global_anim_type; uint32_t global_anim_duration; uint16_t global_anim_stagger;
    // Copy flags
    bool write_black_pixels; bool clear_led_before_copy;
    uint8_t copy_alpha;
    // Timing
    uint32_t last_update; uint32_t update_interval;
    uint16_t current_zone_row_offset;
} led_display_obj_t;

// Proxy objects for zones and color manager
typedef struct _led_display_zone_obj_t {
    mp_obj_base_t base;
    led_display_obj_t *disp;
    uint8_t zone_idx;
} led_display_zone_obj_t;

typedef struct _led_display_color_manager_obj_t {
    mp_obj_base_t base;
    led_display_obj_t *disp;
} led_display_color_manager_obj_t;

extern const mp_obj_type_t led_display_zone_type;
extern const mp_obj_type_t led_display_color_manager_type;

// ========== Function prototypes ==========
static void scroll_manager_init(scroll_manager_t *sm, uint16_t max_chars, uint8_t char_height, uint8_t spacing);
static void scroll_manager_set_text(scroll_manager_t *sm, const char *text);
static void scroll_manager_update_text_in_place(scroll_manager_t *sm, const char *text);
static void scroll_manager_update(scroll_manager_t *sm, char *out_text);
static int16_t scroll_manager_get_pixel_offset(scroll_manager_t *sm);
static bool scroll_manager_is_ready(scroll_manager_t *sm);
static void char_animation_start(char_animation_t *anim, char old_char, char new_char, uint8_t type, uint32_t duration, uint32_t start_delay);
static void char_animation_update(char_animation_t *anim);
static uint16_t* get_char_data(led_display_obj_t *disp, char ch);
static rgb_t get_char_color(led_display_obj_t *disp, uint16_t pos, zone_config_t *zone, rgb_t default_color);
static void fb_clear(led_display_obj_t *disp);
static void fb_pixel_rgb(led_display_obj_t *disp, uint16_t row, uint8_t col, uint8_t r, uint8_t g, uint8_t b);
static void render_char_fast(led_display_obj_t *disp, uint16_t row, uint16_t *char_data, uint8_t width, uint8_t height, rgb_t color, uint8_t alpha);
static void render_char_partial(led_display_obj_t *disp, uint16_t row, uint16_t *old_data, uint16_t *new_data, uint8_t width, uint8_t height, rgb_t color, uint8_t alpha, uint8_t mode, float progress);
static void render_text_vertical(led_display_obj_t *disp, const char *text, int16_t start_row, rgb_t default_color, zone_config_t *zone, int16_t clip_top, int16_t clip_bottom);
static void render_text_vertical_animated(led_display_obj_t *disp, const char *text, uint16_t start_row, rgb_t default_color, zone_config_t *zone);
static void apply_blink_mask(led_display_obj_t *disp, zone_config_t *zone, const char *text, int16_t start_row);
static void zone_update_blink(zone_config_t *zone);
static void change_text_animated(led_display_obj_t *disp, const char *new_text, const char *old_text, zone_config_t *zone);
static void compose_zone_text(led_display_obj_t *self, zone_config_t *zone, mp_obj_t controller, char *out_text, size_t out_len);
static void copy_to_led_buffer(led_display_obj_t *disp, bool do_clear);
static void set_char_color(led_display_obj_t *self, uint16_t pos, uint8_t r, uint8_t g, uint8_t b);
static void set_section_color(led_display_obj_t *self, uint16_t start, uint16_t end, uint8_t r, uint8_t g, uint8_t b);
static void clear_all_colors(led_display_obj_t *self);

#define LED_DISPLAY_DEFINE_FUN_OBJ_4(obj_name, fun_name) \
    static mp_obj_t fun_name##_var(size_t n_args, const mp_obj_t *args) { \
        (void)n_args; \
        return fun_name(args[0], args[1], args[2], args[3]); \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(obj_name, 4, 4, fun_name##_var)

#define LED_DISPLAY_DEFINE_FUN_OBJ_5(obj_name, fun_name) \
    static mp_obj_t fun_name##_var(size_t n_args, const mp_obj_t *args) { \
        (void)n_args; \
        return fun_name(args[0], args[1], args[2], args[3], args[4]); \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(obj_name, 5, 5, fun_name##_var)

#define LED_DISPLAY_DEFINE_FUN_OBJ_6(obj_name, fun_name) \
    static mp_obj_t fun_name##_var(size_t n_args, const mp_obj_t *args) { \
        (void)n_args; \
        return fun_name(args[0], args[1], args[2], args[3], args[4], args[5]); \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(obj_name, 6, 6, fun_name##_var)

#define LED_DISPLAY_DEFINE_FUN_OBJ_7(obj_name, fun_name) \
    static mp_obj_t fun_name##_var(size_t n_args, const mp_obj_t *args) { \
        (void)n_args; \
        return fun_name(args[0], args[1], args[2], args[3], args[4], args[5], args[6]); \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(obj_name, 7, 7, fun_name##_var)

#define LED_DISPLAY_DEFINE_FUN_OBJ_8(obj_name, fun_name) \
    static mp_obj_t fun_name##_var(size_t n_args, const mp_obj_t *args) { \
        (void)n_args; \
        return fun_name(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]); \
    } \
    static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(obj_name, 8, 8, fun_name##_var)

static mp_obj_t led_display_kw_get(mp_map_t *kw_args, qstr key, mp_obj_t default_value) {
    (void)key;
    if (kw_args == NULL) {
        return default_value;
    }
    mp_map_elem_t *elem = mp_map_lookup(kw_args, MP_OBJ_NEW_QSTR(key), MP_MAP_LOOKUP);
    return elem == NULL ? default_value : elem->value;
}

// ========== Scroll Manager ==========
static void scroll_manager_init(scroll_manager_t *sm, uint16_t max_chars, uint8_t char_height, uint8_t spacing) {
    memset(sm, 0, sizeof(scroll_manager_t));
    sm->max_chars = max_chars;
    sm->char_height = char_height;
    sm->spacing = spacing;
    sm->scroll_delay_ms = 200;
    sm->pixel_step = 1;
    sm->end_hold_ms = 800;
    sm->start_hold_ms = 500;
    sm->scroll_mode = SCROLL_P;
    sm->scroll_direction = 0; // up
    sm->active = false;
}
static void scroll_manager_set_text(scroll_manager_t *sm, const char *text) {
    strncpy(sm->scroll_text, text, MAX_MSG_LEN-1);
    sm->scroll_text[MAX_MSG_LEN-1] = '\0';
    sm->text_len = strlen(text);
    sm->pixel_offset = 0;
    sm->hold_active = false;
    sm->started = false;
    sm->active = (sm->text_len > sm->max_chars);
    if (sm->scroll_mode == SCROLL_LOOP && sm->scroll_direction == 1 && sm->active) {
        sm->scroll_pos = sm->text_len - sm->max_chars;
    } else {
        sm->scroll_pos = 0;
    }
    if (sm->active && sm->start_hold_ms > 0) {
        sm->start_hold_active = true;
        sm->start_hold_start = mp_hal_ticks_ms();
    } else {
        sm->start_hold_active = false;
    }
    strncpy(sm->current_cache, text, sm->max_chars);
    sm->current_cache[sm->max_chars] = '\0';
}
static void scroll_manager_update_text_in_place(scroll_manager_t *sm, const char *text) {
    strncpy(sm->scroll_text, text, MAX_MSG_LEN-1);
    sm->scroll_text[MAX_MSG_LEN-1] = '\0';
    if (sm->start_hold_active) {
        strncpy(sm->current_cache, sm->scroll_text, sm->max_chars);
        sm->current_cache[sm->max_chars] = '\0';
    } else if (sm->hold_active) {
        if (sm->scroll_direction == 0) {
            uint16_t max_start = (sm->text_len > sm->max_chars) ? (sm->text_len - sm->max_chars) : 0;
            strncpy(sm->current_cache, sm->scroll_text + max_start, sm->max_chars);
        } else {
            strncpy(sm->current_cache, sm->scroll_text, sm->max_chars);
        }
        sm->current_cache[sm->max_chars] = '\0';
    } else {
        if (sm->scroll_direction == 0) {
            if (sm->scroll_pos < sm->text_len) {
                strncpy(sm->current_cache, sm->scroll_text + sm->scroll_pos, sm->max_chars);
            } else {
                sm->current_cache[0] = '\0';
            }
        } else {
            int16_t start = (int16_t)sm->text_len - sm->max_chars - sm->scroll_pos;
            if (start < 0) start = 0;
            strncpy(sm->current_cache, sm->scroll_text + start, sm->max_chars);
        }
        sm->current_cache[sm->max_chars] = '\0';
    }
}
static void scroll_manager_update(scroll_manager_t *sm, char *out_text) {
    if (!sm->active) {
        strncpy(out_text, sm->scroll_text, sm->max_chars);
        out_text[sm->max_chars] = '\0';
        return;
    }
    uint32_t now = mp_hal_ticks_ms();
    if (sm->start_hold_active) {
        if (now - sm->start_hold_start >= sm->start_hold_ms) {
            sm->start_hold_active = false;
        } else {
            strncpy(out_text, sm->current_cache, sm->max_chars);
            out_text[sm->max_chars] = '\0';
            return;
        }
    }
    if (sm->hold_active) {
        if (now - sm->hold_start >= sm->end_hold_ms) {
            sm->hold_active = false;
        }
        strncpy(out_text, sm->current_cache, sm->max_chars);
        out_text[sm->max_chars] = '\0';
        return;
    }
    if (sm->scroll_mode == SCROLL_LOOP) {
        uint16_t max_start = (sm->text_len > sm->max_chars) ? (sm->text_len - sm->max_chars) : 0;
        if (sm->started && sm->scroll_pos == max_start && sm->pixel_offset == 0 && sm->end_hold_ms > 0 && !sm->hold_active) {
            sm->hold_active = true;
            sm->hold_start = now;
            strncpy(out_text, sm->scroll_text + max_start, sm->max_chars);
            out_text[sm->max_chars] = '\0';
            strncpy(sm->current_cache, out_text, MAX_MSG_LEN);
            return;
        }
        if (now - sm->last_scroll_time >= sm->scroll_delay_ms) {
            sm->last_scroll_time = now;
            sm->started = true;
            int16_t delta = (sm->scroll_direction == 1) ? -(int16_t)sm->pixel_step : (int16_t)sm->pixel_step;
            sm->pixel_offset += delta;
            int16_t limit = sm->char_height + sm->spacing;
            if (sm->pixel_offset >= limit) {
                sm->pixel_offset = 0;
                sm->scroll_pos = (sm->scroll_pos < max_start) ? sm->scroll_pos + 1 : 0;
            } else if (sm->pixel_offset < 0) {
                sm->pixel_offset = limit - 1;
                sm->scroll_pos = (sm->scroll_pos > 0) ? sm->scroll_pos - 1 : max_start;
            }
        }
        strncpy(out_text, sm->scroll_text + sm->scroll_pos, sm->max_chars);
        out_text[sm->max_chars] = '\0';
        strncpy(sm->current_cache, out_text, MAX_MSG_LEN);
        return;
    }
    if (now - sm->last_scroll_time >= sm->scroll_delay_ms) {
        sm->last_scroll_time = now;
        sm->started = true;
        if (sm->scroll_mode == SCROLL_P) {
            int16_t delta = sm->pixel_step;
            if (sm->scroll_direction == 1) delta = -delta;
            sm->pixel_offset += delta;
            int16_t limit = sm->char_height + sm->spacing;
            if (sm->pixel_offset >= limit) {
                sm->pixel_offset = 0;
                sm->scroll_pos++;
            } else if (sm->pixel_offset < 0) {
                sm->pixel_offset = limit - 1;
                if (sm->scroll_pos > 0) sm->scroll_pos--;
            }
        } else {
            if (sm->scroll_direction == 0) sm->scroll_pos++;
            else if (sm->scroll_direction == 1 && sm->scroll_pos > 0) sm->scroll_pos--;
        }
    }
    uint16_t max_start = (sm->text_len > sm->max_chars) ? (sm->text_len - sm->max_chars) : 0;
    if (sm->scroll_direction == 0) { // up
        if (sm->started && sm->scroll_pos == max_start && sm->pixel_offset == 0 && !sm->hold_active) {
            sm->hold_active = true; sm->hold_start = now;
            strncpy(out_text, sm->scroll_text + max_start, sm->max_chars);
            out_text[sm->max_chars] = '\0';
            strncpy(sm->current_cache, out_text, MAX_MSG_LEN);
            return;
        }
        if (sm->scroll_pos < sm->text_len) {
            strncpy(out_text, sm->scroll_text + sm->scroll_pos, sm->max_chars);
        } else out_text[0] = '\0';
    } else { // down
        int16_t start = (int16_t)sm->text_len - sm->max_chars - sm->scroll_pos;
        if (start < 0) start = 0;
        if (sm->started && start == 0 && sm->pixel_offset == 0 && !sm->hold_active) {
            sm->hold_active = true; sm->hold_start = now;
            strncpy(out_text, sm->scroll_text, sm->max_chars);
            out_text[sm->max_chars] = '\0';
            strncpy(sm->current_cache, out_text, MAX_MSG_LEN);
            return;
        }
        strncpy(out_text, sm->scroll_text + start, sm->max_chars);
    }
    out_text[sm->max_chars] = '\0';
    strncpy(sm->current_cache, out_text, MAX_MSG_LEN);
}
static int16_t scroll_manager_get_pixel_offset(scroll_manager_t *sm) { return sm->pixel_offset; }
static bool scroll_manager_is_ready(scroll_manager_t *sm) {
    if (!sm->active) return true;
    uint16_t max_start = (sm->text_len > sm->max_chars) ? (sm->text_len - sm->max_chars) : 0;
    if (sm->scroll_mode == SCROLL_LOOP) {
        return (sm->scroll_pos >= max_start && sm->pixel_offset == 0 && !sm->hold_active);
    }
    return (sm->started && sm->scroll_pos >= max_start && sm->pixel_offset == 0 && !sm->hold_active);
}

// ========== Animation helpers ==========
static void char_animation_start(char_animation_t *anim, char old_char, char new_char, uint8_t type, uint32_t duration, uint32_t start_delay) {
    anim->old_char = old_char;
    anim->new_char = new_char;
    anim->anim_type = type;
    anim->duration_ms = duration;
    anim->start_time = mp_hal_ticks_ms() + start_delay;
    anim->active = true;
    anim->progress = 0.0f;
    anim->last_step = -1;
}
static void char_animation_update(char_animation_t *anim) {
    if (!anim->active) return;
    uint32_t now = mp_hal_ticks_ms();
    if (now < anim->start_time) {
        anim->progress = 0.0f;
        return;
    }
    uint32_t elapsed = now - anim->start_time;
    if (elapsed >= anim->duration_ms) {
        anim->active = false;
        anim->progress = 1.0f;
    } else {
        anim->progress = (float)elapsed / anim->duration_ms;
    }
}

// ========== Font and color helpers ==========
static uint16_t* get_char_data(led_display_obj_t *disp, char ch) {
    int idx = (int)ch - disp->fonts[disp->current_font_idx].start_char;
    if (idx < 0 || idx >= (disp->fonts[disp->current_font_idx].end_char - disp->fonts[disp->current_font_idx].start_char + 1)) idx = 0;
    return (uint16_t*)(disp->fonts[disp->current_font_idx].data + idx * disp->font_width);
}
static rgb_t get_char_color(led_display_obj_t *disp, uint16_t pos, zone_config_t *zone, rgb_t default_color) {
    (void)zone;
    if (pos < disp->char_colors_len) {
        rgb_t c = disp->char_colors[pos];
        if (!(default_color.r == 255 && default_color.g == 255 && default_color.b == 255) &&
            (c.r == 255 && c.g == 255 && c.b == 255)) return default_color;
        if (c.r != 0 || c.g != 0 || c.b != 0) return c;
    }
    color_section_t *s = disp->color_sections;
    while (s) {
        if (pos >= s->start && pos <= s->end) return s->color;
        s = s->next;
    }
    return default_color;
}

// ========== Framebuffer operations ==========
static void fb_clear(led_display_obj_t *disp) {
    memset(disp->framebuffer, 0, disp->fb_rows * disp->fb_cols * 3);
}
static void fb_pixel_rgb(led_display_obj_t *disp, uint16_t row, uint8_t col, uint8_t r, uint8_t g, uint8_t b) {
    if (row < disp->fb_rows && col < disp->fb_cols) {
        uint32_t idx = (row * disp->fb_cols + col) * 3;
        disp->framebuffer[idx] = r;
        disp->framebuffer[idx+1] = g;
        disp->framebuffer[idx+2] = b;
    }
}

// ========== Fast char rendering with brightness and alpha ==========
static void render_char_fast(led_display_obj_t *disp, uint16_t row, uint16_t *char_data,
                             uint8_t width, uint8_t height, rgb_t color, uint8_t alpha) {
    uint8_t brightness = disp->brightness;
    uint32_t factor = ((uint32_t)alpha * brightness) >> 8;
    uint8_t r = (color.r * factor) >> 8;
    uint8_t g = (color.g * factor) >> 8;
    uint8_t b = (color.b * factor) >> 8;
    for (uint8_t col = 0; col < width; col++) {
        uint16_t col_bits = char_data[col];
        for (uint8_t bit = 0; bit < height; bit++) {
            if (col_bits & (1 << bit)) {
                uint16_t pr = row + bit;
                if (pr < disp->fb_rows && col < disp->fb_cols) {
                    uint32_t idx = (pr * disp->fb_cols + col) * 3;
                    disp->framebuffer[idx] = r;
                    disp->framebuffer[idx+1] = g;
                    disp->framebuffer[idx+2] = b;
                }
            }
        }
    }
}

// ========== Partial character rendering for animations ==========
static void render_char_partial(led_display_obj_t *disp, uint16_t row, uint16_t *old_data, uint16_t *new_data,
                                uint8_t width, uint8_t height, rgb_t color, uint8_t alpha,
                                uint8_t mode, float progress) {
    int8_t current_step;
    if (mode == AN_SL || mode == AN_SR) {
        current_step = (int8_t)(progress * (width + 1));
    } else {
        current_step = (int8_t)(progress * (height + 1));
    }
    uint8_t brightness = disp->brightness;
    uint32_t factor = ((uint32_t)alpha * brightness) >> 8;
    uint8_t r = (color.r * factor) >> 8;
    uint8_t g = (color.g * factor) >> 8;
    uint8_t b = (color.b * factor) >> 8;
    #define SET_PIXEL(pr, pc) do { \
        if (pr < disp->fb_rows && pc < disp->fb_cols) { \
            uint32_t idx = (pr * disp->fb_cols + pc) * 3; \
            disp->framebuffer[idx] = r; \
            disp->framebuffer[idx+1] = g; \
            disp->framebuffer[idx+2] = b; \
        } \
    } while(0)
    if (mode == AN_F) {
        if (old_data) {
            uint8_t a_old = (uint8_t)(alpha * (1.0f - progress));
            uint32_t f_old = ((uint32_t)a_old * brightness) >> 8;
            uint8_t r_old = (color.r * f_old) >> 8;
            uint8_t g_old = (color.g * f_old) >> 8;
            uint8_t b_old = (color.b * f_old) >> 8;
            for (uint8_t col = 0; col < width; col++) {
                uint16_t bits = old_data[col];
                for (uint8_t bit = 0; bit < height; bit++) {
                    if (bits & (1 << bit)) {
                        uint16_t pr = row + bit;
                        if (pr < disp->fb_rows && col < disp->fb_cols) {
                            uint32_t idx = (pr * disp->fb_cols + col) * 3;
                            disp->framebuffer[idx] = r_old;
                            disp->framebuffer[idx+1] = g_old;
                            disp->framebuffer[idx+2] = b_old;
                        }
                    }
                }
            }
        }
        if (new_data) {
            uint8_t a_new = (uint8_t)(alpha * progress);
            uint32_t f_new = ((uint32_t)a_new * brightness) >> 8;
            uint8_t r_new = (color.r * f_new) >> 8;
            uint8_t g_new = (color.g * f_new) >> 8;
            uint8_t b_new = (color.b * f_new) >> 8;
            for (uint8_t col = 0; col < width; col++) {
                uint16_t bits = new_data[col];
                for (uint8_t bit = 0; bit < height; bit++) {
                    if (bits & (1 << bit)) {
                        uint16_t pr = row + bit;
                        if (pr < disp->fb_rows && col < disp->fb_cols) {
                            uint32_t idx = (pr * disp->fb_cols + col) * 3;
                            disp->framebuffer[idx] = r_new;
                            disp->framebuffer[idx+1] = g_new;
                            disp->framebuffer[idx+2] = b_new;
                        }
                    }
                }
            }
        }
        return;
    }
    if (mode == AN_FT) {
        if (progress < 0.5f) {
            float t = progress * 2.0f;
            uint8_t a = (uint8_t)(alpha * (1.0f - t));
            uint32_t f = ((uint32_t)a * brightness) >> 8;
            uint8_t rr = (color.r * f) >> 8;
            uint8_t gg = (color.g * f) >> 8;
            uint8_t bb = (color.b * f) >> 8;
            if (old_data) {
                for (uint8_t col = 0; col < width; col++) {
                    uint16_t bits = old_data[col];
                    for (uint8_t bit = 0; bit < height; bit++) {
                        if (bits & (1 << bit)) {
                            uint16_t pr = row + bit;
                            if (pr < disp->fb_rows && col < disp->fb_cols) {
                                uint32_t idx = (pr * disp->fb_cols + col) * 3;
                                disp->framebuffer[idx] = rr;
                                disp->framebuffer[idx+1] = gg;
                                disp->framebuffer[idx+2] = bb;
                            }
                        }
                    }
                }
            }
        } else {
            float t = (progress - 0.5f) * 2.0f;
            uint8_t a = (uint8_t)(alpha * t);
            uint32_t f = ((uint32_t)a * brightness) >> 8;
            uint8_t rr = (color.r * f) >> 8;
            uint8_t gg = (color.g * f) >> 8;
            uint8_t bb = (color.b * f) >> 8;
            if (new_data) {
                for (uint8_t col = 0; col < width; col++) {
                    uint16_t bits = new_data[col];
                    for (uint8_t bit = 0; bit < height; bit++) {
                        if (bits & (1 << bit)) {
                            uint16_t pr = row + bit;
                            if (pr < disp->fb_rows && col < disp->fb_cols) {
                                uint32_t idx = (pr * disp->fb_cols + col) * 3;
                                disp->framebuffer[idx] = rr;
                                disp->framebuffer[idx+1] = gg;
                                disp->framebuffer[idx+2] = bb;
                            }
                        }
                    }
                }
            }
        }
        return;
    }
    uint16_t combined[width];
    memset(combined, 0, width * sizeof(uint16_t));
    if (mode == AN_SU) {
        if (current_step <= 0) { if (old_data) memcpy(combined, old_data, width * sizeof(uint16_t)); }
        else if (current_step >= (int8_t)(height + 1)) { if (new_data) memcpy(combined, new_data, width * sizeof(uint16_t)); }
        else {
            if (old_data) for (uint8_t col=0; col<width; col++) {
                uint16_t old = old_data[col];
                for (uint8_t bit=0; bit<height-current_step; bit++)
                    if (old & (1 << (current_step+bit))) combined[col] |= (1 << bit);
            }
            if (new_data && current_step>=1) for (uint8_t col=0; col<width; col++) {
                uint16_t newb = new_data[col];
                for (uint8_t bit=0; bit<current_step; bit++)
                    if (newb & (1 << bit)) combined[col] |= (1 << (height-current_step+bit));
            }
        }
    } else if (mode == AN_SD) {
        if (current_step <= 0) { if (old_data) memcpy(combined, old_data, width * sizeof(uint16_t)); }
        else if (current_step >= (int8_t)(height + 1)) { if (new_data) memcpy(combined, new_data, width * sizeof(uint16_t)); }
        else {
            if (new_data && current_step>=1) for (uint8_t col=0; col<width; col++) {
                uint16_t newb = new_data[col];
                for (uint8_t bit=0; bit<current_step; bit++)
                    if (newb & (1 << (height-current_step+bit))) combined[col] |= (1 << bit);
            }
            if (old_data) for (uint8_t col=0; col<width; col++) {
                uint16_t old = old_data[col];
                for (uint8_t bit=0; bit<height-current_step; bit++)
                    if (old & (1 << bit)) combined[col] |= (1 << (current_step+bit));
            }
        }
    } else if (mode == AN_SL) {
        if (current_step <= 0) { if (old_data) memcpy(combined, old_data, width * sizeof(uint16_t)); }
        else if (current_step >= (int8_t)(width + 1)) { if (new_data) memcpy(combined, new_data, width * sizeof(uint16_t)); }
        else {
            if (old_data) for (uint8_t col=0; col<width-current_step; col++) combined[col] = old_data[col+current_step];
            if (new_data && current_step>=1) for (uint8_t col=0; col<current_step; col++)
                combined[width-current_step+col] = new_data[col];
        }
    } else if (mode == AN_SR) {
        if (current_step <= 0) { if (old_data) memcpy(combined, old_data, width * sizeof(uint16_t)); }
        else if (current_step >= (int8_t)(width + 1)) { if (new_data) memcpy(combined, new_data, width * sizeof(uint16_t)); }
        else {
            if (new_data && current_step>=1) for (uint8_t col=0; col<current_step; col++)
                combined[col] = new_data[width-current_step+col];
            if (old_data) for (uint8_t col=0; col<width-current_step; col++)
                combined[current_step+col] = old_data[col];
        }
    } else {
        if (old_data && !new_data) memcpy(combined, old_data, width * sizeof(uint16_t));
        else if (new_data) memcpy(combined, new_data, width * sizeof(uint16_t));
        else return;
    }
    for (uint8_t col = 0; col < width; col++) {
        uint16_t bits = combined[col];
        for (uint8_t bit = 0; bit < height; bit++) {
            if (bits & (1 << bit)) {
                uint16_t pr = row + bit;
                if (pr < disp->fb_rows && col < disp->fb_cols) {
                    uint32_t idx = (pr * disp->fb_cols + col) * 3;
                    disp->framebuffer[idx] = r;
                    disp->framebuffer[idx+1] = g;
                    disp->framebuffer[idx+2] = b;
                }
            }
        }
    }
}

// ========== Text rendering without animation (with clipping) ==========
static void render_text_vertical(led_display_obj_t *disp, const char *text, int16_t start_row,
                                 rgb_t default_color, zone_config_t *zone, int16_t clip_top, int16_t clip_bottom) {
    uint8_t char_h = disp->font_height, char_w = disp->font_width, spacing = disp->font_spacing;
    rgb_t bg = zone ? zone->bg_color : (rgb_t){0,0,0};
    int16_t row = start_row;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        rgb_t color = get_char_color(disp, i, zone, default_color);
        uint16_t *char_data = get_char_data(disp, ch);
        uint8_t brightness = disp->brightness;
        uint8_t r = (color.r * brightness) >> 8;
        uint8_t g = (color.g * brightness) >> 8;
        uint8_t b = (color.b * brightness) >> 8;
        uint8_t bg_r = (bg.r * brightness) >> 8;
        uint8_t bg_g = (bg.g * brightness) >> 8;
        uint8_t bg_b = (bg.b * brightness) >> 8;
        for (uint8_t bit = 0; bit < char_h; bit++) {
            int16_t pr = row + bit;
            if (pr >= (int16_t)disp->fb_rows) break;
            if (clip_top >= 0 && (pr < clip_top || pr >= clip_bottom)) continue;
            for (uint8_t col = 0; col < char_w; col++) fb_pixel_rgb(disp, pr, col, bg_r, bg_g, bg_b);
        }
        for (uint8_t col = 0; col < char_w; col++) {
            uint16_t col_bits = char_data[col];
            for (uint8_t bit = 0; bit < char_h; bit++) {
                if (col_bits & (1 << bit)) {
                    int16_t pr = row + bit;
                    if (pr >= (int16_t)disp->fb_rows) continue;
                    if (clip_top >= 0 && (pr < clip_top || pr >= clip_bottom)) continue;
                    fb_pixel_rgb(disp, pr, col, r, g, b);
                }
            }
        }
        row += char_h + spacing;
        if (row >= (int16_t)disp->fb_rows) break;
    }
}

// ========== Animated text rendering ==========
static void render_text_vertical_animated(led_display_obj_t *disp, const char *text, uint16_t start_row,
                                          rgb_t default_color, zone_config_t *zone) {
    uint8_t char_h = disp->font_height, char_w = disp->font_width, spacing = disp->font_spacing;
    rgb_t bg = zone->bg_color;
    uint16_t row = start_row;
    size_t len = strlen(text);
    for (size_t i = 0; i < len; i++) {
        char ch = text[i];
        rgb_t color = get_char_color(disp, i, zone, default_color);
        uint16_t *char_data = get_char_data(disp, ch);
        char_animation_t *anim = (i < MAX_CHARS_PER_ZONE) ? &zone->animations[i] : NULL;
        if (anim && anim->active) {
            char_animation_update(anim);
            float progress = anim->progress;
            if (progress >= 1.0f) {
                render_char_fast(disp, row, char_data, char_w, char_h, color, 255);
                anim->active = false;
            } else {
                for (uint8_t bit = 0; bit < char_h; bit++) {
                    uint16_t pr = row + bit;
                    if (pr < disp->fb_rows) {
                        for (uint8_t col = 0; col < char_w; col++)
                            fb_pixel_rgb(disp, pr, col, bg.r, bg.g, bg.b);
                    }
                }
                uint16_t *old_data = get_char_data(disp, anim->old_char);
                uint16_t *new_data = get_char_data(disp, anim->new_char);
                render_char_partial(disp, row, old_data, new_data, char_w, char_h, color, 255,
                                    anim->anim_type, progress);
            }
        } else {
            for (uint8_t bit = 0; bit < char_h; bit++) {
                uint16_t pr = row + bit;
                if (pr < disp->fb_rows) {
                    for (uint8_t col = 0; col < char_w; col++)
                        fb_pixel_rgb(disp, pr, col, bg.r, bg.g, bg.b);
                }
            }
            render_char_fast(disp, row, char_data, char_w, char_h, color, 255);
        }
        row += char_h + spacing;
        if (row >= disp->fb_rows) break;
    }
}

// ========== Blink management ==========
static void zone_update_blink(zone_config_t *zone) {
    if (!zone->blink_enabled) return;
    uint32_t now = mp_hal_ticks_ms();
    if (now - zone->blink_last_toggle >= zone->blink_period) {
        zone->blink_on = !zone->blink_on;
        zone->blink_last_toggle = now;
    }
}
static void apply_blink_mask(led_display_obj_t *disp, zone_config_t *zone, const char *text, int16_t start_row) {
    if (!zone->blink_enabled) return;
    zone_update_blink(zone);
    if (zone->blink_on) return;
    uint8_t char_h = disp->font_height, char_w = disp->font_width, spacing = disp->font_spacing;
    rgb_t bg = zone->bg_color;
    size_t len = strlen(text);
    if (zone->blink_zone) {
        for (uint16_t r = 0; r < zone->height; r++)
            for (uint8_t c = 0; c < disp->fb_cols; c++)
                fb_pixel_rgb(disp, r, c, bg.r, bg.g, bg.b);
        return;
    }
    for (size_t i = 0; i < len && i < MAX_CHARS_PER_ZONE; i++) {
        bool match = false;
        for (uint8_t p = 0; p < zone->blink_pos_count; p++)
            if (zone->blink_positions[p] == i) { match = true; break; }
        if (!match) {
            for (uint8_t pc = 0; pc < zone->blink_char_count; pc++)
                if (zone->blink_chars[pc] == text[i]) { match = true; break; }
        }
        if (match) {
            uint16_t top = start_row + i * (char_h + spacing);
            for (uint8_t bit = 0; bit < char_h; bit++) {
                uint16_t pr = top + bit;
                if (pr < disp->fb_rows) {
                    for (uint8_t col = 0; col < char_w; col++)
                        fb_pixel_rgb(disp, pr, col, bg.r, bg.g, bg.b);
                }
            }
        }
    }
}

// ========== Change text and create animations ==========
static void change_text_animated(led_display_obj_t *disp, const char *new_text, const char *old_text, zone_config_t *zone) {
    (void)disp;
    if (zone->anim_type == AN_N) return;
    size_t new_len = strlen(new_text), old_len = strlen(old_text);
    size_t max_len = (new_len > old_len) ? new_len : old_len;
    for (size_t i = 0; i < max_len && i < MAX_CHARS_PER_ZONE; i++) {
        char old_char = (i < old_len) ? old_text[i] : ' ';
        char new_char = (i < new_len) ? new_text[i] : ' ';
        if (old_char != new_char) {
            if (!zone->animations[i].active || zone->animations[i].new_char != new_char) {
                uint32_t delay = i * zone->anim_stagger;
                char_animation_start(&zone->animations[i], old_char, new_char, zone->anim_type, zone->anim_duration, delay);
            }
        }
    }
}

// ========== Compose zone text ==========
static void compose_zone_text(led_display_obj_t *self, zone_config_t *zone, mp_obj_t controller, char *out_text, size_t out_len) {
    if (zone->zone_type == 1) {
        mp_obj_t hour = mp_load_attr(controller, MP_QSTR_hour);
        mp_obj_t minute = mp_load_attr(controller, MP_QSTR_minutes);
        mp_obj_t second = mp_load_attr(controller, MP_QSTR_seconds);
        int h = mp_obj_get_int(hour), m = mp_obj_get_int(minute), s = mp_obj_get_int(second);
        uint16_t max_chars = zone->height / (self->font_height + self->font_spacing);
        if (max_chars >= 8) snprintf(out_text, out_len, "%02d:%02d:%02d", h, m, s);
        else snprintf(out_text, out_len, "%02d:%02d", h, m);
    } else if (zone->presentation == PRES_SQ && zone->msg_count > 0) {
        if (!zone->scroll_enabled) {
            uint32_t now = mp_hal_ticks_ms();
            if (now - zone->seq_last_change >= zone->seq_duration) {
                zone->seq_index = (zone->seq_index + 1) % zone->msg_count;
                zone->seq_last_change = now;
            }
        }
        strncpy(out_text, zone->messages[zone->seq_index], out_len-1);
    } else if (zone->msg_count > 0) {
        strncpy(out_text, zone->messages[0], out_len-1);
    } else {
        out_text[0] = '\0';
    }
    out_text[out_len-1] = '\0';
}

// ========== Copy framebuffer to physical LED buffer ==========
static void copy_to_led_buffer(led_display_obj_t *disp, bool do_clear) {
    if (!disp->led_buffer_valid) {
        if (!mp_get_buffer(disp->led_device, &disp->led_buffer, MP_BUFFER_READ | MP_BUFFER_WRITE)) return;
        disp->led_buffer_valid = true;
    }
    uint8_t *led_buf = (uint8_t*)disp->led_buffer.buf;
    if (do_clear) {
        memset(led_buf, 0, disp->led_buffer.len);
    }
    uint16_t phys_rows = disp->rows;
    uint8_t cols = disp->cols;
    uint16_t row_offset = disp->current_zone_row_offset;
    for (uint16_t r = 0; r < disp->fb_rows; r++) {
        uint16_t target_row = r + row_offset;
        if (target_row >= phys_rows) continue;
        uint16_t reversed_row = phys_rows - 1 - target_row;
        for (uint8_t c = 0; c < cols; c++) {
            uint32_t fb_idx = (r * disp->fb_cols + c) * 3;
            uint8_t red = disp->framebuffer[fb_idx];
            uint8_t green = disp->framebuffer[fb_idx+1];
            uint8_t blue = disp->framebuffer[fb_idx+2];
            if (!disp->write_black_pixels && red == 0 && green == 0 && blue == 0) continue;
            uint32_t led_off;
            if (disp->render_direction == 1) {
                led_off = (target_row * cols + c) * 3;
            } else {
                uint16_t col_start = c * phys_rows;
                uint16_t led_idx = (c % 2 == 0) ? col_start + reversed_row : col_start + target_row;
                led_off = led_idx * 3;
            }
            if (disp->copy_alpha < 255) {
                uint16_t a = disp->copy_alpha, ia = 255 - a;
                led_buf[led_off]   = (uint8_t)((red   * a + led_buf[led_off]   * ia) >> 8);
                led_buf[led_off+1] = (uint8_t)((green * a + led_buf[led_off+1] * ia) >> 8);
                led_buf[led_off+2] = (uint8_t)((blue  * a + led_buf[led_off+2] * ia) >> 8);
            } else {
                led_buf[led_off]   = red;
                led_buf[led_off+1] = green;
                led_buf[led_off+2] = blue;
            }
        }
    }
}

// ========== Color Manager helpers ==========
static void set_char_color(led_display_obj_t *self, uint16_t pos, uint8_t r, uint8_t g, uint8_t b) {
    if (pos >= self->char_colors_len) {
        uint16_t new_len = pos + 1;
        rgb_t *new_arr = m_new(rgb_t, new_len);
        if (self->char_colors) {
            memcpy(new_arr, self->char_colors, self->char_colors_len * sizeof(rgb_t));
            m_del(rgb_t, self->char_colors, self->char_colors_len);
        }
        memset(new_arr + self->char_colors_len, 0, (new_len - self->char_colors_len) * sizeof(rgb_t));
        self->char_colors = new_arr;
        self->char_colors_len = new_len;
    }
    self->char_colors[pos] = (rgb_t){r, g, b};
}
static void set_section_color(led_display_obj_t *self, uint16_t start, uint16_t end, uint8_t r, uint8_t g, uint8_t b) {
    color_section_t *sec = m_new(color_section_t, 1);
    sec->start = start;
    sec->end = end;
    sec->color = (rgb_t){r, g, b};
    sec->next = self->color_sections;
    self->color_sections = sec;
}
static void clear_all_colors(led_display_obj_t *self) {
    if (self->char_colors) m_del(rgb_t, self->char_colors, self->char_colors_len);
    self->char_colors = NULL;
    self->char_colors_len = 0;
    color_section_t *s = self->color_sections;
    while (s) {
        color_section_t *next = s->next;
        m_del(color_section_t, s, 1);
        s = next;
    }
    self->color_sections = NULL;
}

// ========== MicroPython object methods ==========
static mp_obj_t led_display_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)type;
    mp_arg_check_num(n_args, n_kw, 1, 5, true);
    mp_obj_t led_device = args[0];
    mp_obj_t rows_obj = mp_const_none;
    mp_obj_t cols_obj = mp_const_none;
    mp_obj_t render_direction_obj = mp_const_none;
    mp_obj_t spacing_obj = mp_obj_new_int(2);

    if (n_args > 1) rows_obj = args[1];
    if (n_args > 2) cols_obj = args[2];
    if (n_args > 3) render_direction_obj = args[3];
    if (n_args > 4) spacing_obj = args[4];

    for (size_t i = 0; i < n_kw; i++) {
        mp_obj_t key = args[n_args + 2 * i];
        mp_obj_t value = args[n_args + 2 * i + 1];
        if (key == MP_OBJ_NEW_QSTR(MP_QSTR_rows)) {
            rows_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_cols)) {
            cols_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_render_direction)) {
            render_direction_obj = value;
        } else if (key == MP_OBJ_NEW_QSTR(MP_QSTR_spacing)) {
            spacing_obj = value;
        }
    }

    mp_int_t cols = (cols_obj == mp_const_none) ? 3 : mp_obj_get_int(cols_obj);
    if (cols < 1) cols = 1;

    mp_int_t rows;
    if (rows_obj != mp_const_none) {
        rows = mp_obj_get_int(rows_obj);
    } else {
        mp_obj_t aled = mp_load_attr(led_device, MP_QSTR_aled_object);
        rows = mp_obj_get_int(mp_load_attr(aled, MP_QSTR_n)) / cols;
    }
    if (rows < 1) rows = 1;

    mp_int_t spacing = mp_obj_get_int(spacing_obj);
    if (spacing < 0) spacing = 0;

    led_display_obj_t *self = m_new_obj(led_display_obj_t);
    self->base.type = &led_display_type;
    mp_buffer_info_t led_buffer;
    if (mp_get_buffer(led_device, &led_buffer, MP_BUFFER_READ | MP_BUFFER_WRITE)) {
        self->led_device = led_device;
        self->led_buffer = led_buffer;
        self->led_buffer_valid = true;
    } else {
        self->led_device = mp_load_attr(led_device, MP_QSTR_led_buffer);
        self->led_buffer_valid = false;
    }
    self->rows = rows;
    self->cols = cols;
    self->render_direction = (render_direction_obj != mp_const_none) ? (uint8_t)mp_obj_get_int(render_direction_obj) : 0;
    self->fb_rows = rows;
    self->fb_cols = cols;
    self->framebuffer = m_new(uint8_t, rows * cols * 3);
    memset(self->framebuffer, 0, rows * cols * 3);
    self->brightness = 255;
    self->font_width = FONT_WIDTH;
    self->font_height = FONT_HEIGHT;
    self->font_spacing = spacing;
    
    // Copy built-in 8-bit font to 16-bit buffer
    size_t builtin_data_len = sizeof(font_4x7_data);
    size_t builtin_num_chars = builtin_data_len / FONT_WIDTH;
    uint16_t *builtin_16 = m_new(uint16_t, builtin_data_len);
    for (size_t i = 0; i < builtin_data_len; i++) builtin_16[i] = font_4x7_data[i];
    
    self->font_data = builtin_16;
    self->font_count = 1;
    self->current_font_idx = 0;
    strcpy(self->fonts[0].name, "4x7");
    self->fonts[0].width = FONT_WIDTH;
    self->fonts[0].height = FONT_HEIGHT;
    self->fonts[0].spacing = spacing;
    self->fonts[0].start_char = 32;
    self->fonts[0].end_char = 32 + builtin_num_chars - 1;
    self->fonts[0].data = builtin_16;
    self->char_colors = NULL;
    self->char_colors_len = 0;
    self->color_sections = NULL;
    self->zone_count = 0;
    self->global_anim_type = AN_F;
    self->global_anim_duration = 300;
    self->global_anim_stagger = 20;
    self->write_black_pixels = true;
    self->clear_led_before_copy = false;
    self->copy_alpha = 255;
    self->last_update = 0;
    self->update_interval = 0;
    self->current_zone_row_offset = 0;
    return MP_OBJ_FROM_PTR(self);
}
// ----- Basic API -----
static mp_obj_t led_display_set_brightness(mp_obj_t self_in, mp_obj_t value) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int v = mp_obj_get_int(value);
    if (v < 0) {
        v = 0;
    }
    if (v > 255) {
        v = 255;
    }
    self->brightness = v;
    return mp_const_none;
}
static mp_obj_t led_display_set_brightness_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t value = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_value, mp_const_none);
    if (value == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("value required"));
    }
    return led_display_set_brightness(args[0], value);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_brightness_obj, 1, led_display_set_brightness_kw);

static mp_obj_t led_display_set_zones(mp_obj_t self_in, mp_obj_t zones_list) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!mp_obj_is_type(zones_list, &mp_type_list)) mp_raise_TypeError(MP_ERROR_TEXT("zones must be a list"));
    size_t len = mp_obj_get_int(mp_obj_len(zones_list));
    self->zone_count = 0;
    for (size_t i = 0; i < len && self->zone_count < MAX_ZONES; i++) {
        mp_obj_t z = mp_obj_subscr(zones_list, MP_OBJ_NEW_SMALL_INT(i), MP_OBJ_SENTINEL);
        const char *name; mp_int_t start, end;
        if (mp_obj_is_type(z, &mp_type_tuple) && mp_obj_get_int(mp_obj_len(z)) >= 3) {
            name = mp_obj_str_get_str(mp_obj_subscr(z, MP_OBJ_NEW_SMALL_INT(0), MP_OBJ_SENTINEL));
            start = mp_obj_get_int(mp_obj_subscr(z, MP_OBJ_NEW_SMALL_INT(1), MP_OBJ_SENTINEL));
            end = mp_obj_get_int(mp_obj_subscr(z, MP_OBJ_NEW_SMALL_INT(2), MP_OBJ_SENTINEL));
        } else if (mp_obj_is_type(z, &mp_type_dict)) {
            name = mp_obj_str_get_str(mp_obj_dict_get(z, MP_OBJ_NEW_QSTR(MP_QSTR_name)));
            start = mp_obj_get_int(mp_obj_dict_get(z, MP_OBJ_NEW_QSTR(MP_QSTR_start_row)));
            end = mp_obj_get_int(mp_obj_dict_get(z, MP_OBJ_NEW_QSTR(MP_QSTR_end_row)));
        } else continue;
        zone_config_t *zone = &self->zones[self->zone_count++];
        memset(zone, 0, sizeof(zone_config_t));
        strncpy(zone->name, name, 15);
        zone->start_row = start;
        zone->end_row = end;
        zone->height = end - start + 1;
        zone->brightness = 255;
        zone->fg_color = (rgb_t){255,255,255};
        zone->bg_color = (rgb_t){0,0,0};
        zone->presentation = PRES_ST;
        zone->anim_type = self->global_anim_type;
        zone->anim_duration = self->global_anim_duration;
        zone->anim_stagger = self->global_anim_stagger;
        zone->scroll_enabled = false;
        uint16_t max_chars = zone->height / (self->font_height + self->font_spacing);
        if (max_chars < 1) max_chars = 1;
        scroll_manager_init(&zone->scroll_mgr, max_chars, self->font_height, self->font_spacing);
        zone->seq_last_change = mp_hal_ticks_ms();
        zone->seq_duration = 3000;
        zone->blink_period = 500;
        zone->blink_last_toggle = mp_hal_ticks_ms();
        zone->blink_on = true;
        zone->zone_type = (strncmp(name, "time", 16) == 0) ? 1 : 0;
        zone->text_align = 0;
        zone->zone_enabled = true;
        zone->zone_alpha = 255;
        memset(zone->animations, 0, sizeof(zone->animations));
    }
    uint16_t max_h = 0;
    for (uint8_t i = 0; i < self->zone_count; i++) if (self->zones[i].height > max_h) max_h = self->zones[i].height;
    if (max_h > 0 && max_h != self->fb_rows) {
        m_del(uint8_t, self->framebuffer, self->fb_rows * self->fb_cols * 3);
        self->fb_rows = max_h;
        self->framebuffer = m_new(uint8_t, self->fb_rows * self->fb_cols * 3);
        memset(self->framebuffer, 0, self->fb_rows * self->fb_cols * 3);
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zones_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t zones = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_defs, mp_const_none);
    if (zones == mp_const_none) {
        zones = led_display_kw_get(kw_args, MP_QSTR_zones, mp_const_none);
    }
    if (zones == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_defs required"));
    }
    return led_display_set_zones(args[0], zones);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zones_obj, 1, led_display_set_zones_kw);

static mp_obj_t led_display_update(mp_obj_t self_in, mp_obj_t controller) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint32_t now = mp_hal_ticks_ms();
    if (now - self->last_update < self->update_interval) return mp_const_none;
    self->last_update = now;
    uint8_t saved_brightness = self->brightness;
    bool did_clear = false;
    for (uint8_t zi = 0; zi < self->zone_count; zi++) {
        zone_config_t *zone = &self->zones[zi];
        if (!zone->zone_enabled) continue;
        char text[MAX_MSG_LEN];
        compose_zone_text(self, zone, controller, text, MAX_MSG_LEN);
        self->fb_rows = zone->height;
        self->current_zone_row_offset = zone->start_row;
        self->brightness = ((uint16_t)zone->brightness * saved_brightness) / 255;
        fb_clear(self);
        for (uint16_t r = 0; r < zone->height; r++)
            for (uint8_t c = 0; c < self->fb_cols; c++)
                fb_pixel_rgb(self, r, c, zone->bg_color.r, zone->bg_color.g, zone->bg_color.b);
        uint8_t char_h = self->font_height, spacing = self->font_spacing;
        uint16_t max_chars = zone->height / (char_h + spacing);
        if (zone->scroll_enabled && strlen(text) > max_chars) {
            if (strcmp(text, zone->scroll_source_last) != 0) {
                if (strlen(text) == strlen(zone->scroll_source_last)) {
                    scroll_manager_update_text_in_place(&zone->scroll_mgr, text);
                } else {
                    scroll_manager_set_text(&zone->scroll_mgr, text);
                }
                strncpy(zone->scroll_source_last, text, MAX_MSG_LEN);
            }
            char scrolled[MAX_MSG_LEN];
            scroll_manager_update(&zone->scroll_mgr, scrolled);
            int16_t offset = -scroll_manager_get_pixel_offset(&zone->scroll_mgr);
            render_text_vertical(self, scrolled, offset, zone->fg_color, zone, 0, zone->height);
            apply_blink_mask(self, zone, scrolled, offset);
            if (zone->presentation == PRES_SQ && zone->msg_count > 0 && scroll_manager_is_ready(&zone->scroll_mgr)) {
                zone->seq_index = (zone->seq_index + 1) % zone->msg_count;
                zone->seq_last_change = now;
                zone->scroll_source_last[0] = '\0';
            }
        } else {
            int16_t align_offset = 0;
            if (zone->text_align != 0) {
                uint16_t text_height = (uint16_t)strlen(text) * (char_h + spacing);
                if (text_height < zone->height) {
                    if (zone->text_align == 1) align_offset = (zone->height - text_height) / 2;
                    else if (zone->text_align == 2) align_offset = zone->height - text_height;
                }
            }
            change_text_animated(self, text, zone->last_text, zone);
            render_text_vertical_animated(self, text, align_offset, zone->fg_color, zone);
            apply_blink_mask(self, zone, text, align_offset);
            strncpy(zone->last_text, text, MAX_MSG_LEN);
            if (zone->presentation == PRES_SQ && zone->msg_count > 0 && (!zone->scroll_enabled || strlen(text) <= max_chars) && (now - zone->seq_last_change >= zone->seq_duration)) {
                zone->seq_index = (zone->seq_index + 1) % zone->msg_count;
                zone->seq_last_change = now;
            }
        }
        uint8_t saved_alpha = self->copy_alpha;
        self->copy_alpha = zone->zone_alpha;
        copy_to_led_buffer(self, !did_clear && self->clear_led_before_copy);
        self->copy_alpha = saved_alpha;
        did_clear = true;
    }
    self->brightness = saved_brightness;
    return mp_const_none;
}
static mp_obj_t led_display_update_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t controller = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_controller, mp_const_none);
    if (controller == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("controller required"));
    }
    return led_display_update(args[0], controller);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_update_obj, 1, led_display_update_kw);

// ----- Additional API methods (zone config, blink, colors, scroll, font, resize) -----
static mp_obj_t led_display_set_zone_brightness(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t value) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    int v = mp_obj_get_int(value);
    if (v < 0) {
        v = 0;
    }
    if (v > 255) {
        v = 255;
    }
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) { self->zones[i].brightness = v; break; }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_brightness_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 3, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t value = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_value, mp_const_none);
    if (zone_name == mp_const_none || value == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name and value required"));
    }
    return led_display_set_zone_brightness(args[0], zone_name, value);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_brightness_obj, 1, led_display_set_zone_brightness_kw);

static mp_obj_t led_display_set_zone_colors(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t fg, mp_obj_t bg) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            if (fg != mp_const_none) {
                mp_obj_t *items; size_t len;
                mp_obj_get_array(fg, &len, &items);
                if (len >= 3) {
                    self->zones[i].fg_color.r = mp_obj_get_int(items[0]);
                    self->zones[i].fg_color.g = mp_obj_get_int(items[1]);
                    self->zones[i].fg_color.b = mp_obj_get_int(items[2]);
                }
            }
            if (bg != mp_const_none) {
                mp_obj_t *items; size_t len;
                mp_obj_get_array(bg, &len, &items);
                if (len >= 3) {
                    self->zones[i].bg_color.r = mp_obj_get_int(items[0]);
                    self->zones[i].bg_color.g = mp_obj_get_int(items[1]);
                    self->zones[i].bg_color.b = mp_obj_get_int(items[2]);
                }
            }
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_colors_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 4, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t fg = n_args > 2 ? args[2] : mp_const_none;
    mp_obj_t bg = n_args > 3 ? args[3] : mp_const_none;
    fg = led_display_kw_get(kw_args, MP_QSTR_fg, fg);
    bg = led_display_kw_get(kw_args, MP_QSTR_bg, bg);
    if (zone_name == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name required"));
    }
    return led_display_set_zone_colors(args[0], zone_name, fg, bg);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_colors_obj, 1, led_display_set_zone_colors_kw);

static mp_obj_t led_display_set_zone_messages(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t messages) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    if (!mp_obj_is_type(messages, &mp_type_list)) mp_raise_TypeError(MP_ERROR_TEXT("messages must be a list"));
    size_t len = mp_obj_get_int(mp_obj_len(messages));
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            z->msg_count = 0;
            for (size_t j = 0; j < len && j < MAX_MSG_PER_ZONE; j++) {
                mp_obj_t msg = mp_obj_subscr(messages, MP_OBJ_NEW_SMALL_INT(j), MP_OBJ_SENTINEL);
                const char *str = mp_obj_str_get_str(msg);
                strncpy(z->messages[z->msg_count], str, MAX_MSG_LEN-1);
                z->messages[z->msg_count][MAX_MSG_LEN-1] = '\0';
                z->msg_count++;
            }
            if (z->msg_count > 1 && z->presentation == PRES_ST) z->presentation = PRES_SQ;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_messages_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 3, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t messages = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_messages, mp_const_none);
    if (zone_name == mp_const_none || messages == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name and messages required"));
    }
    return led_display_set_zone_messages(args[0], zone_name, messages);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_messages_obj, 1, led_display_set_zone_messages_kw);

static mp_obj_t led_display_set_zone_scroll(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t enabled, mp_obj_t mode, mp_obj_t delay_ms, mp_obj_t pixel_step, mp_obj_t end_hold_ms, mp_obj_t start_hold_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    bool en = mp_obj_is_true(enabled);
    const char *mode_str = mp_obj_str_get_str(mode);
    uint8_t smode = (strcmp(mode_str, "p") == 0 || strcmp(mode_str, "pixel") == 0) ? SCROLL_P :
                    (strcmp(mode_str, "l") == 0 || strcmp(mode_str, "loop") == 0) ? SCROLL_LOOP : SCROLL_C;
    uint32_t delay = mp_obj_get_int(delay_ms);
    uint8_t step = mp_obj_get_int(pixel_step);
    if (step < 1) step = 1;
    uint32_t end_hold = mp_obj_get_int(end_hold_ms);
    uint32_t start_hold = mp_obj_get_int(start_hold_ms);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            z->scroll_enabled = en;
            z->scroll_mgr.scroll_mode = smode;
            z->scroll_mgr.scroll_delay_ms = delay;
            z->scroll_mgr.pixel_step = step;
            z->scroll_mgr.end_hold_ms = end_hold;
            z->scroll_mgr.start_hold_ms = start_hold;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_scroll_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 8, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t enabled = n_args > 2 ? args[2] : mp_const_true;
    mp_obj_t mode = n_args > 3 ? args[3] : MP_OBJ_NEW_QSTR(MP_QSTR_pixel);
    mp_obj_t delay_ms = n_args > 4 ? args[4] : mp_obj_new_int(200);
    mp_obj_t pixel_step = n_args > 5 ? args[5] : mp_obj_new_int(1);
    mp_obj_t end_hold_ms = n_args > 6 ? args[6] : mp_obj_new_int(800);
    mp_obj_t start_hold_ms = n_args > 7 ? args[7] : mp_obj_new_int(500);
    enabled = led_display_kw_get(kw_args, MP_QSTR_enabled, enabled);
    mode = led_display_kw_get(kw_args, MP_QSTR_mode, mode);
    delay_ms = led_display_kw_get(kw_args, MP_QSTR_delay_ms, delay_ms);
    pixel_step = led_display_kw_get(kw_args, MP_QSTR_pixel_step, pixel_step);
    end_hold_ms = led_display_kw_get(kw_args, MP_QSTR_end_hold_ms, end_hold_ms);
    start_hold_ms = led_display_kw_get(kw_args, MP_QSTR_start_hold_ms, start_hold_ms);
    if (zone_name == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name required"));
    }
    return led_display_set_zone_scroll(args[0], zone_name, enabled, mode, delay_ms, pixel_step, end_hold_ms, start_hold_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_scroll_obj, 1, led_display_set_zone_scroll_kw);

static mp_obj_t led_display_set_zone_presentation(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t presentation,
                                                  mp_obj_t animation, mp_obj_t duration_ms, mp_obj_t stagger_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    const char *pres_str = mp_obj_str_get_str(presentation);
    const char *anim_str = mp_obj_str_get_str(animation);
    uint8_t pres = (strcmp(pres_str, "sq") == 0 || strcmp(pres_str, "sequential") == 0) ? PRES_SQ : PRES_ST;
    uint8_t anim = AN_N;
    if (strcmp(anim_str, "f") == 0 || strcmp(anim_str, "fade") == 0) anim = AN_F;
    else if (strcmp(anim_str, "ft") == 0 || strcmp(anim_str, "fade_through") == 0) anim = AN_FT;
    else if (strcmp(anim_str, "su") == 0 || strcmp(anim_str, "slide_up") == 0) anim = AN_SU;
    else if (strcmp(anim_str, "sd") == 0 || strcmp(anim_str, "slide_down") == 0) anim = AN_SD;
    else if (strcmp(anim_str, "sl") == 0 || strcmp(anim_str, "slide_left") == 0) anim = AN_SL;
    else if (strcmp(anim_str, "sr") == 0 || strcmp(anim_str, "slide_right") == 0) anim = AN_SR;
    uint32_t dur = mp_obj_get_int(duration_ms);
    uint16_t stag = mp_obj_get_int(stagger_ms);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            self->zones[i].presentation = pres;
            self->zones[i].anim_type = anim;
            self->zones[i].anim_duration = dur;
            self->zones[i].anim_stagger = stag;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_presentation_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 6, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t presentation = n_args > 2 ? args[2] : MP_OBJ_NEW_QSTR(MP_QSTR_static);
    mp_obj_t animation = n_args > 3 ? args[3] : MP_OBJ_NEW_QSTR(MP_QSTR_fade);
    mp_obj_t duration_ms = n_args > 4 ? args[4] : mp_obj_new_int(300);
    mp_obj_t stagger_ms = n_args > 5 ? args[5] : mp_obj_new_int(20);
    presentation = led_display_kw_get(kw_args, MP_QSTR_presentation, presentation);
    animation = led_display_kw_get(kw_args, MP_QSTR_animation, animation);
    duration_ms = led_display_kw_get(kw_args, MP_QSTR_duration_ms, duration_ms);
    stagger_ms = led_display_kw_get(kw_args, MP_QSTR_stagger_ms, stagger_ms);
    if (zone_name == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name required"));
    }
    return led_display_set_zone_presentation(args[0], zone_name, presentation, animation, duration_ms, stagger_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_presentation_obj, 1, led_display_set_zone_presentation_kw);

// Blink API
static mp_obj_t led_display_enable_colon_blink(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t period_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    uint32_t period = mp_obj_get_int(period_ms);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            z->blink_enabled = true;
            z->blink_zone = false;
            z->blink_char_count = 1;
            z->blink_chars[0] = ':';
            z->blink_pos_count = 0;
            z->blink_period = period;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_enable_colon_blink_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 3, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : MP_OBJ_NEW_QSTR(MP_QSTR_time);
    zone_name = led_display_kw_get(kw_args, MP_QSTR_zone_name, zone_name);
    mp_obj_t period_ms = n_args > 2 ? args[2] : mp_obj_new_int(500);
    period_ms = led_display_kw_get(kw_args, MP_QSTR_period_ms, period_ms);
    return led_display_enable_colon_blink(args[0], zone_name, period_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_enable_colon_blink_obj, 1, led_display_enable_colon_blink_kw);
static mp_obj_t led_display_disable_colon_blink(mp_obj_t self_in, mp_obj_t zone_name) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) self->zones[i].blink_enabled = false;
    return mp_const_none;
}
static mp_obj_t led_display_disable_colon_blink_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : MP_OBJ_NEW_QSTR(MP_QSTR_time);
    zone_name = led_display_kw_get(kw_args, MP_QSTR_zone_name, zone_name);
    return led_display_disable_colon_blink(args[0], zone_name);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_disable_colon_blink_obj, 1, led_display_disable_colon_blink_kw);

static mp_obj_t led_display_set_zone_blink_chars(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t chars, mp_obj_t period_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    const char *chars_str = mp_obj_str_get_str(chars);
    uint32_t period = mp_obj_get_int(period_ms);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            z->blink_enabled = true;
            z->blink_zone = false;
            z->blink_char_count = strlen(chars_str);
            if (z->blink_char_count > MAX_BLINK_CHARS) z->blink_char_count = MAX_BLINK_CHARS;
            memcpy(z->blink_chars, chars_str, z->blink_char_count);
            z->blink_pos_count = 0;
            z->blink_period = period;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_blink_chars_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 4, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t chars = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_chars, mp_const_none);
    mp_obj_t period_ms = n_args > 3 ? args[3] : mp_obj_new_int(500);
    period_ms = led_display_kw_get(kw_args, MP_QSTR_period_ms, period_ms);
    if (zone_name == mp_const_none || chars == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name and chars required"));
    }
    return led_display_set_zone_blink_chars(args[0], zone_name, chars, period_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_blink_chars_obj, 1, led_display_set_zone_blink_chars_kw);
static mp_obj_t led_display_set_zone_blink_positions(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t positions, mp_obj_t period_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    uint32_t period = mp_obj_get_int(period_ms);
    if (!mp_obj_is_type(positions, &mp_type_list)) mp_raise_TypeError(MP_ERROR_TEXT("positions must be a list"));
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            z->blink_enabled = true;
            z->blink_zone = false;
            z->blink_pos_count = 0;
            size_t len = mp_obj_get_int(mp_obj_len(positions));
            for (size_t j = 0; j < len && j < MAX_BLINK_POS; j++) {
                mp_obj_t pos = mp_obj_subscr(positions, MP_OBJ_NEW_SMALL_INT(j), MP_OBJ_SENTINEL);
                z->blink_positions[z->blink_pos_count++] = mp_obj_get_int(pos);
            }
            z->blink_char_count = 0;
            z->blink_period = period;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_blink_positions_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 4, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t positions = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_positions, mp_const_none);
    mp_obj_t period_ms = n_args > 3 ? args[3] : mp_obj_new_int(500);
    period_ms = led_display_kw_get(kw_args, MP_QSTR_period_ms, period_ms);
    if (zone_name == mp_const_none || positions == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name and positions required"));
    }
    return led_display_set_zone_blink_positions(args[0], zone_name, positions, period_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_blink_positions_obj, 1, led_display_set_zone_blink_positions_kw);
static mp_obj_t led_display_set_zone_blink(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t enabled, mp_obj_t period_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    bool en = mp_obj_is_true(enabled);
    uint32_t period = mp_obj_get_int(period_ms);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            self->zones[i].blink_enabled = en;
            self->zones[i].blink_zone = en;
            self->zones[i].blink_period = period;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_blink_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 4, true);
    mp_obj_t zone_name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone_name, mp_const_none);
    mp_obj_t enabled = n_args > 2 ? args[2] : mp_const_true;
    mp_obj_t period_ms = n_args > 3 ? args[3] : mp_obj_new_int(500);
    enabled = led_display_kw_get(kw_args, MP_QSTR_enabled, enabled);
    period_ms = led_display_kw_get(kw_args, MP_QSTR_period_ms, period_ms);
    if (zone_name == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone_name required"));
    }
    return led_display_set_zone_blink(args[0], zone_name, enabled, period_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_blink_obj, 1, led_display_set_zone_blink_kw);

// Configure helpers
static mp_obj_t led_display_configure_time_zone(mp_obj_t self_in, mp_obj_t start_row, mp_obj_t end_row, mp_obj_t brightness, mp_obj_t with_seconds, mp_obj_t colon_blink_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int start = mp_obj_get_int(start_row);
    int end = (end_row == mp_const_none) ? -1 : mp_obj_get_int(end_row);
    int bright = mp_obj_get_int(brightness);
    bool with_sec = mp_obj_is_true(with_seconds);
    int blink_ms = (colon_blink_ms == mp_const_none) ? 0 : mp_obj_get_int(colon_blink_ms);
    if (end < 0) {
        int rows_needed = (with_sec ? 8 : 5) * (self->font_height + self->font_spacing);
        end = start + rows_needed - 1;
        if (end >= (int)self->rows) end = self->rows - 1;
    }
    mp_obj_t zones_list = mp_obj_new_list(0, NULL);
    mp_obj_t zone_tuple = mp_obj_new_tuple(3, (mp_obj_t[]){
        MP_OBJ_NEW_QSTR(MP_QSTR_time),
        mp_obj_new_int(start),
        mp_obj_new_int(end)
    });
    mp_obj_list_append(zones_list, zone_tuple);
    led_display_set_zones(self_in, zones_list);
    led_display_set_zone_brightness(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_time), mp_obj_new_int(bright));
    if (blink_ms > 0) led_display_enable_colon_blink(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_time), mp_obj_new_int(blink_ms));
    return mp_const_none;
}
static mp_obj_t led_display_configure_time_zone_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 6, true);
    mp_obj_t start_row = n_args > 1 ? args[1] : mp_obj_new_int(0);
    mp_obj_t end_row = n_args > 2 ? args[2] : mp_const_none;
    mp_obj_t brightness = n_args > 3 ? args[3] : mp_obj_new_int(63);
    mp_obj_t with_seconds = n_args > 4 ? args[4] : mp_const_true;
    mp_obj_t colon_blink_ms = n_args > 5 ? args[5] : mp_const_none;
    start_row = led_display_kw_get(kw_args, MP_QSTR_start_row, start_row);
    end_row = led_display_kw_get(kw_args, MP_QSTR_end_row, end_row);
    brightness = led_display_kw_get(kw_args, MP_QSTR_brightness, brightness);
    with_seconds = led_display_kw_get(kw_args, MP_QSTR_with_seconds, with_seconds);
    colon_blink_ms = led_display_kw_get(kw_args, MP_QSTR_colon_blink_ms, colon_blink_ms);
    return led_display_configure_time_zone(args[0], start_row, end_row, brightness, with_seconds, colon_blink_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_configure_time_zone_obj, 1, led_display_configure_time_zone_kw);

static mp_obj_t led_display_configure_custom_scroll_zone(mp_obj_t self_in, mp_obj_t start_row, mp_obj_t end_row, mp_obj_t brightness, mp_obj_t text, mp_obj_t delay_ms, mp_obj_t pixel_step, mp_obj_t end_hold_ms) {
    int start = mp_obj_get_int(start_row);
    int end = mp_obj_get_int(end_row);
    int bright = mp_obj_get_int(brightness);
    const char *txt = mp_obj_str_get_str(text);
    int delay = mp_obj_get_int(delay_ms);
    int step = mp_obj_get_int(pixel_step);
    int end_hold = mp_obj_get_int(end_hold_ms);
    mp_obj_t zones_list = mp_obj_new_list(0, NULL);
    mp_obj_t zone_tuple = mp_obj_new_tuple(3, (mp_obj_t[]){
        MP_OBJ_NEW_QSTR(MP_QSTR_custom),
        mp_obj_new_int(start),
        mp_obj_new_int(end)
    });
    mp_obj_list_append(zones_list, zone_tuple);
    led_display_set_zones(self_in, zones_list);
    led_display_set_zone_brightness(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_custom), mp_obj_new_int(bright));
    mp_obj_t msg_list = mp_obj_new_list(1, (mp_obj_t[]){ mp_obj_new_str(txt, strlen(txt)) });
    led_display_set_zone_messages(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_custom), msg_list);
    led_display_set_zone_scroll(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_custom), mp_const_true,
        MP_OBJ_NEW_QSTR(MP_QSTR_pixel), mp_obj_new_int(delay), mp_obj_new_int(step),
        mp_obj_new_int(end_hold), mp_obj_new_int(500));
    return mp_const_none;
}
static mp_obj_t led_display_configure_custom_scroll_zone_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 8, true);
    mp_obj_t start_row = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_start_row, mp_const_none);
    mp_obj_t end_row = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_end_row, mp_const_none);
    mp_obj_t brightness = n_args > 3 ? args[3] : led_display_kw_get(kw_args, MP_QSTR_brightness, mp_const_none);
    mp_obj_t text = n_args > 4 ? args[4] : led_display_kw_get(kw_args, MP_QSTR_text, mp_const_none);
    mp_obj_t delay_ms = n_args > 5 ? args[5] : mp_obj_new_int(50);
    mp_obj_t pixel_step = n_args > 6 ? args[6] : mp_obj_new_int(1);
    mp_obj_t end_hold_ms = n_args > 7 ? args[7] : mp_obj_new_int(800);
    
    delay_ms = led_display_kw_get(kw_args, MP_QSTR_delay_ms, delay_ms);
    pixel_step = led_display_kw_get(kw_args, MP_QSTR_pixel_step, pixel_step);
    end_hold_ms = led_display_kw_get(kw_args, MP_QSTR_end_hold_ms, end_hold_ms);
    
    if (start_row == mp_const_none || end_row == mp_const_none || brightness == mp_const_none || text == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("start_row, end_row, brightness, and text required"));
    }
    
    return led_display_configure_custom_scroll_zone(args[0], start_row, end_row, brightness, text, delay_ms, pixel_step, end_hold_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_configure_custom_scroll_zone_obj, 1, led_display_configure_custom_scroll_zone_kw);

static mp_obj_t led_display_configure_menu_blink_zone(mp_obj_t self_in, mp_obj_t start_row, mp_obj_t end_row, mp_obj_t brightness, mp_obj_t text, mp_obj_t blink_positions, mp_obj_t period_ms) {
    int start = mp_obj_get_int(start_row);
    int end = mp_obj_get_int(end_row);
    int bright = mp_obj_get_int(brightness);
    const char *txt = mp_obj_str_get_str(text);
    int period = mp_obj_get_int(period_ms);
    mp_obj_t zones_list = mp_obj_new_list(0, NULL);
    mp_obj_t zone_tuple = mp_obj_new_tuple(3, (mp_obj_t[]){
        MP_OBJ_NEW_QSTR(MP_QSTR_custom),
        mp_obj_new_int(start),
        mp_obj_new_int(end)
    });
    mp_obj_list_append(zones_list, zone_tuple);
    led_display_set_zones(self_in, zones_list);
    led_display_set_zone_brightness(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_custom), mp_obj_new_int(bright));
    mp_obj_t msg_list = mp_obj_new_list(1, (mp_obj_t[]){ mp_obj_new_str(txt, strlen(txt)) });
    led_display_set_zone_messages(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_custom), msg_list);
    if (blink_positions != mp_const_none) {
        led_display_set_zone_blink_positions(self_in, MP_OBJ_NEW_QSTR(MP_QSTR_custom), blink_positions, mp_obj_new_int(period));
    }
    return mp_const_none;
}
static mp_obj_t led_display_configure_menu_blink_zone_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 7, true);
    mp_obj_t start_row = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_start_row, mp_const_none);
    mp_obj_t end_row = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_end_row, mp_const_none);
    mp_obj_t brightness = n_args > 3 ? args[3] : led_display_kw_get(kw_args, MP_QSTR_brightness, mp_const_none);
    mp_obj_t text = n_args > 4 ? args[4] : led_display_kw_get(kw_args, MP_QSTR_text, mp_const_none);
    mp_obj_t blink_positions = n_args > 5 ? args[5] : mp_const_none;
    mp_obj_t period_ms = n_args > 6 ? args[6] : mp_obj_new_int(500);
    
    blink_positions = led_display_kw_get(kw_args, MP_QSTR_blink_positions, blink_positions);
    period_ms = led_display_kw_get(kw_args, MP_QSTR_period_ms, period_ms);

    if (start_row == mp_const_none || end_row == mp_const_none || brightness == mp_const_none || text == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("start_row, end_row, brightness, and text required"));
    }

    return led_display_configure_menu_blink_zone(args[0], start_row, end_row, brightness, text, blink_positions, period_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_configure_menu_blink_zone_obj, 1, led_display_configure_menu_blink_zone_kw);

// Scroll direction, speed, etc.
static mp_obj_t led_display_set_scroll_direction(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t direction) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    const char *dir_str = mp_obj_str_get_str(direction);
    uint8_t dir = 0;
    if (strcmp(dir_str, "d") == 0 || strcmp(dir_str, "down") == 0) dir = 1;
    else if (strcmp(dir_str, "l") == 0 || strcmp(dir_str, "left") == 0) dir = 2;
    else if (strcmp(dir_str, "r") == 0 || strcmp(dir_str, "right") == 0) dir = 3;
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            self->zones[i].scroll_mgr.scroll_direction = dir;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_set_scroll_direction_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 2, 3, true);
    mp_obj_t direction = args[1];
    mp_obj_t zone = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_zone, mp_const_none);
    if (zone == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone required"));
    }
    return led_display_set_scroll_direction(args[0], zone, direction);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_scroll_direction_obj, 2, led_display_set_scroll_direction_kw);

static mp_obj_t led_display_set_scroll_speed(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t delay_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    uint32_t delay = mp_obj_get_int(delay_ms);
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) self->zones[i].scroll_mgr.scroll_delay_ms = delay;
    return mp_const_none;
}
static mp_obj_t led_display_set_scroll_speed_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 2, 3, true);
    mp_obj_t delay_ms = args[1];
    mp_obj_t zone = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_zone, mp_const_none);
    if (zone == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone required"));
    }
    return led_display_set_scroll_speed(args[0], zone, delay_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_scroll_speed_obj, 2, led_display_set_scroll_speed_kw);
static mp_obj_t led_display_set_scroll_pixel_step(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t step) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    uint8_t pixel_step = mp_obj_get_int(step);
    if (pixel_step < 1) pixel_step = 1;
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) self->zones[i].scroll_mgr.pixel_step = pixel_step;
    return mp_const_none;
}
static mp_obj_t led_display_set_scroll_pixel_step_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 2, 3, true);
    mp_obj_t step = args[1];
    mp_obj_t zone = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_zone, mp_const_none);
    if (zone == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone required"));
    }
    return led_display_set_scroll_pixel_step(args[0], zone, step);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_scroll_pixel_step_obj, 2, led_display_set_scroll_pixel_step_kw);
static mp_obj_t led_display_set_scroll_mode(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t mode) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    const char *mode_str = mp_obj_str_get_str(mode);
    uint8_t smode = (strcmp(mode_str, "pixel") == 0 || strcmp(mode_str, "p") == 0) ? SCROLL_P :
                    (strcmp(mode_str, "loop") == 0 || strcmp(mode_str, "l") == 0) ? SCROLL_LOOP : SCROLL_C;
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) self->zones[i].scroll_mgr.scroll_mode = smode;
    return mp_const_none;
}
static mp_obj_t led_display_set_scroll_mode_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 2, 3, true);
    mp_obj_t mode = args[1];
    mp_obj_t zone = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_zone, mp_const_none);
    if (zone == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone required"));
    }
    return led_display_set_scroll_mode(args[0], zone, mode);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_scroll_mode_obj, 2, led_display_set_scroll_mode_kw);

static mp_obj_t led_display_get_scroll_position(mp_obj_t self_in, mp_obj_t zone_name) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) return mp_obj_new_int(self->zones[i].scroll_mgr.scroll_pos);
    return mp_obj_new_int(0);
}
static mp_obj_t led_display_get_scroll_position_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t zone = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone, mp_const_none);
    if (zone == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone required"));
    }
    return led_display_get_scroll_position(args[0], zone);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_get_scroll_position_obj, 1, led_display_get_scroll_position_kw);
static mp_obj_t led_display_reset_scroll(mp_obj_t self_in, mp_obj_t zone_name) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            self->zones[i].scroll_mgr.scroll_pos = 0;
            self->zones[i].scroll_mgr.pixel_offset = 0;
            break;
        }
    }
    return mp_const_none;
}
static mp_obj_t led_display_reset_scroll_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t zone = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_zone, mp_const_none);
    if (zone == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("zone required"));
    }
    return led_display_reset_scroll(args[0], zone);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_reset_scroll_obj, 1, led_display_reset_scroll_kw);

static mp_obj_t led_display_set_mode(mp_obj_t self_in, mp_obj_t mode_in) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *mode = mp_obj_str_get_str(mode_in);
    
    if (strcmp(mode, "time") == 0 || strcmp(mode, "time_only") == 0) {
        return led_display_configure_time_zone(self_in, mp_obj_new_int(0), mp_const_none, mp_obj_new_int(63), mp_const_false, mp_const_none);
    } else if (strcmp(mode, "time_seconds") == 0) {
        return led_display_configure_time_zone(self_in, mp_obj_new_int(0), mp_const_none, mp_obj_new_int(63), mp_const_true, mp_const_none);
    } else if (strcmp(mode, "scroll") == 0 || strcmp(mode, "custom") == 0) {
        return led_display_configure_custom_scroll_zone(self_in, mp_obj_new_int(0), mp_obj_new_int(self->rows-1), mp_obj_new_int(63), mp_obj_new_str("SCROLL", 6), mp_obj_new_int(50), mp_obj_new_int(1), mp_obj_new_int(800));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_display_set_mode_obj, led_display_set_mode);
// Font management
static mp_obj_t led_display_load_external_font(mp_obj_t self_in, mp_obj_t name_obj, mp_obj_t font_dict) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (!mp_obj_is_type(font_dict, &mp_type_dict)) {
        mp_raise_TypeError(MP_ERROR_TEXT("font_dict must be a dict"));
    }
    if (self->font_count >= 8) mp_raise_ValueError(MP_ERROR_TEXT("max fonts reached"));
    const char *name = mp_obj_str_get_str(name_obj);
    mp_map_t *map = &((mp_obj_dict_t *)MP_OBJ_TO_PTR(font_dict))->map;
    mp_map_elem_t *elem;
    
    mp_obj_t width_obj = mp_const_none;
    if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_Width), MP_MAP_LOOKUP)) != NULL) {
        width_obj = elem->value;
    } else if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_width), MP_MAP_LOOKUP)) != NULL) {
        width_obj = elem->value;
    } else {
        mp_raise_msg_varg(&mp_type_KeyError, MP_OBJ_NEW_QSTR(MP_QSTR_Width));
    }

    mp_obj_t height_obj = mp_const_none;
    if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_Height), MP_MAP_LOOKUP)) != NULL) {
        height_obj = elem->value;
    } else if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_height), MP_MAP_LOOKUP)) != NULL) {
        height_obj = elem->value;
    } else {
        mp_raise_msg_varg(&mp_type_KeyError, MP_OBJ_NEW_QSTR(MP_QSTR_Height));
    }

    mp_obj_t start_obj = mp_const_none;
    if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_Start), MP_MAP_LOOKUP)) != NULL) {
        start_obj = elem->value;
    } else if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_start), MP_MAP_LOOKUP)) != NULL) {
        start_obj = elem->value;
    } else {
        mp_raise_msg_varg(&mp_type_KeyError, MP_OBJ_NEW_QSTR(MP_QSTR_Start));
    }

    mp_obj_t end_obj = mp_const_none;
    if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_End), MP_MAP_LOOKUP)) != NULL) {
        end_obj = elem->value;
    } else if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_end), MP_MAP_LOOKUP)) != NULL) {
        end_obj = elem->value;
    } else {
        mp_raise_msg_varg(&mp_type_KeyError, MP_OBJ_NEW_QSTR(MP_QSTR_End));
    }

    mp_obj_t spacing_obj = mp_obj_new_int(2);
    if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_Spacing), MP_MAP_LOOKUP)) != NULL) {
        spacing_obj = elem->value;
    } else if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_spacing), MP_MAP_LOOKUP)) != NULL) {
        spacing_obj = elem->value;
    }

    mp_obj_t data_obj = mp_const_none;
    if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_Data), MP_MAP_LOOKUP)) != NULL) {
        data_obj = elem->value;
    } else if ((elem = mp_map_lookup(map, MP_OBJ_NEW_QSTR(MP_QSTR_data), MP_MAP_LOOKUP)) != NULL) {
        data_obj = elem->value;
    } else {
        mp_raise_msg_varg(&mp_type_KeyError, MP_OBJ_NEW_QSTR(MP_QSTR_Data));
    }
    uint8_t idx = self->font_count++;
    strncpy(self->fonts[idx].name, name, 15);
    self->fonts[idx].width = mp_obj_get_int(width_obj);
    self->fonts[idx].height = mp_obj_get_int(height_obj);
    self->fonts[idx].spacing = mp_obj_get_int(spacing_obj);
    self->fonts[idx].start_char = mp_obj_get_int(start_obj);
    self->fonts[idx].end_char = mp_obj_get_int(end_obj);
    
    size_t num_chars = self->fonts[idx].end_char - self->fonts[idx].start_char + 1;
    size_t data_size = num_chars * self->fonts[idx].width;
    uint16_t *data_copy = m_new(uint16_t, data_size);

    if (mp_obj_is_type(data_obj, &mp_type_list)) {
        size_t len;
        mp_obj_t *items;
        mp_obj_list_get(data_obj, &len, &items);
        size_t copy_len = (len < data_size) ? len : data_size;
        for (size_t i = 0; i < copy_len; i++) {
            data_copy[i] = (uint16_t)mp_obj_get_int(items[i]);
        }
        if (len < data_size) {
            memset(data_copy + len, 0, (data_size - len) * sizeof(uint16_t));
        }
    } else {
        mp_buffer_info_t bufinfo;
        mp_get_buffer_raise(data_obj, &bufinfo, MP_BUFFER_READ);
        // If buffer is 8-bit, copy with conversion
        if (bufinfo.typecode == 'b' || bufinfo.typecode == 'B') {
            uint8_t *src = bufinfo.buf;
            for (size_t i = 0; i < data_size && i < bufinfo.len; i++) {
                data_copy[i] = src[i];
            }
        } else {
            // Assume 16-bit or just raw copy if bytes
            size_t copy_bytes = (bufinfo.len < data_size * sizeof(uint16_t)) ? bufinfo.len : data_size * sizeof(uint16_t);
            memcpy(data_copy, bufinfo.buf, copy_bytes);
        }
    }
    self->fonts[idx].data = data_copy;
    return mp_const_none;
}
static mp_obj_t led_display_load_external_font_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 2, 3, true);
    mp_obj_t font_dict = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_font_dict, mp_const_none);
    if (font_dict == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("font_dict required"));
    }
    return led_display_load_external_font(args[0], args[1], font_dict);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_load_external_font_obj, 2, led_display_load_external_font_kw);

static mp_obj_t led_display_set_font(mp_obj_t self_in, mp_obj_t name_obj) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(name_obj);
    for (uint8_t i = 0; i < self->font_count; i++) {
        if (strcmp(self->fonts[i].name, name) == 0) {
            self->current_font_idx = i;
            self->font_width = self->fonts[i].width;
            self->font_height = self->fonts[i].height;
            self->font_spacing = self->fonts[i].spacing;
            self->font_data = self->fonts[i].data;
            for (uint8_t z = 0; z < self->zone_count; z++) {
                zone_config_t *zone = &self->zones[z];
                uint16_t new_max = zone->height / (self->font_height + self->font_spacing);
                if (new_max < 1) new_max = 1;
                zone->scroll_mgr.max_chars = new_max;
                zone->scroll_mgr.char_height = self->font_height;
                zone->scroll_mgr.spacing = self->font_spacing;
            }
            return mp_const_true;
        }
    }
    return mp_const_false;
}
static mp_obj_t led_display_set_font_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t name = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_name, mp_const_none);
    if (name == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("name required"));
    }
    return led_display_set_font(args[0], name);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_font_obj, 1, led_display_set_font_kw);

// Resize matrix
static mp_obj_t led_display_resize(mp_obj_t self_in, mp_obj_t rows_obj, mp_obj_t cols_obj) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    uint16_t new_rows = mp_obj_get_int(rows_obj);
    uint8_t new_cols = mp_obj_get_int(cols_obj);
    if (new_rows == 0 || new_cols == 0) mp_raise_ValueError(MP_ERROR_TEXT("invalid size"));
    m_del(uint8_t, self->framebuffer, self->fb_rows * self->fb_cols * 3);
    self->rows = new_rows;
    self->cols = new_cols;
    self->fb_rows = new_rows;
    self->fb_cols = new_cols;
    self->framebuffer = m_new(uint8_t, new_rows * new_cols * 3);
    memset(self->framebuffer, 0, new_rows * new_cols * 3);
    self->led_buffer_valid = false;
    self->zone_count = 0;
    return mp_const_none;
}
static mp_obj_t led_display_resize_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 3, true);
    mp_obj_t rows = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_rows, mp_const_none);
    mp_obj_t cols = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_cols, mp_const_none);
    if (rows == mp_const_none || cols == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("rows and cols required"));
    }
    return led_display_resize(args[0], rows, cols);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_resize_obj, 1, led_display_resize_kw);

// Color Manager API
static mp_obj_t led_display_set_char_color(mp_obj_t self_in, mp_obj_t position, mp_obj_t r, mp_obj_t g, mp_obj_t b) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    set_char_color(self, mp_obj_get_int(position), mp_obj_get_int(r), mp_obj_get_int(g), mp_obj_get_int(b));
    return mp_const_none;
}
static mp_obj_t led_display_set_char_color_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 5, true);
    mp_obj_t position = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_position, mp_const_none);
    mp_obj_t r = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_r, mp_const_none);
    mp_obj_t g = n_args > 3 ? args[3] : led_display_kw_get(kw_args, MP_QSTR_g, mp_const_none);
    mp_obj_t b = n_args > 4 ? args[4] : led_display_kw_get(kw_args, MP_QSTR_b, mp_const_none);
    if (position == mp_const_none || r == mp_const_none || g == mp_const_none || b == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("position, r, g, b required"));
    }
    return led_display_set_char_color(args[0], position, r, g, b);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_char_color_obj, 1, led_display_set_char_color_kw);
static mp_obj_t led_display_set_section_color(mp_obj_t self_in, mp_obj_t start, mp_obj_t end, mp_obj_t r, mp_obj_t g, mp_obj_t b) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    set_section_color(self, mp_obj_get_int(start), mp_obj_get_int(end), mp_obj_get_int(r), mp_obj_get_int(g), mp_obj_get_int(b));
    return mp_const_none;
}
static mp_obj_t led_display_set_section_color_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 6, true);
    mp_obj_t start = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_start_pos, mp_const_none);
    mp_obj_t end = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_end_pos, mp_const_none);
    mp_obj_t r = n_args > 3 ? args[3] : led_display_kw_get(kw_args, MP_QSTR_r, mp_const_none);
    mp_obj_t g = n_args > 4 ? args[4] : led_display_kw_get(kw_args, MP_QSTR_g, mp_const_none);
    mp_obj_t b = n_args > 5 ? args[5] : led_display_kw_get(kw_args, MP_QSTR_b, mp_const_none);
    if (start == mp_const_none) start = led_display_kw_get(kw_args, MP_QSTR_start, mp_const_none);
    if (end == mp_const_none) end = led_display_kw_get(kw_args, MP_QSTR_end, mp_const_none);
    if (start == mp_const_none || end == mp_const_none || r == mp_const_none || g == mp_const_none || b == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("start_pos, end_pos, r, g, b required"));
    }
    return led_display_set_section_color(args[0], start, end, r, g, b);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_section_color_obj, 1, led_display_set_section_color_kw);
static mp_obj_t led_display_clear_all_colors(mp_obj_t self_in) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    clear_all_colors(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_display_clear_all_colors_obj, led_display_clear_all_colors);

// Other utilities
static mp_obj_t led_display_set_zone_sequence_duration(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    uint32_t duration = mp_obj_get_int(ms);
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) self->zones[i].seq_duration = duration;
    return mp_const_none;
}
static mp_obj_t led_display_set_zone_sequence_duration_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 2, 3, true);
    mp_obj_t ms = n_args > 2 ? args[2] : led_display_kw_get(kw_args, MP_QSTR_ms, mp_const_none);
    if (ms == mp_const_none) {
        ms = led_display_kw_get(kw_args, MP_QSTR_duration_ms, mp_const_none);
    }
    if (ms == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("ms required"));
    }
    return led_display_set_zone_sequence_duration(args[0], args[1], ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_zone_sequence_duration_obj, 2, led_display_set_zone_sequence_duration_kw);
static mp_obj_t led_display_set_animation(mp_obj_t self_in, mp_obj_t anim_type, mp_obj_t duration_ms, mp_obj_t stagger_ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *anim_str = mp_obj_str_get_str(anim_type);
    uint8_t anim = AN_N;
    if (strcmp(anim_str, "f") == 0 || strcmp(anim_str, "fade") == 0) anim = AN_F;
    else if (strcmp(anim_str, "ft") == 0 || strcmp(anim_str, "fade_through") == 0) anim = AN_FT;
    else if (strcmp(anim_str, "su") == 0 || strcmp(anim_str, "slide_up") == 0) anim = AN_SU;
    else if (strcmp(anim_str, "sd") == 0 || strcmp(anim_str, "slide_down") == 0) anim = AN_SD;
    else if (strcmp(anim_str, "sl") == 0 || strcmp(anim_str, "slide_left") == 0) anim = AN_SL;
    else if (strcmp(anim_str, "sr") == 0 || strcmp(anim_str, "slide_right") == 0) anim = AN_SR;
    self->global_anim_type = anim;
    self->global_anim_duration = mp_obj_get_int(duration_ms);
    self->global_anim_stagger = mp_obj_get_int(stagger_ms);
    return mp_const_none;
}
static mp_obj_t led_display_set_animation_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 2, 4, true);
    mp_obj_t duration_ms = n_args > 2 ? args[2] : mp_obj_new_int(120);
    mp_obj_t stagger_ms = n_args > 3 ? args[3] : mp_obj_new_int(15);
    duration_ms = led_display_kw_get(kw_args, MP_QSTR_duration_ms, duration_ms);
    stagger_ms = led_display_kw_get(kw_args, MP_QSTR_stagger_ms, stagger_ms);
    return led_display_set_animation(args[0], args[1], duration_ms, stagger_ms);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_animation_obj, 2, led_display_set_animation_kw);

static mp_obj_t led_display_set_render_direction(mp_obj_t self_in, mp_obj_t direction) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->render_direction = mp_obj_get_int(direction);
    return mp_const_none;
}
static mp_obj_t led_display_set_render_direction_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t direction = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_direction, mp_const_none);
    if (direction == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("direction required"));
    }
    return led_display_set_render_direction(args[0], direction);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_render_direction_obj, 1, led_display_set_render_direction_kw);
static mp_obj_t led_display_set_copy_black_pixels(mp_obj_t self_in, mp_obj_t enabled) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->write_black_pixels = mp_obj_is_true(enabled);
    return mp_const_none;
}
static mp_obj_t led_display_set_copy_black_pixels_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t enabled = n_args > 1 ? args[1] : mp_const_true;
    enabled = led_display_kw_get(kw_args, MP_QSTR_enabled, enabled);
    return led_display_set_copy_black_pixels(args[0], enabled);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_copy_black_pixels_obj, 1, led_display_set_copy_black_pixels_kw);
static mp_obj_t led_display_set_led_clear_before_copy(mp_obj_t self_in, mp_obj_t enabled) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->clear_led_before_copy = mp_obj_is_true(enabled);
    return mp_const_none;
}
static mp_obj_t led_display_set_led_clear_before_copy_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t enabled = n_args > 1 ? args[1] : mp_const_true;
    enabled = led_display_kw_get(kw_args, MP_QSTR_enabled, enabled);
    return led_display_set_led_clear_before_copy(args[0], enabled);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_set_led_clear_before_copy_obj, 1, led_display_set_led_clear_before_copy_kw);

static mp_obj_t led_display_get_display_info(mp_obj_t self_in) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t dict = mp_obj_new_dict(6);
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_rows), mp_obj_new_int(self->rows));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_columns), mp_obj_new_int(self->cols));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_brightness), mp_obj_new_int(self->brightness));
    mp_obj_t font_dict = mp_obj_new_dict(5);
    mp_obj_dict_store(font_dict, MP_OBJ_NEW_QSTR(MP_QSTR_name), mp_obj_new_str(self->fonts[self->current_font_idx].name, strlen(self->fonts[self->current_font_idx].name)));
    mp_obj_dict_store(font_dict, MP_OBJ_NEW_QSTR(MP_QSTR_width), mp_obj_new_int(self->font_width));
    mp_obj_dict_store(font_dict, MP_OBJ_NEW_QSTR(MP_QSTR_height), mp_obj_new_int(self->font_height));
    mp_obj_dict_store(font_dict, MP_OBJ_NEW_QSTR(MP_QSTR_spacing), mp_obj_new_int(self->font_spacing));
    mp_obj_dict_store(dict, MP_OBJ_NEW_QSTR(MP_QSTR_font), font_dict);
    return dict;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_display_get_display_info_obj, led_display_get_display_info);

static mp_obj_t led_display_calculate_height(mp_obj_t self_in, mp_obj_t char_count) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(mp_obj_get_int(char_count) * (self->font_height + self->font_spacing));
}
static mp_obj_t led_display_calculate_height_kw(size_t n_args, const mp_obj_t *args, mp_map_t *kw_args) {
    mp_arg_check_num(n_args, kw_args == NULL ? 0 : kw_args->used, 1, 2, true);
    mp_obj_t char_count = n_args > 1 ? args[1] : led_display_kw_get(kw_args, MP_QSTR_char_count, mp_const_none);
    if (char_count == mp_const_none) {
        mp_raise_TypeError(MP_ERROR_TEXT("char_count required"));
    }
    return led_display_calculate_height(args[0], char_count);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(led_display_calculate_height_obj, 1, led_display_calculate_height_kw);
static mp_obj_t led_display_print_display_info(mp_obj_t self_in) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    printf("\n=== LED Display System Info ===\n");
    printf("  Rows: %d, Columns: %d\n", self->rows, self->cols);
    printf("  Framebuffer: %d x %d, size %d bytes\n", self->fb_rows, self->fb_cols, self->fb_rows * self->fb_cols * 3);
    printf("  Brightness: %d/255\n", self->brightness);
    printf("  Font: %s (%dx%d, spacing %d)\n", self->fonts[self->current_font_idx].name, self->font_width, self->font_height, self->font_spacing);
    printf("  Write black pixels: %s\n", self->write_black_pixels ? "yes" : "no");
    printf("  Zones: %d\n", self->zone_count);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_display_print_display_info_obj, led_display_print_display_info);

static mp_obj_t led_display_copy_to_led_buffer(mp_obj_t self_in) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    copy_to_led_buffer(self, self->clear_led_before_copy);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_display_copy_to_led_buffer_obj, led_display_copy_to_led_buffer);

static mp_obj_t led_display_set_copy_alpha(mp_obj_t self_in, mp_obj_t alpha) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    int v = mp_obj_get_int(alpha);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    self->copy_alpha = (uint8_t)v;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_display_set_copy_alpha_obj, led_display_set_copy_alpha);

static mp_obj_t led_display_set_zone_type(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t type_str) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    const char *type = mp_obj_str_get_str(type_str);
    uint8_t zt = (strcmp(type, "time") == 0) ? 1 : 0;
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) { self->zones[i].zone_type = zt; break; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(led_display_set_zone_type_obj, led_display_set_zone_type);

static mp_obj_t led_display_clear_framebuffer(mp_obj_t self_in) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    fb_clear(self);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(led_display_clear_framebuffer_obj, led_display_clear_framebuffer);

static mp_obj_t led_display_set_update_interval(mp_obj_t self_in, mp_obj_t ms) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    self->update_interval = mp_obj_get_int(ms);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_display_set_update_interval_obj, led_display_set_update_interval);

static mp_obj_t led_display_set_pixel(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    led_display_obj_t *self = MP_OBJ_TO_PTR(args[0]);
    fb_pixel_rgb(self, mp_obj_get_int(args[1]), mp_obj_get_int(args[2]),
                 mp_obj_get_int(args[3]), mp_obj_get_int(args[4]), mp_obj_get_int(args[5]));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(led_display_set_pixel_obj, 6, 6, led_display_set_pixel);

static mp_obj_t led_display_set_zone_align(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t align_str) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    const char *align = mp_obj_str_get_str(align_str);
    uint8_t a = 0;
    if (strcmp(align, "center") == 0 || strcmp(align, "c") == 0) a = 1;
    else if (strcmp(align, "bottom") == 0 || strcmp(align, "b") == 0) a = 2;
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) { self->zones[i].text_align = a; break; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(led_display_set_zone_align_obj, led_display_set_zone_align);

static mp_obj_t led_display_set_zone_enabled(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t enabled) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    bool en = mp_obj_is_true(enabled);
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) { self->zones[i].zone_enabled = en; break; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(led_display_set_zone_enabled_obj, led_display_set_zone_enabled);

static mp_obj_t led_display_get_zone_enabled(mp_obj_t self_in, mp_obj_t zone_name) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            return mp_obj_new_bool(self->zones[i].zone_enabled);
        }
    }
    return mp_const_false;
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_display_get_zone_enabled_obj, led_display_get_zone_enabled);

static mp_obj_t led_display_zone_next_message(mp_obj_t self_in, mp_obj_t zone_name) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            if (z->msg_count > 0)
                z->seq_index = (z->seq_index + 1) % z->msg_count;
            z->seq_last_change = mp_hal_ticks_ms();
            z->scroll_source_last[0] = '\0';
            break;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_display_zone_next_message_obj, led_display_zone_next_message);

static mp_obj_t led_display_zone_set_message(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t index) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    int idx = mp_obj_get_int(index);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            if (z->msg_count > 0) {
                if (idx < 0) idx = 0;
                if (idx >= z->msg_count) idx = z->msg_count - 1;
                z->seq_index = (uint8_t)idx;
            }
            z->seq_last_change = mp_hal_ticks_ms();
            z->scroll_source_last[0] = '\0';
            break;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(led_display_zone_set_message_obj, led_display_zone_set_message);

static mp_obj_t led_display_set_zone_alpha(mp_obj_t self_in, mp_obj_t zone_name, mp_obj_t alpha) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    int v = mp_obj_get_int(alpha);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0) { self->zones[i].zone_alpha = (uint8_t)v; break; }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(led_display_set_zone_alpha_obj, led_display_set_zone_alpha);

static mp_obj_t led_display_get_zone_text(mp_obj_t self_in, mp_obj_t zone_name) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++) {
        if (strcmp(self->zones[i].name, name) == 0) {
            zone_config_t *z = &self->zones[i];
            const char *text = (z->scroll_enabled && z->scroll_mgr.active)
                ? z->scroll_mgr.current_cache
                : z->last_text;
            return mp_obj_new_str(text, strlen(text));
        }
    }
    return mp_obj_new_str("", 0);
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_display_get_zone_text_obj, led_display_get_zone_text);

static mp_obj_t led_display_zone_scroll_done(mp_obj_t self_in, mp_obj_t zone_name) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    const char *name = mp_obj_str_get_str(zone_name);
    for (uint8_t i = 0; i < self->zone_count; i++)
        if (strcmp(self->zones[i].name, name) == 0)
            return mp_obj_new_bool(scroll_manager_is_ready(&self->zones[i].scroll_mgr));
    return mp_const_true;
}
static MP_DEFINE_CONST_FUN_OBJ_2(led_display_zone_scroll_done_obj, led_display_zone_scroll_done);

// ========== Method table ==========
static const mp_rom_map_elem_t led_display_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_set_brightness), MP_ROM_PTR(&led_display_set_brightness_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zones), MP_ROM_PTR(&led_display_set_zones_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_brightness), MP_ROM_PTR(&led_display_set_zone_brightness_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_colors), MP_ROM_PTR(&led_display_set_zone_colors_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_messages), MP_ROM_PTR(&led_display_set_zone_messages_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_scroll), MP_ROM_PTR(&led_display_set_zone_scroll_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_presentation), MP_ROM_PTR(&led_display_set_zone_presentation_obj) },
    { MP_ROM_QSTR(MP_QSTR_enable_colon_blink), MP_ROM_PTR(&led_display_enable_colon_blink_obj) },
    { MP_ROM_QSTR(MP_QSTR_disable_colon_blink), MP_ROM_PTR(&led_display_disable_colon_blink_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_blink_chars), MP_ROM_PTR(&led_display_set_zone_blink_chars_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_blink_positions), MP_ROM_PTR(&led_display_set_zone_blink_positions_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_blink), MP_ROM_PTR(&led_display_set_zone_blink_obj) },
    { MP_ROM_QSTR(MP_QSTR_configure_time_zone), MP_ROM_PTR(&led_display_configure_time_zone_obj) },
    { MP_ROM_QSTR(MP_QSTR_configure_custom_scroll_zone), MP_ROM_PTR(&led_display_configure_custom_scroll_zone_obj) },
    { MP_ROM_QSTR(MP_QSTR_configure_menu_blink_zone), MP_ROM_PTR(&led_display_configure_menu_blink_zone_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_scroll_direction), MP_ROM_PTR(&led_display_set_scroll_direction_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_scroll_speed), MP_ROM_PTR(&led_display_set_scroll_speed_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_scroll_pixel_step), MP_ROM_PTR(&led_display_set_scroll_pixel_step_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_scroll_mode), MP_ROM_PTR(&led_display_set_scroll_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_scroll_position), MP_ROM_PTR(&led_display_get_scroll_position_obj) },
    { MP_ROM_QSTR(MP_QSTR_reset_scroll), MP_ROM_PTR(&led_display_reset_scroll_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_external_font), MP_ROM_PTR(&led_display_load_external_font_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_font), MP_ROM_PTR(&led_display_set_font_obj) },
    { MP_ROM_QSTR(MP_QSTR_resize), MP_ROM_PTR(&led_display_resize_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_char_color), MP_ROM_PTR(&led_display_set_char_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_section_color), MP_ROM_PTR(&led_display_set_section_color_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_all_colors), MP_ROM_PTR(&led_display_clear_all_colors_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_sequence_duration), MP_ROM_PTR(&led_display_set_zone_sequence_duration_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_type), MP_ROM_PTR(&led_display_set_zone_type_obj) },
    { MP_ROM_QSTR(MP_QSTR_clear_framebuffer), MP_ROM_PTR(&led_display_clear_framebuffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_update_interval), MP_ROM_PTR(&led_display_set_update_interval_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_pixel), MP_ROM_PTR(&led_display_set_pixel_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_animation), MP_ROM_PTR(&led_display_set_animation_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_mode), MP_ROM_PTR(&led_display_set_mode_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_render_direction), MP_ROM_PTR(&led_display_set_render_direction_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_copy_black_pixels), MP_ROM_PTR(&led_display_set_copy_black_pixels_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_led_clear_before_copy), MP_ROM_PTR(&led_display_set_led_clear_before_copy_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_copy_alpha), MP_ROM_PTR(&led_display_set_copy_alpha_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_align), MP_ROM_PTR(&led_display_set_zone_align_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_enabled), MP_ROM_PTR(&led_display_set_zone_enabled_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_zone_enabled), MP_ROM_PTR(&led_display_get_zone_enabled_obj) },
    { MP_ROM_QSTR(MP_QSTR_zone_next_message), MP_ROM_PTR(&led_display_zone_next_message_obj) },
    { MP_ROM_QSTR(MP_QSTR_zone_set_message), MP_ROM_PTR(&led_display_zone_set_message_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_zone_alpha), MP_ROM_PTR(&led_display_set_zone_alpha_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_zone_text), MP_ROM_PTR(&led_display_get_zone_text_obj) },
    { MP_ROM_QSTR(MP_QSTR_zone_scroll_done), MP_ROM_PTR(&led_display_zone_scroll_done_obj) },
    { MP_ROM_QSTR(MP_QSTR_get_display_info), MP_ROM_PTR(&led_display_get_display_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_calculate_height), MP_ROM_PTR(&led_display_calculate_height_obj) },
    { MP_ROM_QSTR(MP_QSTR_print_display_info), MP_ROM_PTR(&led_display_print_display_info_obj) },
    { MP_ROM_QSTR(MP_QSTR_copy_to_led_buffer), MP_ROM_PTR(&led_display_copy_to_led_buffer_obj) },
    { MP_ROM_QSTR(MP_QSTR_update), MP_ROM_PTR(&led_display_update_obj) },
};
static MP_DEFINE_CONST_DICT(led_display_locals_dict, led_display_locals_dict_table);

// Proxy attribute handlers
static void led_display_zone_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    led_display_zone_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) { // Load
        if (attr == MP_QSTR_sequence_index) {
            dest[0] = mp_obj_new_int(self->disp->zones[self->zone_idx].seq_index);
        } else if (attr == MP_QSTR_name) {
            dest[0] = mp_obj_new_str(self->disp->zones[self->zone_idx].name, strlen(self->disp->zones[self->zone_idx].name));
        } else if (attr == MP_QSTR_zone_enabled) {
            dest[0] = mp_obj_new_bool(self->disp->zones[self->zone_idx].zone_enabled);
        }
    } else if (dest[1] != MP_OBJ_NULL) { // Store
        if (attr == MP_QSTR_sequence_index) {
            self->disp->zones[self->zone_idx].seq_index = mp_obj_get_int(dest[1]);
            dest[0] = MP_OBJ_NULL; // Success
        } else if (attr == MP_QSTR_zone_enabled) {
            self->disp->zones[self->zone_idx].zone_enabled = mp_obj_is_true(dest[1]);
            dest[0] = MP_OBJ_NULL; // Success
        }
    }
}



MP_DEFINE_CONST_OBJ_TYPE(
    led_display_zone_type,
    MP_QSTR_LEDZone,
    MP_TYPE_FLAG_NONE,
    attr, led_display_zone_attr
);

static void led_display_color_manager_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    led_display_color_manager_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) { // Load
        if (attr == MP_QSTR_set_char_color) {
            dest[0] = mp_load_attr(MP_OBJ_FROM_PTR(self->disp), MP_QSTR_set_char_color);
        } else if (attr == MP_QSTR_clear_all_colors) {
            dest[0] = mp_load_attr(MP_OBJ_FROM_PTR(self->disp), MP_QSTR_clear_all_colors);
        }
    }
}

MP_DEFINE_CONST_OBJ_TYPE(
    led_display_color_manager_type,
    MP_QSTR_ColorManager,
    MP_TYPE_FLAG_NONE,
    attr, led_display_color_manager_attr
);

// LEDDisplaySystem attribute handler
static void led_display_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    led_display_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) { // Load
        if (attr == MP_QSTR_zones) {
            mp_obj_t list = mp_obj_new_list(self->zone_count, NULL);
            for (uint8_t i = 0; i < self->zone_count; i++) {
                led_display_zone_obj_t *z = mp_obj_malloc(led_display_zone_obj_t, &led_display_zone_type);
                z->disp = self;
                z->zone_idx = i;
                mp_obj_list_store(list, mp_obj_new_int(i), MP_OBJ_FROM_PTR(z));
            }
            dest[0] = list;
            return;
        } else if (attr == MP_QSTR_color_manager) {
            led_display_color_manager_obj_t *cm = mp_obj_malloc(led_display_color_manager_obj_t, &led_display_color_manager_type);
            cm->disp = self;
            dest[0] = MP_OBJ_FROM_PTR(cm);
            return;
        } else if (attr == MP_QSTR_rows) {
            dest[0] = mp_obj_new_int(self->rows);
            return;
        } else if (attr == MP_QSTR_cols) {
            dest[0] = mp_obj_new_int(self->cols);
            return;
        }

        // Fallback to locals_dict for methods
        mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&led_display_locals_dict.map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
        if (elem != NULL) {
            mp_convert_member_lookup(self_in, mp_obj_get_type(self_in), elem->value, dest);
        }
    }
}

// Define the type - updated with attr handler
MP_DEFINE_CONST_OBJ_TYPE(
    led_display_type,
    MP_QSTR_LEDDisplaySystem,
    MP_TYPE_FLAG_NONE,
    make_new, led_display_make_new,
    attr, led_display_attr,
    locals_dict, &led_display_locals_dict
);

// ========== Module registration ==========
static const mp_rom_map_elem_t leddisplay_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_leddisplay) },
    { MP_ROM_QSTR(MP_QSTR_LEDDisplaySystem), MP_ROM_PTR(&led_display_type) },
};
static MP_DEFINE_CONST_DICT(leddisplay_module_globals, leddisplay_module_globals_table);

const mp_obj_module_t leddisplay_module = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t*)&leddisplay_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_leddisplay, leddisplay_module);
