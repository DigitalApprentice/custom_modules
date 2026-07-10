# fft_core1 — MicroPython Custom C Module for ESP32-S3

A high-performance, real-time audio FFT processor for **ESP32-S3** written as a MicroPython custom C module (compatible with **ESP-IDF v5.4** and **MicroPython v1.28.0**). 

It is designed to run asynchronously on **Core 1** to prevent interference with MicroPython execution or Wi-Fi operations on Core 0. It captures stereo I2S audio (specifically tested with the **INMP441** MEMS microphone), performs Hanning windowing and a Radix-2 FFT, groups magnitudes into configurable logarithmic bands, estimates BPM, and extracts complex spectral features.

All buffer data (`magnitudes`, `bands`, `features`) are exposed directly to MicroPython as zero-allocation buffers (`memoryview` compatible), ensuring absolute minimum memory churn.

---

## 🧠 Core Assignment (Core 1 vs Core 0)

By default, the I2S polling and FFT calculation task is pinned to **Core 1**. If you wish to migrate the task to **Core 0**, you need to modify the following lines in [fft_core1.c](file:///h:/Mój dysk/CLeds/CLeds_leds/custom_modules/fft_core1/fft_core1.c):

1. **Task Spawning (Line 680):**
   Locate `xTaskCreatePinnedToCore` in the `py_start` function:
   ```c
   // Before (Core 1):
   xTaskCreatePinnedToCore(fft_core1_task, "fft_c1", 8192, NULL, 5, &fft_task_h, 1);

   // After (Core 0):
   xTaskCreatePinnedToCore(fft_core1_task, "fft_c0", 8192, NULL, 5, &fft_task_h, 0);
   ```

2. **Diagnostics Info (Line 781):**
   Locate the dictionary store call in the `py_info` function:
   ```c
   // Before (Core 1):
   mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_core),            mp_obj_new_int(1));

   // After (Core 0):
   mp_obj_dict_store(d, MP_OBJ_NEW_QSTR(MP_QSTR_core),            mp_obj_new_int(0));
   ```

---

## 🔌 API Reference

### `fft_core1.start(sck, ws, sd, [sample_rate])`
Initializes and starts the I2S microphone peripheral and spawns the background FFT processing task.
- `sck` (int): BCLK (Bit Clock) pin.
- `ws` (int): WS (Word Select / LRCK) pin.
- `sd` (int): DIN (Data input) pin.
- `sample_rate` (int, optional): Audio sampling rate. Defaults to `44100` Hz.

### `fft_core1.stop()`
Stops the background task, terminates and deletes the I2S driver channel, and frees allocated mutexes.

### `fft_core1.configure(fft_size, [bands], [peak_decay], [global_max_dec], [beat_threshold], [global_max_floor], [noise_gate])`
Configures runtime parameters under mutex protection:
- `fft_size` (int): FFT size. Must be a power of 2 between `64` and `1024` (default `256`).
- `bands` (int, optional): Number of logarithmic bands to group frequencies into, from `1` to `24` (default `12`).
- `peak_decay` (float, optional): Per-bin peak decay factor `[0.0 - 1.0]` (default `0.92`).
- `global_max_dec` (float, optional): AGC decay rate for the global maximum `[0.0 - 1.0]` (default `0.99`).
- `beat_threshold` (float, optional): Spectral flux threshold for beat detection (default `0.25`).
- `global_max_floor` (float, optional): Minimum global maximum divisor for Auto Gain Control (default `0.001`).
- `noise_gate` (float, optional): Minimum magnitude threshold below which bins are cleared (default `0.001`).

### `fft_core1.ready()`
Returns `True` if new FFT results are available to read, `False` otherwise.

### `fft_core1.clear_ready()`
Clears the `ready` flag. Call this after processing the frame.

### `fft_core1.magnitudes()`
Returns a zero-allocation read-only buffer object (`MagBuf`, typecode `'B'`) wrapping the linear FFT magnitude bins.
- Length: `fft_size / 2` bytes.
- Values: `0–255`.

### `fft_core1.bands()`
Returns a zero-allocation read-only buffer object (`BandBuf`, typecode `'B'`) wrapping the grouped logarithmic bands.
- Length: `num_bands` bytes.
- Values: `0–255`.

### `fft_core1.features()`
Returns a zero-allocation read-only buffer object (`FeatBuf`, typecode `'B'`) wrapping the raw 56-byte `fft_features_t` structure. See layout details below.

### `fft_core1.info()`
Returns a dictionary containing runtime state and diagnostics parameters:
- `fft_size`: Current FFT window size.
- `bands`: Current number of output bands.
- `bins`: `fft_size / 2`.
- `bpm`: Estimated BPM.
- `beat`: Boolean representing current beat state.
- `energy`: RMS signal energy.
- `global_max`: Divisor used for Auto Gain Control (AGC).
- `core`: Assigned core number (returns `1`).
- `max_mag_raw`, `bins_above_gate`, `max_sample`, `noise_gate`: Internal diagnostic values.

---

## 📊 Features Buffer Structure (`fft_features_t`)

The `fft_core1.features()` buffer is a 56-byte raw struct containing spectral features and beat markers. 
The module exposes pre-defined offset constants (`fft_core1.OFF_...`) to easily extract values using `struct.unpack_from`.

### Struct Layout

| Offset | Constant | Type | Range | Description |
| :--- | :--- | :--- | :--- | :--- |
| **0** | `OFF_ENERGY` | `float` | `0.0` to `1.0` | Root Mean Square (RMS) energy of the full frame. |
| **4** | `OFF_BASS` | `float` | `0.0` to `1.0` | Normalised energy in bass frequencies (20Hz - 300Hz). |
| **8** | `OFF_MID` | `float` | `0.0` to `1.0` | Normalised energy in mid frequencies (300Hz - 4kHz). |
| **12** | `OFF_TREBLE` | `float` | `0.0` to `1.0` | Normalised energy in treble frequencies (4kHz - 20kHz). |
| **16** | `OFF_PRESENCE` | `float` | `0.0` to `1.0` | Presence band energy (2kHz - 6kHz). |
| **20** | *n/a* | `float` | `0.0` to `1.0` | Brilliance band energy (6kHz - 20kHz). |
| **24** | `OFF_CENTROID` | `float` | `0.0` to `1.0` | Spectral centroid (spectral "center of mass"). |
| **28** | `OFF_FLUX` | `float` | `0.0` to `1.0` | Spectral flux (rate of change of the spectrum). |
| **32** | *n/a* | `float` | `0.0` to `1.0` | Rolloff frequency (85% energy threshold). |
| **36** | *n/a* | `float` | `0.0` to `1.0` | Spectral spread (standard deviation of the spectrum). |
| **40** | *n/a* | `float` | `0.0` to `1.0` | Zero-Crossing Rate (normalised). |
| **44** | `OFF_BEAT` | `uint8` | `0` or `1` | `1` if a beat has been detected in this frame. |
| **45** | `OFF_BEAT_STR` | `uint8` | `0` to `255` | Intensity of the detected beat. |
| **46** | `OFF_BPM` | `uint8` | `0` to `255` | Estimated BPM (returns `0` if not enough beats accumulated). |
| **47** | *n/a* | `uint8` | — | Struct padding. |
| **48** | `OFF_BASS_L` | `uint8` | `0` to `255` | Bass level normalised and ready for LED visuals (uses AGC). |
| **49** | `OFF_MID_L` | `uint8` | `0` to `255` | Mid level normalised for LED visuals (uses AGC). |
| **50** | `OFF_TREBLE_L` | `uint8` | `0` to `255` | Treble level normalised for LED visuals (uses AGC). |
| **51** | `OFF_ENERGY_L` | `uint8` | `0` to `255` | RMS energy level normalised for LED visuals (uses AGC). |
| **52** | `OFF_PRESENCE_L`| `uint8` | `0` to `255` | Presence level normalised for LED visuals (uses AGC). |
| **53** | `OFF_CENTROID_L`| `uint8` | `0` to `255` | Centroid level normalised for LED visuals. |
| **54** | `OFF_FLUX_L` | `uint8` | `0` to `255` | Flux level normalised for LED visuals. |
| **55** | *n/a* | `uint8` | — | Struct padding. |

---

## 🔍 Extracting Individual Features

To read a single feature (e.g., `treble`, which is a 4-byte `float` at offsets 12–15) from the `features()` bytearray without unpacking the entire 56-byte structure, you can use Python's `struct.unpack_from` function.

You can specify the location using either the hardcoded byte offset (`12`) or the module's exposed offset constants (e.g., `fft_core1.OFF_TREBLE`):

```python
import struct
import fft_core1

features_buf = fft_core1.features()

# Wait for a new frame
if fft_core1.ready():
    # 1. Unpack using the exposed offset constant (Recommended)
    treble_val = struct.unpack_from('<f', features_buf, fft_core1.OFF_TREBLE)[0]
    
    # 2. Or unpack using the direct numerical offset (12)
    treble_val_alt = struct.unpack_from('<f', features_buf, 12)[0]
    
    print(f"Treble value (0.0-1.0): {treble_val:.4f}")
```

### Unpacking Format Helper:
* **Floats** (`'<f'`, 4 bytes): Use for `energy`, `bass`, `mid`, `treble`, `presence`, `centroid`, `flux`.
* **Unsigned Bytes** (`'<B'`, 1 byte): Use for `beat`, `beat_strength`, `bpm_est`, and any of the `_level` outputs (e.g., `OFF_BASS_L`).

---

## 💻 MicroPython Example

Here is a complete, well-commented example showing how to initialize, configure, read frequency bands, extract spectral features, and print beat diagnostics.

```python
import time
import struct
import fft_core1

# 1. Start the audio processing thread (Core 1)
# SCK (BCLK) = Pin 4, WS (LRCK) = Pin 5, SD (DIN) = Pin 6
print("Starting FFT module...")
fft_core1.start(4, 5, 6, 44100)

# 2. Configure runtime parameters
# FFT size: 256 samples, Bands: 12 bands
# Other parameters left at their optimal defaults
fft_core1.configure(256, 12)

# 3. Create references to zero-allocation buffers
# Calling these once binds to the memory directly — no allocation inside the loop!
mags_buf = fft_core1.magnitudes()  # typecode 'B', length 128
bands_buf = fft_core1.bands()      # typecode 'B', length 12
features_buf = fft_core1.features() # typecode 'B', length 56

print("Ready. Processing audio...")

try:
    while True:
        # Check if a new frame has been processed by Core 1
        if fft_core1.ready():
            # A. Access the bands buffer (0-255)
            # You can loop or map values directly
            bass_band = bands_buf[0]
            mid_band = bands_buf[5]
            treble_band = bands_buf[11]
            
            # B. Read features using struct.unpack
            # Unpack format:
            # - 6 floats (energy, bass, mid, treble, presence, brilliance) -> '6f'
            # - 5 floats (centroid, flux, rolloff, spread, zcr) -> '5f'
            # - 4 uint8s (beat, beat_strength, bpm_est, pad) -> '4B'
            # - 8 uint8s (bass_lvl, mid_lvl, treble_lvl, energy_lvl, presence_lvl, centroid_lvl, flux_lvl, pad2) -> '8B'
            unpacked = struct.unpack('<6f5f4B8B', features_buf)
            
            energy = unpacked[0]
            bass = unpacked[1]
            mid = unpacked[2]
            
            beat = unpacked[11]           # 1 if beat, 0 otherwise
            beat_strength = unpacked[12]
            bpm_est = unpacked[13]
            
            # Extracted LED levels (normalised via AGC to 0-255 range)
            energy_lvl = unpacked[18]
            
            # Display beat status and BPM estimation
            if beat:
                print(f"💥 BEAT! Strength: {beat_strength} | BPM: {bpm_est} | RMS Energy Level: {energy_lvl}")
            
            # C. Alternately use exposed offsets directly via struct.unpack_from
            # e.g., to read only the BPM (uint8, offset 46)
            bpm_val = struct.unpack_from('<B', features_buf, fft_core1.OFF_BPM)[0]
            
            # Clear the ready flag to await the next audio frame
            fft_core1.clear_ready()
            
        # Tiny delay to prevent polling block
        time.sleep_ms(2)

except KeyboardInterrupt:
    print("Stopping module...")
finally:
    # Always stop the module to release I2S resources & stop the core task
    fft_core1.stop()
    print("Stopped.")
```
