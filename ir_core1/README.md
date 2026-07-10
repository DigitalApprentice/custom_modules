# ir_core1 — MicroPython Custom C Module for ESP32-S3

A high-performance, non-blocking Infrared (IR) receiver module for the **ESP32-S3** written as a MicroPython custom C module (compatible with **ESP-IDF v5.x** and **MicroPython v1.28.0**).

It utilizes the ESP32-S3's hardware **RMT (Remote Control)** peripheral to capture and decode IR signals in the background. By default, the background decode task runs on **Core 1** to avoid interfering with timing-critical LED drivers, audio processing, or Wi-Fi operations running on Core 0.

---

## 🧠 Core Assignment (Core 1 vs Core 0)

By default, the FreeRTOS background task responsible for waiting for RMT events and decoding incoming IR frames is pinned to **Core 1**.

If you want to run this task on **Core 0** instead (for instance, if Core 1 is heavily loaded with other tasks like FFT or fast LED strips), you can modify the core pinning argument in [ir_core1.c](file:///h:/Mój dysk/CLeds/CLeds_leds/custom_modules/ir_core1/ir_core1.c):

1. **Locate Task Spawning (Line 251):**
   Look for the `xTaskCreatePinnedToCore` call inside the `ir_start` function:
   ```c
   // Line 251:
   xTaskCreatePinnedToCore(ir_core1_task, "ir_c1", 4096, NULL, 5, &ir_task_h, 1);
   ```

2. **Modify the Core ID Parameter:**
   Change the last argument (the `xCoreID` parameter) from `1` to `0`:
   ```c
   // To run on Core 0:
   xTaskCreatePinnedToCore(ir_core1_task, "ir_c1", 4096, NULL, 5, &ir_task_h, 0);
   ```

---

## 🔌 API Reference

### `ir_core1.start(pin)`
Initializes the RMT RX peripheral on the specified GPIO pin, registers the completion callbacks, and spawns the background decoding task.
- `pin` (int): GPIO pin connected to the IR receiver (e.g., TSOP38238 or equivalent).

### `ir_core1.stop()`
Safely shuts down the module. It signals the background task to terminate, deletes the RMT RX channel, releases allocated FreeRTOS queues/tasks, and resets internal state variables.

### `ir_core1.available()`
Returns `True` if a new IR command has been successfully decoded and is waiting to be read, `False` otherwise.

### `ir_core1.read()`
Reads the latest decoded IR command from the buffer and clears the availability flag.
- **Returns**: A `dict` containing the decoded data, or `None` if no new frame is available.
- **Returned Dictionary Layout**:
  - `raw` (int): The complete 32-bit raw value decoded from the carrier wave.
  - `protocol` (str): Either `"NEC"` or `"LED"` depending on the decoded signature.
  - `long_press` (bool): `True` if the button is being held down (generating repeat codes/patterns), `False` if it was a single/initial press.
  - `address` (int, **NEC only**): The 8-bit device address.
  - `command` (int, **NEC only**): The 8-bit button command code.

### `ir_core1.set_long_press_ms(ms)`
Sets the threshold duration in milliseconds for detecting a long press.
- `ms` (int): Duration threshold (default is `600` ms). If a repeat signal or repeated pattern continues past this time, `long_press` is marked as `True`.

---

## 🛠️ Supported Protocols & Decoding

The module has hardware-timed decoders for two common remote control protocols:
1. **NEC Protocol**:
   - Expects a header consisting of a ~9000µs mark and a ~4500µs space, followed by 32 data bits.
   - Automatically validates command integrity by verifying that the address and command bytes match their bitwise complements (`address ^ address_inv == 0xFF` and `command ^ cmd_inv == 0xFF`).
   - Supports standard NEC repeat codes (~9000µs mark, ~2250µs space) to detect when a key is held.
2. **LED Protocol** (Custom/Typical cheap RGB LED controllers):
   - Expects a header with a ~9000µs mark and a ~1500–5000µs space, followed by 32 data bits.
   - Detects repeated codes within a 600ms window to determine holding/repeat actions.

---

## 🐛 ESP32-S3 RMT Workaround (Bug #17811)

On ESP32-S3 using ESP-IDF v5.3.x/v5.4.x, there is a known issue where the RMT RX channel automatically disables itself or drops its active state after receiving a frame (see Espressif issue #17811). 
This module incorporates an automatic workaround: prior to calling `rmt_receive()`, it explicitly cycles the channel state via `rmt_disable()` followed by `rmt_enable()` to guarantee that the hardware is armed and ready for the next incoming IR burst.

---

## 💻 MicroPython Example

Below is a complete, production-ready MicroPython example demonstrating how to initialize the module, poll for data, inspect decoded fields, handle repeat keys, and cleanly exit.

```python
import time
import ir_core1

# GPIO Pin connected to the OUT pin of your IR receiver (e.g. GPIO 14)
IR_PIN_NUM = 14

print("Initializing IR Receiver on Core 1...")
# 1. Start the IR task and RMT receiver
ir_core1.start(IR_PIN_NUM)

# 2. Optionally configure the long-press threshold (default is 600ms)
ir_core1.set_long_press_ms(500)

print("IR Receiver is active. Press buttons on your remote!")

try:
    while True:
        # Check if a new IR frame has been decoded
        if ir_core1.available():
            data = ir_core1.read()
            
            if data is not None:
                raw_code = data['raw']
                protocol = data['protocol']
                is_long = data['long_press']
                
                # Check for NEC specific details
                if protocol == "NEC":
                    addr = data['address']
                    cmd = data['command']
                    
                    press_type = "HELD/REPEAT" if is_long else "SINGLE PRESS"
                    print(f"[{protocol}] {press_type} -> Addr: 0x{addr:02X}, Cmd: 0x{cmd:02X} (Raw: 0x{raw_code:08X})")
                    
                    # Example command handling
                    if cmd == 0x1F:
                        print(" -> Action: Volume Up")
                    elif cmd == 0x1E:
                        print(" -> Action: Volume Down")
                        
                elif protocol == "LED":
                    press_type = "HELD/REPEAT" if is_long else "SINGLE PRESS"
                    print(f"[{protocol}] {press_type} -> Raw code: 0x{raw_code:08X}")
            
        # Small delay to keep the MicroPython REPL responsive
        time.sleep_ms(10)

except KeyboardInterrupt:
    print("\nInterrupted by user. Exiting...")
finally:
    # 3. Clean up RMT resources and stop the core task
    print("Stopping IR Receiver...")
    ir_core1.stop()
    print("Stopped.")
```
