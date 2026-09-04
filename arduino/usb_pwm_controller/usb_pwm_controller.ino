/*
 * Arduino Nano R4 USB-Controllable PWM Fan Controller
 * 
 * Generates 25 kHz PWM signals to control two 5V Noctua PWM fans independently.
 * Measures RPM of both fans using hardware interrupts.
 * Exposes a lightweight request-response protocol over native USB-CDC Serial.
 * 
 * Pin mapping:
 * - Fan 1 PWM: D3 (RA4M1 GPT PWM)
 * - Fan 1 Tacho: D2 (RA4M1 External Interrupt, Dedicated Channel 0)
 */

#include "pwm.h"

// Hardware Pin Definitions
const int FAN1_PWM_PIN = D3;
const int FAN1_TACHO_PIN = D2;

// PWM Output Objects
PwmOut fan1_pwm(FAN1_PWM_PIN);

// Pulse counters for RPM calculations (marked volatile as they are modified in ISRs)
volatile unsigned long fan1_pulses = 0;

// Calculated RPM values
unsigned int fan1_rpm = 0;

// PWM cache values (0-255)
int pwm1_val = 96;

// Timing tracking
unsigned long last_rpm_update = 0;
const unsigned long RPM_UPDATE_INTERVAL_MS = 1000; // Update RPM every 1s

// Serial buffer variables
char cmd_buf[64];
int cmd_len = 0;

// Interrupt Service Routines (ISRs)
void fan1_isr() {
    fan1_pulses++;
}

// Convert 0-255 PWM duty cycle to 0-100% float for the PwmOut class
float pwm_to_percent(int val) {
    if (val < 0) val = 0;
    if (val > 255) val = 255;
    return (val / 255.0f) * 100.0f;
}

void setup() {
    // 1. Initialize native USB CDC Serial
    Serial.begin(115200);

    // 2. Initialize PWM Outputs (25 kHz for PC Fans, default to 100% duty cycle for safety)
    fan1_pwm.begin(25000.0f, pwm_to_percent(pwm1_val));

    // 3. Initialize Tachometer inputs with internal Pull-up resistors
    pinMode(FAN1_TACHO_PIN, INPUT_PULLUP);

    // 4. Attach interrupts on FALLING edge (Noctua tacho pulls to GND twice per revolution)
    attachInterrupt(digitalPinToInterrupt(FAN1_TACHO_PIN), fan1_isr, FALLING);

    last_rpm_update = millis();
}

void loop() {
    unsigned long current_time = millis();

    // 1. Periodic RPM Calculation (Non-blocking)
    if (current_time - last_rpm_update >= RPM_UPDATE_INTERVAL_MS) {
        // Disable interrupts briefly to copy and reset counts atomically
        noInterrupts();
        unsigned long pulses1 = fan1_pulses;
        fan1_pulses = 0;
        interrupts();

        unsigned long elapsed_ms = current_time - last_rpm_update;

        // Noctua NF-A8 outputs 2 pulses per revolution.
        // RPM = (pulses / 2) * (60,000 / elapsed_ms)
        // Mathematically simplified: (pulses * 30,000) / elapsed_ms
        if (elapsed_ms > 0) {
            fan1_rpm = (pulses1 * 30000UL) / elapsed_ms;
        }

        last_rpm_update = current_time;
    }

    // 2. Process Incoming USB CDC Commands
    while (Serial.available() > 0) {
        char c = Serial.read();

        if (c == '\n' || c == '\r') {
            if (cmd_len > 0) {
                cmd_buf[cmd_len] = '\0';
                process_command(cmd_buf);
                cmd_len = 0;
            }
        } else if (cmd_len < sizeof(cmd_buf) - 1) {
            cmd_buf[cmd_len++] = c;
        }
    }
}

const char* set_cmd="SET PWM ";
// Command State Machine & Parsing
void process_command(char* cmd) {
    if (strcmp(cmd, "GET") == 0) {
        // Respond with current RPM speeds
        Serial.print(fan1_rpm);
    } 
    else if (strncmp(cmd, set_cmd, strlen(set_cmd)) == 0) {
	int val = atoi(cmd + strlen(set_cmd));
        if (val >= 0 && val <= 255) {
            pwm1_val = val;
            fan1_pwm.pulse_perc(pwm_to_percent(pwm1_val));
            Serial.println("OK");
        } else {
            Serial.println("ERR");
        }
    } 
    else {
        Serial.println("ERR_UNKNOWN_CMD");
    }
}
