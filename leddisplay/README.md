# leddisplay — MicroPython Complete LED Display System

A high-performance, memory-efficient LED text display and layout rendering engine for **MicroPython (ESP32-S3)** written as a custom C module. 

It is designed to drive vertical LED grids/matrices, providing advanced text zoning, multi-mode scrolling, smooth character-level transition animations, blinking configurations, and custom font loading. All frame updates are highly optimized to minimize allocation and garbage collection overhead.

---

## 🚀 Key Features

* **Multi-Zone Layouts:** Divide a physical LED matrix into up to 8 independent horizontal zones (e.g., static text, time, and scrolling text side-by-side).
* **Smooth Character Animations:** Transition characters gracefully with **Fade**, **Fade-Through**, **Slide Up**, **Slide Down**, **Slide Left**, or **Slide Right** transitions.
* **Flexible Scrolling:** Pixel-perfect scrolling per zone with loop support, custom step sizes, configurable delay/speed, and start/end hold times.
* **Blinking Configurations:** Blink individual characters (e.g., time colons `:`), specified character positions, targeted characters, or entire zones.
* **Smart Color Manager:** Override default colors per-character or per-section.
* **Dynamic Font Engine:** Uses a built-in `4x7` font by default, and supports loading up to 8 custom font dicts at runtime.
* **Alpha Blending:** Alpha blending support (0–255) for smooth zone-mixing or dimming transitions.

---

## 🔌 API Reference

### Constructor

#### `leddisplay.LEDDisplaySystem(led_device, [rows], [cols], [render_direction], [spacing])`
Initializes a new display system object.
* `led_device` (object): The underlying LED buffer or device object (which provides a writeable buffer interface or contains an `led_buffer` / `aled_object` attribute).
* `rows` (int, optional): The number of rows in the display. Defaults to `len(led_device.aled_object) / cols`.
* `cols` (int, optional): The number of columns in the display. Defaults to `3`.
* `render_direction` (int, optional): Layout configuration routing. `0` for serpentine column major (default), `1` for normal matrix row-major layout.
* `spacing` (int, optional): Default spacing in pixels between characters. Defaults to `2`.

---

### Basic System Controls

#### `.update(controller)`
The primary loop function. Evaluates animations, scrolls, blinking masks, and draws the output to the framebuffer, then copies it to the physical LED buffer.
* `controller` (object): A class instance or object containing `.hour`, `.minutes`, and `.seconds` integer properties. Required for updating zones marked as type `"time"`.

#### `.copy_to_led_buffer()`
Manually copies the current state of the display framebuffer to the underlying physical LED buffer.

#### `.clear_framebuffer()`
Clears all pixels in the display system's internal framebuffer.

#### `.set_brightness(value)`
Sets the global system brightness factor.
* `value` (int): Brightness level from `0` (off) to `255` (full brightness).

#### `.set_update_interval(ms)`
Restricts the rate at which `.update()` will repaint the screen.
* `ms` (int): Minimum interval in milliseconds between active frame draws.

#### `.set_pixel(row, col, r, g, b)`
Directly updates a pixel color in the framebuffer.
* `row` (int), `col` (int): Framebuffer coordinate.
* `r`, `g`, `b` (int): Pixel RGB color values `[0 - 255]`.

#### `.resize(rows, cols)`
Resizes the internal framebuffer and resets all defined zones.
* `rows` (int), `cols` (int): New grid size.

#### `.get_display_info()`
Returns a dictionary containing the current configuration (rows, columns, brightness, and active font details).

#### `.print_display_info()`
Prints diagnostic information about the current framebuffer, font, zones, and copy settings directly to stdout.

---

### Layout & Zone Configuration

A display can contain up to `8` zones. Each zone occupies a horizontal slice of the display.

#### `.set_zones(zone_defs)`
Configures layout zones.
* `zone_defs` (list): A list of tuples `(name, start_row, end_row)` or dicts `{"name": str, "start_row": int, "end_row": int}` defining the boundaries of each zone.

#### `.set_zone_enabled(zone_name, enabled)`
Enables or disables rendering for a specific zone.
* `zone_name` (str): Target zone name.
* `enabled` (bool): `True` to render, `False` to skip.

#### `.get_zone_enabled(zone_name)`
Returns `True` if the zone is enabled, `False` otherwise.

#### `.set_zone_brightness(zone_name, value)`
Sets a brightness scaling factor for a specific zone.
* `value` (int): Brightness scale `[0 - 255]`.

#### `.set_zone_colors(zone_name, fg, bg)`
Configures foreground (text) and background colors for the zone.
* `fg` (tuple/list): Foreground RGB color `(r, g, b)`. Pass `None` to leave unchanged.
* `bg` (tuple/list): Background RGB color `(r, g, b)`. Pass `None` to leave unchanged.

#### `.set_zone_align(zone_name, align_str)`
Aligns the text vertically within the zone boundary when scrolling is disabled.
* `align_str` (str): `"top"` / `"t"`, `"center"` / `"c"`, or `"bottom"` / `"b"`.

#### `.set_zone_type(zone_name, type_str)`
Specifies the type of zone.
* `type_str` (str): `"time"` configures the zone to pull hour/minute/seconds values from the controller; other strings default to standard text rendering.

---

### Zone Sequence & Text Management

Each zone can store up to 16 messages and loop through them sequentially.

#### `.set_zone_messages(zone_name, messages)`
Loads a list of strings into a zone.
* `messages` (list of str): Up to 16 strings. Loading more than one message automatically switches the zone's presentation mode to sequenced (`"sq"`).

#### `.set_zone_sequence_duration(zone_name, ms)`
Sets the display duration for each message when sequenced presentation is active.
* `ms` (int): Time in milliseconds to show each message (default `3000`).

#### `.zone_next_message(zone_name)`
Forces the zone to skip immediately to the next message in its sequence.

#### `.zone_set_message(zone_name, index)`
Sets the active message sequence index.
* `index` (int): Index from `0` to `message_count - 1`.

#### `.get_zone_text(zone_name)`
Returns the current string (or substring if scrolling) displayed within the zone.

---

### Scroll Management

#### `.set_zone_scroll(zone_name, enabled, [mode], [delay_ms], [pixel_step], [end_hold_ms], [start_hold_ms])`
Configures text scrolling for a zone.
* `enabled` (bool): `True` to enable scrolling, `False` to disable.
* `mode` (str, optional): `"pixel"` / `"p"` (smooth scrolling, default), `"loop"` / `"l"` (loops text without restart gap), or `"character"` / `"c"` (jump scroll character by character).
* `delay_ms` (int, optional): Delay in milliseconds between scroll updates (default `200`).
* `pixel_step` (int, optional): Scroll distance in pixels per tick (default `1`).
* `end_hold_ms` (int, optional): Time to pause at the end of the text before restarting (default `800`).
* `start_hold_ms` (int, optional): Time to pause at the start of the text before starting to scroll (default `500`).

#### `.set_scroll_direction(zone_name, direction)`
* `direction` (str): `"up"` / `"u"`, `"down"` / `"d"`, `"left"` / `"l"`, or `"right"` / `"r"`.

#### `.set_scroll_speed(zone_name, delay_ms)`
Updates the scroll timer interval.

#### `.set_scroll_pixel_step(zone_name, step)`
Updates the scroll pixel step.

#### `.set_scroll_mode(zone_name, mode)`
Updates the scroll mode (`"pixel"`, `"loop"`, or `"character"`).

#### `.get_scroll_position(zone_name)`
Returns the current scroll position offset (in characters/pixels depending on mode).

#### `.reset_scroll(zone_name)`
Resets the scrolling parameters back to the start index.

#### `.zone_scroll_done(zone_name)`
Returns `True` if the scrolling text has reached its end (and completed its hold duration), or if scrolling is disabled.

---

### Character Animations

Smooth transitions are calculated on character updates.

#### `.set_animation(anim_type, duration_ms, stagger_ms)`
Sets default global transitions.
* `anim_type` (str): `"fade"` / `"f"`, `"fade_through"` / `"ft"`, `"slide_up"` / `"su"`, `"slide_down"` / `"sd"`, `"slide_left"` / `"sl"`, `"slide_right"` / `"sr"`, or `"none"` / `"n"`.
* `duration_ms` (int): Duration of the character transition in milliseconds (default `120`).
* `stagger_ms` (int): Sequential staggering delay applied character-by-character (default `15`).

#### `.set_zone_presentation(zone_name, presentation, animation, duration_ms, stagger_ms)`
Overwrites transition animations specifically for one zone.
* `presentation` (str): `"static"` / `"st"` or `"sequential"` / `"sq"`.
* `animation` (str): Zone-specific transition type.
* `duration_ms` (int): Transition duration.
* `stagger_ms` (int): Character stagger delay.

---

### Blinking Effects

#### `.enable_colon_blink(zone_name, [period_ms])`
Forces any colon (`:`) characters in the zone text to blink (extremely useful for separation markers in clocks).
* `period_ms` (int, optional): Blinking frequency interval in milliseconds (default `500`).

#### `.disable_colon_blink(zone_name)`
Disables colon blinking.

#### `.set_zone_blink_chars(zone_name, chars, [period_ms])`
Blinks any characters found in the `chars` string when rendered inside this zone.
* `chars` (str): Set of characters (max 8) that will blink.

#### `.set_zone_blink_positions(zone_name, positions, [period_ms])`
Blinks characters located at specific index offsets.
* `positions` (list of int): List of text indexes to blink.

#### `.set_zone_blink(zone_name, enabled, [period_ms])`
Toggles blinking for the entire zone layout.

---

### Color Management

Overrides can be assigned to customize color on specific letters/offsets.

#### `.set_char_color(position, r, g, b)`
Sets the color of a specific character position index.
* `position` (int): The 0-based character index.
* `r`, `g`, `b` (int): RGB values.

#### `.set_section_color(start, end, r, g, b)`
Assigns a color to a range of character indexes.
* `start` (int), `end` (int): Boundary character offsets.
* `r`, `g`, `b` (int): RGB values.

#### `.clear_all_colors()`
Removes all character and section color overrides.

---

### Font Engine

The default system font is named `"4x7"`. Up to 8 fonts can be loaded dynamically.

#### `.load_external_font(name, font_dict)`
Loads a font from a MicroPython dictionary.
* `name` (str): Font registry key name.
* `font_dict` (dict): Dictionary representing the font. Keys must include:
  * `"width"` or `"Width"` (int): Character width in pixels.
  * `"height"` or `"Height"` (int): Character height in pixels.
  * `"start"` or `"Start"` (int): Starting ASCII value code.
  * `"end"` or `"End"` (int): Ending ASCII value code.
  * `"spacing"` or `"Spacing"` (int, optional): Horizontal spacing default.
  * `"data"` or `"Data"` (list of ints / bytes / bytearray): Raw font bitmaps (stored in column-major order).

#### `.set_font(name)`
Switches the active font. Returns `True` if successfully switched, otherwise `False`.
* `name` (str): Registered font key (e.g. `"4x7"`).

#### `.calculate_height(char_count)`
Calculates the vertical height in pixels required to render a specific number of characters in the active font.

---

### Advanced Copy & Alpha Settings

#### `.set_copy_black_pixels(enabled)`
Determines how black pixels `(0, 0, 0)` are copied from the framebuffer to the physical LED buffer.
* `enabled` (bool): If `True` (default), black pixels are copied over. If `False`, they are treated as transparent, preserving the existing pixels in the underlying buffer.

#### `.set_led_clear_before_copy(enabled)`
Clears the physical LED buffer (sets all to 0) immediately before copying the framebuffer contents.

#### `.set_copy_alpha(alpha)`
Sets a global alpha value for buffer copy operations.
* `alpha` (int): Alpha value `[0 - 255]`. Values `< 255` perform alpha blending over the current LED strip buffer state.

#### `.set_zone_alpha(zone_name, alpha)`
Applies alpha blending factor specifically to the layout boundary of a zone.

---

### Object Proxies & Attributes

`LEDDisplaySystem` exposes properties that yield sub-interface wrappers:

#### `Display.zones`
Returns a list of proxy `LEDZone` objects representing the active zones.
* **Properties on `LEDZone`:**
  * `zone.name` (str, read-only): The zone name identifier.
  * `zone.zone_enabled` (bool, read-write): Enables/disables the zone.
  * `zone.sequence_index` (int, read-write): The current active message index in sequenced mode.

#### `Display.color_manager`
Returns a color manager proxy wrapper.
* **Methods on `ColorManager`:**
  * `color_manager.set_char_color(pos, r, g, b)`
  * `color_manager.clear_all_colors()`

#### `Display.rows` & `Display.cols`
Read-only integer values representing display dimensions.

---

## 💻 MicroPython Examples

### 1. Simple Multi-Zone Clock & Scroll Example

Here is a complete, well-commented example demonstrating how to initialize a display system, configure a time zone and a scrolling text zone, and run a loop.

To maximize performance, this example utilizes the `@micropython.native` decorator for the update call.

```python
import time
import leddisplay

# A mock LED device acting as the frame target.
# Replace this with your actual LED driver/buffer class.
class MockLEDDevice:
    def __init__(self, num_leds):
        self.led_buffer = bytearray(num_leds * 3)
        self.aled_object = list(range(num_leds))
        
    def __size__(self):
         return len(self.led_buffer)

# A simple Controller object tracking time.
# The display system expects hour, minutes, and seconds properties.
class TimeController:
    def __init__(self):
        self.hour = 12
        self.minutes = 34
        self.seconds = 56

# Initialize mock hardware
led_strip = MockLEDDevice(150) # 150 LEDs total

# Initialize Display: 50 rows, 3 columns
display = leddisplay.LEDDisplaySystem(led_strip, rows=50, cols=3, render_direction=0, spacing=1)

# Configure layout zones:
# 1. Zone "time" (rows 0 to 24)
# 2. Zone "scroll" (rows 25 to 49)
display.set_zones([
    ("time", 0, 24),
    ("scroll", 25, 49)
])

# Configure the "time" zone type & enable colon blinking (500ms period)
display.set_zone_type("time", "time")
display.enable_colon_blink("time", 500)
display.set_zone_colors("time", fg=(255, 0, 0), bg=(0, 0, 0)) # Red text

# Configure "scroll" zone messages & enable scrolling
display.set_zone_messages("scroll", ["ESP32-S3!", "MICROPYTHON", "C MODULE"])
display.set_zone_colors("scroll", fg=(0, 255, 0), bg=(0, 0, 0)) # Green text
display.set_zone_scroll("scroll", enabled=True, mode="pixel", delay_ms=80, pixel_step=1)

# Initialize time controller
controller = TimeController()

# Use .native decorator to optimize performance of the refresh loop
@micropython.native
def refresh_display(disp, ctrl):
    # Process scrolling, blinking, animations, and paint
    disp.update(ctrl)

print("Starting display system loop. Press Ctrl+C to exit.")

try:
    last_tick = time.ticks_ms()
    while True:
        # Update clock seconds
        t_now = time.localtime()
        controller.hour = t_now[3]
        controller.minutes = t_now[4]
        controller.seconds = t_now[5]
        
        # Refresh display
        refresh_display(display, controller)
        
        # Run at ~30 FPS
        time.sleep_ms(33)
        
except KeyboardInterrupt:
    print("Exited.")
```

---

### 2. Custom Font Loading

This example shows how to format and load a custom dictionary-based font.

```python
import leddisplay

# Mock LED strip setup
class MockStrip:
    def __init__(self):
        self.aled_object = list(range(200))
        self.led_buffer = bytearray(600)

strip = MockStrip()
display = leddisplay.LEDDisplaySystem(strip, rows=64, cols=3)

# Define a custom 3x5 font dictionary
# data: 3 columns wide, column-major bit patterns (1 = pixel set, 0 = unset)
custom_font = {
    "width": 3,
    "height": 5,
    "start": 65, # Starts at ASCII 'A'
    "end": 67,   # Ends at ASCII 'C'
    "spacing": 1,
    "data": [
        # 'A' (3 columns)
        0b11110, # . X X X .
        0b00101, # X . . X . (MSB is bottom bit)
        0b11110,
        # 'B'
        0b11111,
        0b10101,
        0b01010,
        # 'C'
        0b01110,
        0b10001,
        0b10001
    ]
}

# Load the font to the registry
display.load_external_font("mini_3x5", custom_font)

# Switch to the custom font
success = display.set_font("mini_3x5")
if success:
    print("Font successfully loaded and applied!")
else:
    print("Failed to apply font.")
```
