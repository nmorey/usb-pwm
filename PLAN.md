# USB-Controllable PWM Fan Controller with Arduino Nano R4 & Linux hwmon Driver

This document details the design and implementation of a USB-controllable PWM fan controller using an Arduino Nano R4 to manage two Noctua 80mm NF-A8 5V PWM fans, integrated directly into the Linux `hwmon` subsystem.

---

## 1. Objective & Overview

To build a high-performance, native Linux solution that allows monitoring and controlling two independent PC fans via standard utilities like `fancontrol`, `coolercontrol`, or custom scripts.

The solution consists of:
1. **Arduino Nano R4 Firmware**: Generates 25 kHz PWM signals, measures tachometer RPM via interrupts, and implements a lightweight, robust request-response protocol over native USB bulk endpoints.
2. **Linux Kernel Driver**: A custom USB driver that binds to the Arduino Nano R4 (VID `0x2341`, PID `0x0063`). It registers a `hwmon` device under `/sys/class/hwmon/` exposing RPM inputs and PWM controls.
3. **Verification Suite**: Documentation and steps to build, load, and verify functionality using sysfs and standard tools.

---

## 2. Hardware Architecture & Wiring

The Arduino Nano R4 operates at 5V logic, which perfectly matches Noctua's 5V PWM fans, removing the need for external level shifters.

### Fan Specifications (Noctua NF-A8 5V PWM)
- **Pin 1 (GND):** Black
- **Pin 2 (+5V VCC):** Yellow
- **Pin 3 (Tacho/Speed):** Green (Outputs 2 pulses per revolution, open-drain)
- **Pin 4 (PWM Control):** Blue (Expects 25 kHz PWM signal)

### Wiring Diagram
```
  [Arduino Nano R4]                        [Noctua Fan 1]
    D2 (Interrupt 0) <---------------------- Pin 3 (Tacho, Green)
    D3 (PWM)         ----------------------> Pin 4 (PWM, Blue)
    5V               ----------------------> Pin 2 (VCC, Yellow)
    GND              <----------------------> Pin 1 (GND, Black)

                                           [Noctua Fan 2]
    D8 (Interrupt 9) <---------------------- Pin 3 (Tacho, Green)
    D9 (PWM)         ----------------------> Pin 4 (PWM, Blue)
    5V               ----------------------> Pin 2 (VCC, Yellow)
    GND              <----------------------> Pin 1 (GND, Black)
```

*Note: Since the Arduino Nano R4 D2 and D8 pins do not share any hardware interrupt channels, they will trigger completely independently without collision.*

---

## 3. Communication Protocol

To ensure perfect synchronization, we use a synchronous, half-duplex, request-response serial protocol over native USB bulk endpoints:

1. **Get Fan Speeds**
   - **Host sends:** `GET\n`
   - **Arduino replies:** `RPM1 <rpm1> RPM2 <rpm2>\n`
2. **Set Fan 1 PWM**
   - **Host sends:** `SET PWM1 <val>\n` (where `<val>` is 0-255)
   - **Arduino replies:** `OK\n` or `ERR\n`
3. **Set Fan 2 PWM**
   - **Host sends:** `SET PWM2 <val>\n` (where `<val>` is 0-255)
   - **Arduino replies:** `OK\n` or `ERR\n`

---

## 4. Phased Implementation Plan

### Phase 1: Arduino Firmware
We will implement the firmware under `arduino/usb_pwm_controller/usb_pwm_controller.ino`.
- **PWM Generation:** Use the RA4M1 core `PwmOut` class (`"pwm.h"`) on pins `D3` and `D9` to achieve a 25 kHz carrier frequency.
- **Tachometer Readings:** Use hardware interrupts (`attachInterrupt`) on `D2` and `D8` mapped to FALLING edges. The background `loop()` aggregates the pulses over elapsed time using `millis()` to compute precise RPM.
- **Serial Processing:** Parse incoming USB commands buffer-by-buffer, calling the state machine immediately upon matching `\n`.

### Phase 2: Linux Kernel Driver
We will implement the driver under `driver/usb_pwm_hwmon.c` with an associated `Makefile`.
- **USB Matching:** Bind to Vendor ID `0x2341` and Product ID `0x0063` (Arduino Nano R4).
- **Interface Selection:** Ignore the Control interface (Interface 0) to avoid processing overhead, and bind to the Data interface (Interface 1).
- **Endpoint Discovery:** Dynamically search for bulk IN and bulk OUT endpoints, ensuring portability across potential Arduino bootloader updates.
- **HWMON Registration:** Register with the hwmon subsystem as `"usb_pwm_controller"` using modern descriptors (`HWMON_CHANNEL_INFO` / `devm_hwmon_device_register_with_info`).
- **Caching Mechanism:** Cache RPM queries for 1 second (1000ms) to prevent excessive USB traffic when multiple sysfs entries are queried simultaneously. Protect communications with a `mutex`.

---

## 5. File Layout

The workspace will be organized as follows:
```
usb-pwm/
├── arduino/
│   └── usb_pwm_controller/
│       └── usb_pwm_controller.ino
├── driver/
│   ├── Makefile
│   └── usb_pwm_hwmon.c
└── README.md
```

---

## 6. Verification & Testing

1. **Compilation of Arduino sketch:** Upload and verify code structure compiles without issues.
2. **Compilation of Driver:**
   ```bash
   cd driver
   make
   ```
3. **Loading the Driver:**
   ```bash
   sudo insmod usb_pwm_hwmon.ko
   ```
4. **Verifying sysfs structure:**
   Check if the device is correctly loaded under `/sys/class/hwmon/`:
   ```bash
   ls -la /sys/class/hwmon/hwmon*/
   ```
5. **Testing Controls:**
   - Read fan speeds: `cat /sys/class/hwmon/hwmonX/fan1_input` (where `hwmonX` is the registered index).
   - Read and write PWM:
     ```bash
     cat /sys/class/hwmon/hwmonX/pwm1
     echo 128 | sudo tee /sys/class/hwmon/hwmonX/pwm1
     ```
6. **Integration Verification:** Check compatibility with `fancontrol` and `coolercontrol`.

---

## 7. Migration, Uninstallation, & Safety

- **Default State:** On startup, both PWM channels default to 100% duty cycle (255) for maximum hardware safety (ensures fans spin to protect the system in case driver is not loaded).
- **Unloading:**
  ```bash
  sudo rmmod usb_pwm_hwmon
  ```
  Once unloaded, the standard `cdc_acm` driver is free to claim the device again if the user needs to upload a new sketch.
