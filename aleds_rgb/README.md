# `aleds_rgb` MicroPython C Module (ESP32 / ESP32-S3)

The `aleds_rgb` module is a high-performance driver for addressable RGB LEDs (e.g., WS2812) written as a native C user module for MicroPython. It replaces the original Python-based driver (which used `@micropython.viper` decorators), providing optimized execution speed for pixel rendering loops with zero memory allocation overhead.

---

## 1. Installation & Registration

To include this module in your custom MicroPython firmware build:
1. Copy the `aleds_rgb` folder into the user C modules directory of your MicroPython repository.
2. Update your `micropython.cmake` file with the following configuration:
   ```cmake
   add_library(usermod_aleds_rgb INTERFACE)
   target_sources(usermod_aleds_rgb INTERFACE ${CMAKE_CURRENT_LIST_DIR}/aleds_rgb/aleds_rgb.c)
   target_include_directories(usermod_aleds_rgb INTERFACE ${CMAKE_CURRENT_LIST_DIR}/aleds_rgb)
   target_link_libraries(usermod INTERFACE usermod_aleds_rgb)
   ```
3. Rebuild your MicroPython firmware. The module will be registered and importable under the name `aleds_rgb`.

---

## 2. Constants

Both the `aleds_rgb` module and the `AledsRgb` class expose the following ROM-based constants:

### Color Order:
* `ORDER_RGB` = `(0, 1, 2)`
* `ORDER_RBG` = `(0, 2, 1)`
* `ORDER_GRB` = `(1, 0, 2)` — Standard for many WS2812B strips.
* `ORDER_GBR` = `(2, 0, 1)`
* `ORDER_BRG` = `(1, 2, 0)`
* `ORDER_BGR` = `(2, 1, 0)`

### Signal Timings:
* `TIMING_800KHZ` = `(400, 850, 800, 450)` — Recommended for WS2812B.
* `TIMING_400KHZ` = `(800, 1700, 1600, 900)` — Recommended for older WS-family LEDs.

### LED Chip Type:
* `TYPE_WS2812` = `'ws2812'`

---

## 3. Class `AledsRgb` API Reference

### Constructor:
```python
AledsRgb(pin, buffer, n, bpp=3, order=ORDER_RGB, timing=TIMING_800KHZ, led_type='ws2812')
```
* **Parameters**:
  * `pin` (`int` or `machine.Pin`): The GPIO pin used to drive the LEDs.
  * `buffer` (`bytearray` or compatible buffer): A buffer of at least `n * bpp` bytes.
  * `n` (`int`): The total number of pixels on the strip.
  * `bpp` (`int`, optional, default `3`): Bytes per pixel (e.g., 3 for RGB, 4 for RGBW).
  * `order` (`tuple`, optional, default `ORDER_RGB`): The color component sequence in the physical buffer.
  * `timing` (`tuple`, optional, default `TIMING_800KHZ`): Bitstream timing tuple.
  * `led_type` (`str`, optional, default `'ws2812'`): Target LED chip architecture.

---

### Instance Attributes (Load & Store):
The following fields are exposed as read/write properties in Python:
* `pin` — A `machine.Pin` object representing the control pin.
* `n` — Number of LEDs (integer).
* `bpp` — Bytes per pixel (integer).
* `led_type` — The LED chip name (string).
* `timing` — Timing configuration (tuple).
* `order` — Color order configuration (tuple).
* `aled_buffer` — Reference to the active pixel buffer object.
* `_order_buf` — Writable bytearray containing raw indices for color mapping.
* `_tmp` — Writable temporary bytearray of size `bpp`.

---

### Subscription Interface (`__setitem__` / `__getitem__`):
You can read and write individual LED colors directly using indexing syntax:
* **Set color**: `strip[index] = (R, G, B)`
  * Converts the RGB value into the target layout sequence specified by `order` and writes it to the raw buffer.
* **Get color**: `color = strip[index]`
  * Reads the color data from the physical layout buffer and returns a standard RGB tuple.

---

### Public Methods:

#### `set_buffer(buffer)`
Assigns a new pixel buffer to the LED driver.
```python
strip.set_buffer(new_buffer)
```

#### `change_brightness(color, brightness)`
Scales a color tuple (R, G, B) by a brightness level (0–255).
```python
scaled = strip.change_brightness((255, 128, 64), 128)
# Returns: (127, 64, 32)
```

#### `apply_brightness_to_buffer(brightness)`
Scales every byte in the active pixel buffer in-place by a brightness level (0–255).
```python
strip.apply_brightness_to_buffer(200)
```

#### `apply_gamma(color, brightness, gamma_table)`
Applies gamma correction lookup from a lookup table (LUT) to scale the brightness of a color.
```python
corrected = strip.apply_gamma((255, 0, 0), 128, my_gamma_table)
```

#### `apply_br_to_buffer(gamma_table, brightness)`
Applies gamma correction lookup from `gamma_table` at the specified `brightness` index to all bytes in the pixel buffer in-place.
```python
strip.apply_br_to_buffer(my_gamma_table, 150)
```

#### `apply_br_per_pixel(gamma, buffer, modulator)`
Applies individual pixel brightness scaling using the `modulator` array (of length `n`) and `gamma` lookup table. Modyfies the target `buffer` in-place.
```python
strip.apply_br_per_pixel(gamma_table, strip.aled_buffer, modulator_array)
```

#### `clear()`
Resets the entire pixel buffer to `0` (turns all LEDs black) using an optimized `memset` in C.
```python
strip.clear()
```

#### `aled_fast_fill(ba, ba_len, value)`
Quickly fills `ba_len` 32-bit words in buffer `ba` with `value`.
```python
strip.aled_fast_fill(strip.aled_buffer, len(strip.aled_buffer) >> 2, 0xFFFFFFFF)
```

#### `aled_fill(color)`
Fills the entire strip buffer with a single color, automatically converting the color layout based on `order`.
```python
strip.aled_fill((255, 0, 255))
```

#### `set_all(color, brightness=255)`
Fills the entire strip with a single color scaled by the specified brightness.
```python
strip.set_all((0, 255, 0), brightness=128)
```

#### `fast_gradient(color1, color2)`
Fills the strip buffer with a linear color gradient transitioning from `color1` to `color2`.
```python
strip.fast_gradient((255, 0, 0), (0, 0, 255))
```

#### `fast_fill_segment(start, end, color)`
Fills a specific segment `[start, end)` of the strip with the specified color.
```python
strip.fast_fill_segment(5, 15, (255, 255, 0))
```

#### `write()`
Transmits the active pixel buffer data to the physical LEDs using the MicroPython `machine.bitstream` utility.
```python
strip.write()
```

#### `round_coordinates(coordinates)`
Converts float-based coordinate pairs into integers, useful for 2D/3D coordinate rounding during animation loops.
```python
coords = [(1.2, 3.8), (5.5, 9.1)]
rounded = strip.round_coordinates(coords)
# Returns: [(1, 4), (6, 9)]
```

---

## 4. Usage Example

The API comes with an automatic wrapper fallback that attempts to load the C module first, falling back to the Python class definition if the firmware was compiled without it.

```python
import time
from lib.aleds_rgb import AledsRgb

# Setup LED parameters
NUM_LEDS = 60
PIN_NUM = 2
led_buffer = bytearray(NUM_LEDS * 3)

# Initialize driver
leds = AledsRgb(
    pin=PIN_NUM, 
    buffer=led_buffer, 
    n=NUM_LEDS, 
    order=AledsRgb.ORDER_GRB
)

# 1. Fill entire strip with Green
leds.aled_fill((0, 255, 0))
leds.write()
time.sleep(1)

# 2. Render Red -> Blue gradient
leds.fast_gradient((255, 0, 0), (0, 0, 255))
leds.write()
time.sleep(1)

# 3. Scale brightness in-place
leds.apply_brightness_to_buffer(128) # Dim strip by 50%
leds.write()
time.sleep(1)

# 4. Control individual pixels
leds[0] = (255, 255, 255) # Set first pixel to white
print("First pixel color:", leds[0])
leds.write()
time.sleep(1)

# 5. Clear all pixels
leds.clear()
leds.write()
```
