#include "SoilHealthMonitor.h"

static const char* NS_SOIL = "cfg_soil";

// =========================================
// Constructor
// =========================================
SoilHealthMonitor::SoilHealthMonitor(ESPNowManager& espNow, ConfigManager& config)
    : _espNow(espNow), _config(config)
{
    memset(_adcBuffer, 0, sizeof(_adcBuffer));
}

// =========================================
// begin() -- load mode dari NVS (persisten across reboot)
// FIX: sebelumnya selalu meng-override NVS dengan IRRIGATION_MODE_SOURCE
// compile-time flag, jadi pilihan mode dari web (MQTT) hilang setiap boot.
// Sekarang: NVS jadi source of truth. Compile-time flag hanya default
// jika NVS masih kosong (first boot / setelah wipe).
// =========================================
void SoilHealthMonitor::begin() {
    _prefs.begin(NS_SOIL, true);  // read-only
    uint8_t storedMode = _prefs.getUChar("mode", 0xFF);  // 0xFF = sentinel "belum diset"
    _prefs.end();

    if (storedMode == static_cast<uint8_t>(IrrigationMode::HUMIDITY) ||
        storedMode == static_cast<uint8_t>(IrrigationMode::TIMER)) {
        _mode = static_cast<IrrigationMode>(storedMode);
    } else {
        // NVS masih kosong — pakai compile-time default lalu simpan
#if IRRIGATION_MODE_SOURCE == 1
        _mode = IrrigationMode::TIMER;
#else
        _mode = IrrigationMode::HUMIDITY;
#endif
        saveMode();
    }
}

// =========================================
// update() -- dipanggil tiap loop dari handleReady()
// =========================================
void SoilHealthMonitor::update(uint16_t soilADC, bool irrigCompleted, uint16_t currentWetThreshold) {
    unsigned long now = millis();

    // --- Catat ADC ke ring buffer ---
    pushADCBuffer(soilADC);

    // --- Tangkap event irigasi selesai ---
    if (irrigCompleted && !_waitingPostIrrig) {
        _adcBeforeIrrig    = soilADC;
        _irrigDoneMs       = now;
        _waitingPostIrrig  = true;
        _rule4Evaluated    = false;
        _irrigCycleCount++;
    }

    // --- Evaluasi rule #4 setelah cooldown pasca-irigasi ---
    if (_waitingPostIrrig && !_rule4Evaluated) {
        if (now - _irrigDoneMs >= POST_IRRIG_COOLDOWN_MS) {
            // ADC kapasitif: basah = angka LEBIH KECIL (kering = ~3900, basah = ~3400)
            // "Turun signifikan menuju wetThreshold" artinya soilADC sekarang
            // berkurang >= SOIL_RESPONSE_MIN_DELTA dibanding sebelum irigasi
            bool responded = (_adcBeforeIrrig > soilADC) &&
                             ((_adcBeforeIrrig - soilADC) >= SOIL_RESPONSE_MIN_DELTA);
            _rules.noResponse = !responded;
            _rule4Evaluated   = true;
            _waitingPostIrrig = false;
        }
    }

    // --- Evaluasi periodis (interval) ---
    if (now - _lastEvalMs >= SOIL_HEALTH_EVAL_INTERVAL) {
        _lastEvalMs = now;
        evaluate(soilADC);

        // Mode irigasi dipilih manual dari SystemConfig.h.
    }
}

// =========================================
// evaluate() -- hitung ulang health score
// =========================================
void SoilHealthMonitor::evaluate(uint16_t soilADC) {
    unsigned long now = millis();

    // --- Rule #1: Heartbeat timeout ---
    unsigned long lastRecv = _espNow.getLastReceiveTime();
    // Jika belum pernah terima paket sama sekali (lastRecv == 0), dianggap timeout
    // setelah SOIL_HEARTBEAT_TIMEOUT berlalu sejak boot
    if (lastRecv == 0) {
        _rules.heartbeatTimeout = (now >= SOIL_HEARTBEAT_TIMEOUT);
    } else {
        _rules.heartbeatTimeout = (now - lastRecv > SOIL_HEARTBEAT_TIMEOUT);
    }

    // --- Rule #2: Out-of-range ---
    _rules.outOfRange = (soilADC < SOIL_ADC_MIN_VALID || soilADC > SOIL_ADC_MAX_VALID);

    // --- Rule #3: Flatline ---
    // Hanya evaluasi jika sudah ada cukup siklus irigasi (agar tidak false-positive
    // di awal sebelum ada airflow) dan buffer sudah penuh
    if (_adcBufFull && _irrigCycleCount >= SOIL_MIN_CYCLES_FOR_FLATLINE) {
        _rules.flatline = (adcBufferRange() < SOIL_FLATLINE_DELTA);
    } else {
        _rules.flatline = false;
    }

    // Rule #4 sudah dievaluasi secara event-driven di update(), bukan di sini.
    // Hanya reset ke false jika belum pernah dievaluasi (state awal).

    // --- Hitung health score ---
    // Mulai dari 100, kurangi per rule yang aktif
    int score = 100;
    if (_rules.heartbeatTimeout) score -= 20;  // Rule #1: bobot 20
    if (_rules.outOfRange)       score -= 40;  // Rule #2: bobot 40 (lebih besar)
    if (_rules.flatline)         score -= 20;  // Rule #3: bobot 20
    if (_rules.noResponse)       score -= 20;  // Rule #4: bobot 20

    if (score < 0) score = 0;
    _healthScore = static_cast<uint8_t>(score);
}

// =========================================
// switchToTimerMode()
// =========================================
void SoilHealthMonitor::switchToTimerMode() {
    _mode = IrrigationMode::TIMER;
    saveMode();
}

// =========================================
// resetToHumidityMode() -- HANYA via MQTT command
// =========================================
void SoilHealthMonitor::resetToHumidityMode() {
    _mode = IrrigationMode::HUMIDITY;
    // Reset rule flags dan score agar evaluasi fresh
    _rules        = {false, false, false, false};
    _healthScore  = 100;
    _irrigCycleCount  = 0;
    _adcBufFull   = false;
    _adcBufIdx    = 0;
    _waitingPostIrrig = false;
    _rule4Evaluated   = false;
    memset(_adcBuffer, 0, sizeof(_adcBuffer));
    saveMode();
}

// =========================================
// saveMode() -- simpan mode ke NVS
// =========================================
void SoilHealthMonitor::saveMode() {
    _prefs.begin(NS_SOIL, false);
    _prefs.putUChar("mode", static_cast<uint8_t>(_mode));
    _prefs.end();
}

// =========================================
// pushADCBuffer() -- tambah sample ke ring buffer
// =========================================
void SoilHealthMonitor::pushADCBuffer(uint16_t adc) {
    _adcBuffer[_adcBufIdx] = adc;
    _adcBufIdx = (_adcBufIdx + 1) % SOIL_ADC_BUFFER_SIZE;
    if (_adcBufIdx == 0) {
        _adcBufFull = true;  // buffer sudah penuh minimal sekali
    }
}

// =========================================
// adcBufferRange() -- max - min dari ring buffer
// =========================================
uint16_t SoilHealthMonitor::adcBufferRange() const {
    uint8_t count = _adcBufFull ? SOIL_ADC_BUFFER_SIZE : _adcBufIdx;
    if (count == 0) return 0;

    uint16_t minVal = _adcBuffer[0];
    uint16_t maxVal = _adcBuffer[0];

    for (uint8_t i = 1; i < count; i++) {
        if (_adcBuffer[i] < minVal) minVal = _adcBuffer[i];
        if (_adcBuffer[i] > maxVal) maxVal = _adcBuffer[i];
    }

    return maxVal - minVal;
}
