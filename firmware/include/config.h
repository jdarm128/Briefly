#pragma once
// ---------------------------------------------------------------------------
// Briefing Station - hardware map & tunables
// Board: LILYGO TTGO T-Display (ESP32). Verify pins against your board's
// pinout card; these use commonly free GPIOs on the T-Display.
// ---------------------------------------------------------------------------

// I2C bus shared by OLED (0x3C), CAP1188 (0x29/0x28), LSM6DSO (0x6B/0x6A), DHT20 (0x38)
#define PIN_I2C_SDA      21
#define PIN_I2C_SCL      22

// Peripherals
#define PIN_BUZZER       15   // piezo buzzer (+) ; (-) to GND
#define PIN_LED          26   // status LED through ~220 ohm resistor
#define PIN_IMU_INT      32   // LSM6DSO INT1 -> knock detection
#define PIN_TFT_BL        4   // T-Display backlight (matches build flags)

// Onboard T-Display buttons (pressed = LOW). GPIO35 is input-only and has an
// external pull-up on the board; GPIO0 uses the internal pull-up.
#define PIN_BTN_A         0   // short press: request update / long press: dismiss alarm
#define PIN_BTN_B        35   // short press: next card  / long press: dismiss alarm

// Buzzer PWM
#define BUZZER_LEDC_CH    0

// Timekeeping (Pacific time incl. DST)
#define TZ_INFO          "PST8PDT,M3.2.0,M11.1.0"
#define NTP_1            "pool.ntp.org"
#define NTP_2            "time.nist.gov"

// MQTT topics
#define T_CARDS          "station/cards"      // gateway -> device (retained)
#define T_COMMAND        "station/command"    // gateway -> device
#define T_EVENT          "station/event"      // device -> gateway
#define T_TELEMETRY      "station/telemetry"  // device -> gateway

// Behavior tunables
#define MAX_CARDS         8
#define CARD_CHARS       21     // SSD1306 fits 21 chars per line at size 1
#define SNOOZE_SECONDS   300
#define KNOCK_COOLDOWN_MS 1500
#define BACKLIGHT_MS     120000UL   // dim after 2 min idle
#define TELEMETRY_MS     300000UL   // every 5 min
#define LONGPRESS_MS     1200
#define MAX_ALARMS        6
