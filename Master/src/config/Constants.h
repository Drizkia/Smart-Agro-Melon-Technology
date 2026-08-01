#ifndef CONSTANTS_H
#define CONSTANTS_H

#include <stdint.h>
#include "TestFlags.h"

// Dosis per injeksi koreksi PPM nutrisi A/B.
constexpr float CORRECTION_DOSE = 0.05f; // 50mL

// Batas atas pembacaan sensor TDS secara fisik (~900 ppm).
// Di atas ini sensor clip — sistem beralih ke mode estimasi.
constexpr float TDS_SENSOR_CLIP_PPM     = 900.0f;

// Kenaikan PPM per 1 mL gabungan A+B per 1 liter air (data field test).
// Base 490 ppm + 1 mL A + 1 mL B / 1 L → +330 ppm.
constexpr float PPM_PER_ML_PER_LITER_AB = 330.0f;

// Durasi mixing produksi.
constexpr uint32_t PRE_MIX_TANK_TIME = 60000UL; // 1 menit (sama di semua mode)

#if ENABLE_FULL_SYSTEM_TEST
// Fast test: semua timer mixing dikompres ke 1 menit.
// Timeout & config fisik (toren 640L) tetap sama agar pengisian air nyata.
constexpr uint32_t     PRE_MIX_CORRECTION_TIME = 60000UL;   // 1 menit
constexpr uint32_t     MIX_A_TIME              = 60000UL;   // 1 menit
constexpr uint32_t     MIX_B_TIME              = 60000UL;   // 1 menit
constexpr unsigned long CORRECTION_MIX_TIME    = 60000UL;   // 1 menit
constexpr uint32_t     CORRECTION_DELAY        = 60000UL;   // 1 menit
#else
constexpr uint32_t     PRE_MIX_CORRECTION_TIME = 300000UL;  // 5 menit
constexpr uint32_t     MIX_A_TIME              = 900000UL;  // 15 menit
constexpr uint32_t     MIX_B_TIME              = 900000UL;  // 15 menit
constexpr unsigned long CORRECTION_MIX_TIME    = 900000UL;  // 15 menit
constexpr uint32_t     CORRECTION_DELAY        = 180000UL;  // 3 menit
#endif

constexpr unsigned long WATER_FILL_TIMEOUT  = 7200000UL; // 2 jam
constexpr unsigned long NUTRIENT_TIMEOUT    = 900000UL;  // 15 menit

// Auto-recovery: setelah WATER_TIMEOUT, tunggu delay ini lalu retry FILL_WATER
// otomatis (tanpa perlu reset fisik). Jika tetap gagal, state ERROR persist
// hingga remote reset diterima via MQTT.
constexpr unsigned long WATER_TIMEOUT_AUTO_RECOVER_MS = 1800000UL; // 30 menit

// Durasi pulsing per channel — hasil kalibrasi stabil 0.5L (FlowCalibrationTestData.h).
// A lebih lemah, butuh ON lebih lama agar air naik ke sensor.
constexpr unsigned long NUTRIENT_A_PULSE_ON_MS  = 3125UL;  // 3.125 detik ON
constexpr unsigned long NUTRIENT_A_PULSE_OFF_MS =  100UL;  // 100ms OFF
constexpr unsigned long NUTRIENT_B_PULSE_ON_MS  =  305UL;  // 305ms ON
constexpr unsigned long NUTRIENT_B_PULSE_OFF_MS = 1400UL;  // 1.4 detik OFF

// Waktu solenoid tetap terbuka setelah pompa mati (drain air di selang kembali ke toren).
constexpr unsigned long NUTRIENT_DRAIN_DELAY_MS = 60000UL; // 1 menit
constexpr uint32_t PRE_IRRIGATION_MIX_TIME = 60000UL; // 1 menit

// FILL_WATER guards.
constexpr float          WATER_LEVEL_NOISE_THRESHOLD  = 0.1f;     // liter
constexpr unsigned long  WATER_LEVEL_STABLE_TIMEOUT   = 3000UL;   // 3 detik
constexpr float          TANK_SAFETY_FLOOR_LITER       = 5.0f;
constexpr unsigned long  NEED_REFILL_ALERT_INTERVAL_MS = 1000UL;  // 1 detik

// Soil health monitor.
constexpr unsigned long SOIL_HEALTH_EVAL_INTERVAL    = 300000UL;  // 5 menit
constexpr unsigned long SOIL_HEARTBEAT_TIMEOUT       = 300000UL;  // 5 menit
constexpr uint16_t      SOIL_ADC_MIN_VALID           = 800;       // batas bawah ADC wajar
constexpr uint16_t      SOIL_ADC_MAX_VALID           = 4095;      // batas atas ADC wajar
constexpr uint16_t      SOIL_FLATLINE_DELTA          = 50;        // delta ADC
constexpr uint8_t       SOIL_ADC_BUFFER_SIZE          = 8;
constexpr uint8_t       SOIL_MIN_CYCLES_FOR_FLATLINE  = 2;
constexpr uint16_t      SOIL_RESPONSE_MIN_DELTA      = 100;
constexpr uint8_t       SOIL_HEALTH_SWITCH_THRESHOLD  = 50;

#endif
