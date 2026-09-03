# USB PWM Fan Controller (Arduino Nano R4 & Native Linux hwmon Driver)

A high-performance, native Linux hardware monitoring (`hwmon`) solution for controlling and monitoring PC fans. 

This project uses an **Arduino Nano R4** to manage two **Noctua 80mm NF-A8 5V PWM** fans independently. It includes a custom **Linux USB kernel driver** that registers directly into the `hwmon` subsystem, allowing seamless control via standard utilities like `fancontrol` and `coolercontrol`.

---

## Features
- **Native `hwmon` Integration:** No background userspace daemons required. Exposes standard sysfs attributes (`fan1_input`, `fan2_input`, `pwm1`, `pwm2`, `pwm1_enable`, `pwm2_enable`).
- **Standard PC Fan PWM (25 kHz):** Implemented directly on the Renesas RA4M1 General PWM Timers (GPT). No high-pitched acoustic coil whine.
- **Accurate Tachometer Tracking:** Hardware interrupts (falling-edge triggered) monitor actual RPM without blocking the Arduino execution loop.
- **Fail-Safe Startup:** On system power-up, both channels default to **100% speed (255 PWM)** to protect connected equipment until the driver takes active control.
- **Thread-Safe Caching:** Features a 1-second (1000ms) cache protected by a kernel mutex to prevent USB communication bottlenecks under multi-client polling (e.g. concurrent tool queries).

---

## 1. Hardware Architecture & Wiring

The Arduino Nano R4 runs on **5V logic**, which perfectly matches Noctua's 5V PWM fan specifications. No level shifters are required!

### Fan Specifications (Noctua NF-A8 5V PWM)
- **Pin 1 (GND):** Black wire
- **Pin 2 (+5V VCC):** Yellow wire
- **Pin 3 (Tacho/Speed):** Green wire (open-drain, active low, 2 pulses per revolution)
- **Pin 4 (PWM Control):** Blue wire (accepts 25 kHz PWM)

### Wiring Connections

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

*Note: Pins D2 and D8 use completely independent hardware interrupt channels on the Renesas RA4M1 chip, preventing collisions.*

---

## 2. Arduino Firmware Setup

1. Open the Arduino IDE (or VS Code with Arduino extension).
2. Install the **Arduino UNO R4 Boards** core package.
3. Open `arduino/usb_pwm_controller/usb_pwm_controller.ino`.
4. Connect the Arduino Nano R4 using a USB-C cable.
5. Select **Arduino UNO R4 Minima** (or Arduino Nano R4 if listed) as the board.
6. Compile and upload the sketch to your board.

---

## 3. Linux Kernel Driver Setup

### Prerequisites
Ensure you have the Linux kernel headers installed for your running kernel.
- **Ubuntu/Debian:** `sudo apt install linux-headers-$(uname -r) build-essential`
- **Fedora/RHEL:** `sudo dnf install kernel-devel-$(uname -r) kernel-headers-$(uname -r) development-tools`
- **Arch Linux:** `sudo pacman -S linux-headers base-devel`

### Building the Driver
Navigate to the `driver` directory and compile the driver module:
```bash
cd driver
make
```

### Loading the Driver
Insert the compiled kernel module into the running kernel:
```bash
sudo insmod usb_pwm_hwmon.ko
```

Confirm that the driver has successfully bound to the Arduino Nano R4:
```bash
dmesg | grep "USB PWM"
```

You should see output similar to:
```
[  218.456123] usb 1-3: USB PWM: Claimed control interface 0
[  218.456890] usb 1-3: USB PWM: Discovered Bulk IN endpoint: 0x82 (size: 64)
[  218.456893] usb 1-3: USB PWM: Discovered Bulk OUT endpoint: 0x01 (size: 64)
[  218.457121] usb 1-3: USB PWM: HWMON device registered successfully
```

---

## 4. Verification & Testing

Once loaded, standard Linux sysfs hardware monitoring attributes are created.

### 1. Locate the HWMON Device
Search for the device in `/sys/class/hwmon/`:
```bash
for dev in /sys/class/hwmon/hwmon*; do
    if [ -f "$dev/name" ]; then
        name=$(cat "$dev/name")
        if [ "$name" = "usb_pwm_controller" ]; then
            echo "Found driver at: $dev"
            HWMON_DIR=$dev
        fi
    fi
done
```

### 2. Read Fan RPM Speeds
Read current RPM speeds (returns `0` if fans are disconnected or not spinning):
```bash
cat $HWMON_DIR/fan1_input
cat $HWMON_DIR/fan2_input
```

### 3. Read & Write PWM duty cycle (0-255)
- Read current PWM settings (defaults to `255`):
  ```bash
  cat $HWMON_DIR/pwm1
  cat $HWMON_DIR/pwm2
  ```
- Change Fan 1 speed to ~50% duty cycle:
  ```bash
  echo 128 | sudo tee $HWMON_DIR/pwm1
  ```
- Change Fan 2 speed to 0% duty cycle (stop fan):
  ```bash
  echo 0 | sudo tee $HWMON_DIR/pwm2
  ```

---

## 5. Tool Integration (`coolercontrol` & `fancontrol`)

Since the driver exposes standard `hwmon` nodes, it is natively supported by Linux fan control suites.

### CoolerControl
1. Ensure the `coolercontrold` daemon is running.
2. The custom USB controller will be automatically discovered and listed as `usb_pwm_controller`.
3. Open the **CoolerControl GUI** to map temperatures (CPU, GPU, etc.) to your custom fans with custom curves.

### Fancontrol / lm-sensors
1. Run `pwmconfig` to configure fan control rules:
   ```bash
   sudo pwmconfig
   ```
2. Follow the on-screen instructions. The utility will dynamically change the `pwm1`/`pwm2` values to verify that fans stop/spin, mapping them to temperature sensors.
3. Start the fancontrol daemon:
   ```bash
   sudo systemctl start fancontrol
   ```

---

## 6. Unloading & Safety

To release the USB device (allowing you to upload new firmware sketches via the Arduino IDE):
```bash
sudo rmmod usb_pwm_hwmon
```
Once unloaded, the standard `cdc_acm` driver is free to re-claim the virtual serial port (`/dev/ttyACM0`).
