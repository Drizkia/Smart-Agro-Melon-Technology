#ifndef SOIL_HEALTH_MONITOR_H
#define SOIL_HEALTH_MONITOR_H

#include <Arduino.h>
#include <Preferences.h>

#include "../communication/ESPNowManager.h"
#include "../config/ConfigManager.h"
#include "../config/Constants.h"
#include "../config/SystemConfig.h"

#if IRRIGATION_MODE_SOURCE != 0 && IRRIGATION_MODE_SOURCE != 1
#error "IRRIGATION_MODE_SOURCE must be 0 (ESP-NOW soil sensor) or 1 (timer schedule)"
#endif

// =========================================
// Mode irigasi yang aktif saat ini
// =========================================
enum class IrrigationMode : uint8_t {
    HUMIDITY = 0,   // Mode normal: pakai dryThreshold/wetThreshold
    TIMER    = 1    // Mode fallback: irigasi berbasis jadwal + volume
};

// =========================================
// Status tiap rule — digunakan untuk
// observability via MQTT publish
// =========================================
struct SoilRuleFlags {
    bool heartbeatTimeout;  // Rule #1: tidak ada paket ESP-NOW baru dalam X menit
    bool outOfRange;        // Rule #2: ADC di luar batas kalibrasi wajar (BOBOT BESAR)
    bool flatline;          // Rule #3: ADC hampir tidak berubah meski irigasi jalan
    bool noResponse;        // Rule #4: ADC tidak turun setelah siklus irigasi
};

// =========================================
// SoilHealthMonitor
//
// Mengevaluasi kualitas data sensor kelembapan
// secara berkala dan independen dari status
// koneksi ESP-NOW. Menyimpan mode ke NVS agar
// tidak hilang saat reboot.
//
// Bobot health:
//   Rule #1 Heartbeat   : 20 poin
//   Rule #2 Out-of-range: 40 poin  <- bobot lebih besar (bukti kerusakan hardware pasti)
//   Rule #3 Flatline    : 20 poin
//   Rule #4 No response : 20 poin
//
// Mode irigasi dipaksa dari IRRIGATION_MODE_SOURCE di SystemConfig.h:
// 0 = HUMIDITY/ESP-NOW, 1 = TIMER. Health score tetap tersedia untuk telemetry.
// =========================================
class SoilHealthMonitor {
public:
    SoilHealthMonitor(ESPNowManager& espNow, ConfigManager& config);

    // Panggil di setup() -- load mode dari NVS
    void begin();

    // Panggil tiap loop dari FertigationFSM::handleReady()
    // soilADC             : nilai ADC kelembapan saat ini
    // irrigCompleted      : true pada loop pertama setelah satu siklus irigasi selesai
    // currentWetThreshold : wetThreshold dari resep aktif (untuk rule #4)
    void update(uint16_t soilADC, bool irrigCompleted, uint16_t currentWetThreshold = 3650);

    // Mode irigasi aktif saat ini
    IrrigationMode getMode() const { return _mode; }

    // Health score 0-100
    uint8_t getHealthScore() const { return _healthScore; }

    // Status tiap rule saat ini
    SoilRuleFlags getActiveRules() const { return _rules; }

    // Kembalikan ke HUMIDITY mode -- HANYA dipanggil dari perintah MQTT eksplisit.
    // Tidak boleh ada logic internal yang memanggil ini secara otomatis.
    void resetToHumidityMode();

    // Set ke TIMER mode -- dipanggil dari perintah MQTT eksplisit.
    void switchToTimerMode();

private:
    // Evaluasi semua rule dan hitung health score.
    // Dipanggil setiap SOIL_HEALTH_EVAL_INTERVAL.
    void evaluate(uint16_t soilADC);

    // Simpan mode ke NVS namespace "cfg_soil"
    void saveMode();

    // Tambahkan ADC ke ring buffer flatline
    void pushADCBuffer(uint16_t adc);

    // Hitung max-min dari ring buffer ADC
    uint16_t adcBufferRange() const;

    ESPNowManager& _espNow;
    ConfigManager& _config;

    IrrigationMode _mode        = IrrigationMode::HUMIDITY;
    uint8_t        _healthScore = 100;
    SoilRuleFlags  _rules       = {false, false, false, false};

    // --- Evaluasi timer ---
    unsigned long _lastEvalMs = 0;

    // --- Rule #3: Flatline ring buffer ---
    uint16_t _adcBuffer[SOIL_ADC_BUFFER_SIZE];
    uint8_t  _adcBufIdx   = 0;
    bool     _adcBufFull  = false;

    // Berapa siklus irigasi sudah selesai (untuk flatline gate)
    uint8_t  _irrigCycleCount = 0;

    // --- Rule #4: No response after watering ---
    bool          _waitingPostIrrig = false;  // flag: menunggu bacaan pasca-irigasi
    uint16_t      _adcBeforeIrrig   = 0;      // ADC sebelum siklus irigasi
    unsigned long _irrigDoneMs      = 0;      // kapan irigasi selesai
    bool          _rule4Evaluated   = false;  // apakah rule #4 sudah dievaluasi untuk siklus ini

    // Cooldown sebelum baca ADC pasca-irigasi (biarkan air meresap dulu)
    static constexpr unsigned long POST_IRRIG_COOLDOWN_MS = 120000UL; // 2 menit

    Preferences _prefs;
};

#endif
