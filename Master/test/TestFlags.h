#pragma once

#define ENABLE_FSM_SIMULATION_TEST        0
#define ENABLE_RELAY_HARDWARE_TEST        0
#define ENABLE_FULL_SYSTEM_TEST           0
#define ENABLE_MQTT_CONFIGURATION_TEST    0

// Test irigasi langsung: skip FILL_WATER & mixing, langsung masuk state IRRIGATION.
// Gunakan ini untuk verifikasi pompa irigasi, flow sensor irigasi, dan durasi irigasi.
#define ENABLE_IRRIGATION_TEST            0

// Durasi irigasi saat ENABLE_IRRIGATION_TEST (ms).
// 0 = nyala terus sampai board di-reset / flag dimatikan.
// Contoh: 60000UL = 1 menit, 300000UL = 5 menit.
#define IRRIGATION_TEST_DURATION_MS       0UL

// Kalibrasi flow sensor dengan target volume.
// Set A=1 → test Pompa A + Solenoid A saja, relay lain mati.
// Set B=1 → test Pompa B + Solenoid B saja, relay lain mati.
// Target volume diatur di test/data/FlowCalibrationTestData.h
#define ENABLE_FLOW_CALIBRATION_TEST_A    0
#define ENABLE_FLOW_CALIBRATION_TEST_B    0

// Set PCF8563 RTC to firmware build time on boot.
// Keep this 1 while calibrating/testing RTC from a WIB build machine.
// Set to 0 after RTC time is correct and the backup battery is installed.
#define SYNC_RTC_FROM_BUILD_TIME          0

#define TEST_MODE_COUNT \
    (ENABLE_FSM_SIMULATION_TEST + \
     ENABLE_RELAY_HARDWARE_TEST + \
     ENABLE_FULL_SYSTEM_TEST + \
     ENABLE_MQTT_CONFIGURATION_TEST + \
     ENABLE_FLOW_CALIBRATION_TEST_A + \
     ENABLE_FLOW_CALIBRATION_TEST_B + \
     ENABLE_IRRIGATION_TEST)

#if TEST_MODE_COUNT > 1
#error "Only one test mode may be enabled at a time."
#endif

#define ENABLE_ANY_TEST_MODE (TEST_MODE_COUNT > 0)
