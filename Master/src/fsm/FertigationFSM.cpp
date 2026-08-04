#include "FertigationFSM.h"
#include "../config/Constants.h"
#include "../config/SystemConfig.h"

static bool isMinuteInsideWindow(uint16_t nowMinute, uint16_t startMinute, uint16_t endMinute) {
    if (startMinute == endMinute) return false;
    if (startMinute < endMinute) {
        return nowMinute >= startMinute && nowMinute < endMinute;
    }
    return nowMinute >= startMinute || nowMinute < endMinute;
}

FertigationFSM::FertigationFSM(
    SensorManager&     sensors,
    RelayManager&      relays,
    RTCManager&        rtc,
    RecipeManager&     recipe,
    IrrigationRecipe&  irrigation,
    FlowMeter&         a,
    FlowMeter&         b,
    FlowMeter&         irrig,
    RecoveryManager&   recoveryManager,
    ConfigManager&     config,
    SoilHealthMonitor& soilHealth
)
:
sensorManager(sensors),
relayManager(relays),
rtcManager(rtc),
recipeManager(recipe),
irrigationRecipe(irrigation),
recovery(recoveryManager),
configManager(config),
nutrientAFlow(a),
nutrientBFlow(b),
irrigFlow(irrig),
soilHealthMonitor(soilHealth)
{
    lastStateBeforeError = FertigationState::IDLE;

    waitingRecovery = false;

    state = FertigationState::IDLE;
    stateStartTime = 0;

    stateInitialized = false;

    targetNutrientA = 0;
    targetNutrientB = 0;

    targetPPM = 0;
    targetMinPH = 0;
    targetMaxPH = 0;
    lastMixDay = 0;
    lastMixMonth = 0;
    lastMixYear = 0;

    lastPulseTime = 0;
    pulseOpenState = false;
    correctionOrigin = FertigationState::VALIDATE;

    _irrigJustCompleted = false;
    _activeSlotIdx = -1;
    _timerSlotRunning = false;
}

//! Begin
void FertigationFSM::begin() {
    stopNutrientFlowCounting();

    if (!rtcManager.isOk()) {
        logError("[FSM] RTC gagal — masuk ERROR state");
        enterError(ErrorCode::RTC_FAILURE);
        return;
    }

    restoreRecovery();

    if (recovering) {
        logRecovery("[FSM] Resume From Power Failure");
        return;
    }

    if (!configManager.isConfigured()) {
        logStateAction("[FSM] Belum ada konfigurasi dari web — menunggu di IDLE");
        changeState(FertigationState::IDLE);
    } else {
#if ENABLE_IRRIGATION_TEST
        logStateAction("[FSM][TEST] IRRIGATION_TEST: skip mix, langsung PRE_IRRIGATION_MIX");
        startIrrigationOutput();
#elif SKIP_DAILY_SCHEDULE || ENABLE_FULL_SYSTEM_TEST
        changeState(FertigationState::PREPARE_DAILY_MIX);
#else
        changeState(FertigationState::WAIT_DAILY_MIX);
#endif
    }
}

//! Update
void FertigationFSM::update() {
    rtcManager.refresh();
    sensorManager.update();
    sensor = sensorManager.getData();

    if (configManager.isConfigured()) {
        uint8_t numStages = configManager.getNumRecipeStages();
        if (numStages > 0) {
            uint16_t maxAge = configManager.getRecipeStage(numStages - 1).maxAgeDays;
            uint16_t age = rtcManager.getPlantAgeDays();
            if (age > maxAge) {
                logStateAction("[FSM] Umur tanaman melebihi batas resep maksimal — reset konfigurasi");
                configManager.clearConfig();
                changeState(FertigationState::IDLE);
            }
        }
    }

    switch (state) {
        case FertigationState::IDLE:
            handleIdle();
            break;

        case FertigationState::WAIT_DAILY_MIX:
            handleWaitDailyMix();
            break;

        case FertigationState::PREPARE_DAILY_MIX:
            handlePrepareDailyMix();
            break;

        case FertigationState::FILL_WATER:
            handleFillWater();
            break;

        case FertigationState::PRE_MIX_A:
            handlePreMixA();
            break;

        case FertigationState::ADD_NUTRIENT_A:
            handleAddNutrientA();
            break;

        case FertigationState::MIX_A:
            handleMixA();
            break;

        case FertigationState::PRE_MIX_B:
            handlePreMixB();
            break;

        case FertigationState::ADD_NUTRIENT_B:
            handleAddNutrientB();
            break;

        case FertigationState::MIX_B:
            handleMixB();
            break;

        case FertigationState::VALIDATE:
            handleValidate();
            break;

        case FertigationState::PRE_MIX_CORRECTION:
            handlePreMixCorrection();
            break;

        case FertigationState::CORRECT_PPM:
            handleCorrectPPM();
            break;

        case FertigationState::CORRECTION_MIX:
            handleCorrectionMix();
            break;

        case FertigationState::ESTIMATION_DOSE:
            handleEstimationDose();
            break;

        case FertigationState::READY:
            handleReady();
            break;

        case FertigationState::PRE_IRRIGATION_MIX:
            handlePreIrrigationMix();
            break;

        case FertigationState::PRE_IRRIGATION_VALIDATE:
            handlePreIrrigationValidate();
            break;

        case FertigationState::IRRIGATION:
            handleIrrigation();
            break;

        case FertigationState::ERROR:
            handleError();
            break;

        default:
            logError("[FSM] Unknown state — fallback to IDLE");
            changeState(FertigationState::IDLE);
            break;
    }
}

//! ChangeState
void FertigationFSM::changeState(FertigationState newState) {
    stopNutrientFlowCounting();
    state = newState;
    stateStartTime = millis();
    stateInitialized = false;
    saveRecovery();
    logStateTransition(newState);
}

//! GetState
FertigationState
FertigationFSM::getState() const {
    return state;
}

//! GetError
ErrorCode
FertigationFSM::getError() const {
    return currentError;
}

//! CONDITION HELPERS
bool FertigationFSM::isTankSafeForMixing() {
    return sensor.tankVolume >= 0.0f;
}

bool FertigationFSM::isPPMInRange() {
    return abs(sensor.ppm - targetPPM) <= configManager.getPPMTolerance();
}

bool FertigationFSM::isPPMAcceptableForUse() {
    return sensor.ppm >= (targetPPM - configManager.getPPMTolerance());
}

bool FertigationFSM::isStateTimeout(unsigned long timeout) {
    return millis() - stateStartTime > timeout;
}

//! ACTUATOR HELPERS
void FertigationFSM::startMixer() {
    relayManager.on(RELAY_PUMP_MIX);
    relayManager.on(RELAY_MIXER_STIR);
}

void FertigationFSM::stopMixer() {
    relayManager.off(RELAY_PUMP_MIX);
    relayManager.off(RELAY_MIXER_STIR);
}

void FertigationFSM::startIrrigationOutput() {
    relayManager.on(RELAY_PUMP_MIX);
    relayManager.on(RELAY_SOLENOID_IRRIG);
}

void FertigationFSM::stopIrrigationOutput() {
    relayManager.off(RELAY_PUMP_MIX);
    relayManager.off(RELAY_SOLENOID_IRRIG);
}

void FertigationFSM::openWaterInlet() {
    relayManager.on(RELAY_WATER_INLET);
}

void FertigationFSM::closeWaterInlet() {
    relayManager.off(RELAY_WATER_INLET);
}

void FertigationFSM::startFillStirrer() {
    relayManager.on(RELAY_PUMP_MIX);
    relayManager.on(RELAY_MIXER_STIR);
}

void FertigationFSM::stopFillStirrer() {
    relayManager.off(RELAY_PUMP_MIX);
    relayManager.off(RELAY_MIXER_STIR);
}

void FertigationFSM::setWarning(ErrorCode error) {
    currentError = error;
}

void FertigationFSM::clearWarning() {
    currentError = ErrorCode::NONE;
}

void FertigationFSM::preMixNutrientA() {
    relayManager.on(RELAY_PUMP_A);
}

void FertigationFSM::stopPreMixNutrientA() {
    relayManager.off(RELAY_PUMP_A);
}

void FertigationFSM::startNutrientA() {
    relayManager.on(RELAY_PUMP_A);
    relayManager.on(RELAY_SOLENOID_A);
}

void FertigationFSM::stopNutrientA() {
    relayManager.off(RELAY_PUMP_A);
    relayManager.off(RELAY_SOLENOID_A);
}

void FertigationFSM::preMixNutrientB() {
    relayManager.on(RELAY_PUMP_B);
}

void FertigationFSM::stopPreMixNutrientB() {
    relayManager.off(RELAY_PUMP_B);
}

void FertigationFSM::startNutrientB() {
    relayManager.on(RELAY_PUMP_B);
    relayManager.on(RELAY_SOLENOID_B);
}

void FertigationFSM::stopNutrientB() {
    relayManager.off(RELAY_PUMP_B);
    relayManager.off(RELAY_SOLENOID_B);
}

void FertigationFSM::stopNutrientFlowCounting() {
    nutrientAFlow.setCountingEnabled(false);
    nutrientBFlow.setCountingEnabled(false);
}

//! RECIPE
void FertigationFSM::prepareDailyRecipe() {
    uint16_t age = rtcManager.getPlantAgeDays();

    // Ambil resep hari ini langsung — tidak ada lagi grouping N-hari.
    // Fungsi ini dipanggil SETELAH tangki penuh (di akhir FILL_WATER),
    // sehingga dosis selalu full dose tanpa scaling rasio.
    NutrientRecipe r = recipeManager.getRecipe(age);
    targetPPM    = (uint16_t)r.targetPPM;
    targetMinPH  = r.targetMinPH;
    targetMaxPH  = r.targetMaxPH;

    targetNutrientA = configManager.getInitialNutrientA();
    targetNutrientB = configManager.getInitialNutrientB();

    logRecipe();
}

// =========================================
// refreshDailyChecks()
// Dipanggil dari handleWaitDailyMix() setiap tick.
// Guard internal: hanya eksekusi sekali per hari (berdasarkan plant age).
// =========================================
void FertigationFSM::refreshDailyChecks() {
    uint16_t age = rtcManager.getPlantAgeDays();
    if (age == _lastDailyCheckDay) return;
    _lastDailyCheckDay = age;

    // 1. Refresh threshold irigasi harian (independen dari siklus mixing)
    currentIrrigation = irrigationRecipe.getRecipe(age);

    // 2. Reset flag stir harian (hanya stir sore; stir pagi dihapus)
    _eveningStirDone = false;

    // 3. Cek minimum-liter — update status warning tangki harian.
    //    Stir pagi sengaja TIDAK dipicu di sini: RELAY_MIXER_STIR tidak boleh
    //    nyala selama state WAIT_DAILY_MIX (risiko overheat solenoid).
    checkMinimumWater();
}

// =========================================
// checkMinimumWater() — Opsi A: safety floor sederhana
// Blokir irigasi jika tangki mendekati kosong untuk mencegah dry-run pompa.
// Tidak terkait target fill harian — murni proteksi hardware.
// Return true jika aman, false jika diblokir.
// =========================================
bool FertigationFSM::checkMinimumWater() {
    if (sensor.tankVolume < TANK_SAFETY_FLOOR_LITER) {
        if (!_tankLowBlocked) {
            _tankLowBlocked = true;
            setWarning(ErrorCode::WAITING_FOR_FILL);
        }
        return false;
    }

    if (_tankLowBlocked) {
        _tankLowBlocked = false;
        clearWarning();
        logStateAction("[FSM] Tank volume kembali aman — irigasi diizinkan");
    }
    return true;
}

// =========================================
// handleStirSchedule()
// Hentikan stir jika durasi tercapai.
// Trigger stir sore jika jam/menit sesuai konfigurasi.
// Hanya jalan saat state READY.
// =========================================
void FertigationFSM::handleStirSchedule() {
    // Hentikan stir yang sedang berjalan jika durasinya sudah habis
    if (_stirring) {
        if (millis() - _stirStartMs >= configManager.getStirDurationMs()) {
            stopFillStirrer();
            _stirring = false;
            logStateAction("[FSM] Stir selesai");
        }
        return;  // tidak trigger stir baru selagi masih stir
    }

    // Trigger stir sore
    uint8_t h = rtcManager.getHour();
    uint8_t m = rtcManager.getMinute();
    if (!_eveningStirDone &&
        h == configManager.getStirEveningHour() &&
        m == configManager.getStirEveningMinute()) {
        startFillStirrer();
        _stirring        = true;
        _stirStartMs     = millis();
        _eveningStirDone = true;
        logStateAction("[FSM] Stir sore dimulai");
    }
}

//! MIXING HELPERS
bool FertigationFSM::beginMixing() {
    if (!isTankSafeForMixing()) {
        logError("[FSM] Mixer Dry Run");
        gotoError(ErrorCode::MIXER_DRY_RUN);
        return false;
    }

    startMixer();
    stateInitialized = true;
    return true;
}

bool FertigationFSM::updateMixing(uint32_t mixTime) {
    if (millis() - stateStartTime >= mixTime) {
        stopMixer();
        return true;
    }
    return false;
}

//! TRANSITION HELPERS
void FertigationFSM::gotoReady() {
    changeState(FertigationState::READY);
}

void FertigationFSM::gotoValidate() {
    changeState(FertigationState::VALIDATE);
}

void FertigationFSM::gotoCorrection() {
    correctionOrigin = state;
    changeState(FertigationState::PRE_MIX_CORRECTION);
}

void FertigationFSM::gotoError(ErrorCode error) {
    enterError(error);
}

void FertigationFSM::gotoPostCorrection() {
    if (correctionOrigin == FertigationState::PRE_IRRIGATION_VALIDATE) {
        changeState(FertigationState::PRE_IRRIGATION_VALIDATE);
    } else if (correctionOrigin == FertigationState::ESTIMATION_DOSE) {
        // Setelah CORRECTION_MIX pasca estimation dose, langsung READY —
        // tidak balik ke VALIDATE karena TDS tidak bisa baca di atas 900 ppm.
        gotoReady();
    } else {
        gotoValidate();
    }
}

//! RECOVERY HELPERS
void FertigationFSM::consumeRecovery() {
    if (recovering) {
        logRecovery("[FSM] Resuming from recovery");
        recovering = false;
    }
}

void FertigationFSM::saveRecovery() {
    RecoveryData data;

    data.state = static_cast<uint16_t>(state);
    data.waterPulse = 0; // Removed flowWater
    data.nutrientAPulse = nutrientAFlow.pulseCount;
    data.nutrientBPulse = nutrientBFlow.pulseCount;
    data.day = lastMixDay;
    data.month = lastMixMonth;
    data.year = lastMixYear;
    data.lastStateBeforeError = static_cast<uint16_t>(lastStateBeforeError);
    data.batchRunning = true;

    recovery.save(data);
}

void FertigationFSM::restoreRecovery() {
    RecoveryData data = recovery.load();

    if (!data.batchRunning)
        return;

    const uint16_t maxValidState = static_cast<uint16_t>(FertigationState::ERROR);

    if (data.state > maxValidState) {
        logError("[FSM] Recovery data corrupt — skip restore");
        return;
    }

    if (data.state == static_cast<uint16_t>(FertigationState::ERROR)) {
        logRecovery("[FSM] Stored state is ERROR — restore lastStateBeforeError");

        if (data.lastStateBeforeError <= maxValidState) {
            state = static_cast<FertigationState>(data.lastStateBeforeError);
        } else {
            state = FertigationState::WAIT_DAILY_MIX;
        }
    } else {
        state = static_cast<FertigationState>(data.state);
    }

    lastMixDay   = data.day;
    lastMixMonth = data.month;
    lastMixYear  = data.year;

    lastStateBeforeError = static_cast<FertigationState>(
        data.lastStateBeforeError <= maxValidState
            ? data.lastStateBeforeError
            : static_cast<uint16_t>(FertigationState::IDLE)
    );

    nutrientAFlow.setPulseCount(data.nutrientAPulse);
    nutrientBFlow.setPulseCount(data.nutrientBPulse);

    recovering = true;
    stateInitialized = false;

    logRecovery(
        "[FSM] Recovery State : ",
        state
    );
}

void FertigationFSM::clearRecovery() {
    recovery.clear();
}

//! ERROR HELPERS
void FertigationFSM::enterError(ErrorCode error) {
    relayManager.allOff();
    setError(error);
    lastStateBeforeError = state;
    changeState(FertigationState::ERROR);
}

void FertigationFSM::setError(ErrorCode error) {
    currentError = error;
    waitingRecovery = true;
}

void FertigationFSM::clearError() {
    waitingRecovery = false;
    recovering = true;
}

void FertigationFSM::recoverFromError() {
    relayManager.allOff();
    stateInitialized = false;
    clearError();
    logRecovery(
        "[FSM] Recover to state : ",
        lastStateBeforeError
    );
    changeState(lastStateBeforeError);
}

//! STATE HANDLERS
void FertigationFSM::handleIdle() {
    relayManager.allOff();

    if (configManager.isConfigured()) {
        logStateAction("[FSM] Konfigurasi terdeteksi — memulai penjadwalan fertigasi");
#if SKIP_DAILY_SCHEDULE || ENABLE_FULL_SYSTEM_TEST
        changeState(FertigationState::PREPARE_DAILY_MIX);
#else
        changeState(FertigationState::WAIT_DAILY_MIX);
#endif
    }
}

void FertigationFSM::handleWaitDailyMix() {
    uint8_t hour   = rtcManager.getHour();
    uint8_t minute = rtcManager.getMinute();
    uint16_t day   = rtcManager.getPlantAgeDays();
    uint8_t month  = rtcManager.getMonth();
    uint16_t year  = rtcManager.getYear();

    // Refresh harian: update threshold irigasi, cek minimum-liter, trigger stir pagi.
    // refreshDailyChecks() di-guard internal agar hanya jalan sekali per hari.
    refreshDailyChecks();

    // Trigger mixing tiap pagi di jam yang dikonfigurasi — guard per hari agar tidak retrigger
    // dalam menit yang sama (cek lastMixDay vs plant age hari ini)
    if (hour == rtcManager.getDailyMixHour() && minute == rtcManager.getDailyMixMinute()
        && day != lastMixDay) {
        lastMixDay   = day;
        lastMixMonth = month;
        lastMixYear  = year;
        changeState(FertigationState::PREPARE_DAILY_MIX);
    }
}

void FertigationFSM::handlePrepareDailyMix() {
    // FIX BUG DAY-2 FILL_WATER_TIMEOUT:
    // Matikan SEMUA relay & reset flag stale sebelum masuk FILL_WATER. Tanpa ini,
    // bila daily-mix trigger jatuh saat timer irrigation slot masih aktif di READY,
    // RELAY_SOLENOID_IRRIG + RELAY_PUMP_MIX tetap ON dan air justru dipompa keluar
    // ke tanaman selama FILL_WATER — toren tidak pernah terisi → WATER_TIMEOUT.
    relayManager.allOff();
    _timerSlotRunning  = false;
    _activeSlotIdx     = -1;
    _fillTargetReached = false;
    _stirring          = false;
    _nutrientDraining  = false;
    _tankLowBlocked    = false;

    // Reset flow meter dan mulai pengisian air otomatis di FILL_WATER.
    // prepareDailyRecipe() dipanggil setelah tangki penuh (di akhir handleFillWater()).
    nutrientAFlow.reset();
    nutrientBFlow.reset();
    clearRecovery();
    // Reset estimation state setiap siklus harian — dosis estimasi tidak boleh
    // dibawa ke hari berikutnya karena PPM anchor berubah setelah FILL_WATER.
    _estimationActive = false;
    _estimAnchorPPM   = 0.0f;
    _estimTargetA_L   = 0.0f;
    _estimTargetB_L   = 0.0f;
    _estimDosedA_L    = 0.0f;
    _estimDosedB_L    = 0.0f;
    changeState(FertigationState::FILL_WATER);
}

void FertigationFSM::handleFillWater() {
    if (isStateTimeout(WATER_FILL_TIMEOUT)) {
        closeWaterInlet();
        gotoError(ErrorCode::WATER_TIMEOUT);
        return;
    }

    if (sensor.tankVolume < 0.0f) {
        closeWaterInlet();
        gotoError(ErrorCode::ULTRASONIC_FAILURE);
        return;
    }

    if (!stateInitialized) {
        consumeRecovery();
        // Safety: pastikan output irigasi TERTUTUP selama FILL_WATER supaya
        // air yang masuk lewat WATER_INLET tidak sekaligus dipompa keluar via
        // RELAY_PUMP_MIX + RELAY_SOLENOID_IRRIG (penyebab WATER_TIMEOUT day 2).
        relayManager.off(RELAY_SOLENOID_IRRIG);
        relayManager.off(RELAY_SOLENOID_A);
        relayManager.off(RELAY_SOLENOID_B);
        relayManager.off(RELAY_PUMP_A);
        relayManager.off(RELAY_PUMP_B);
        relayManager.off(RELAY_PUMP_MIX);
        relayManager.off(RELAY_MIXER_STIR);

        lastTankVolume      = sensor.tankVolume;
        _fillStartVolume    = sensor.tankVolume;
        lastLevelChangeTime = millis();
        _lastRefillAlertMs  = 0;  // paksa kirim alert segera
        _fillTargetReached  = false;
        if (sensor.tankVolume < configManager.getTargetFillVolume()) {
            openWaterInlet();
        } else {
            closeWaterInlet();
        }
        stateInitialized = true;
    }

    // Deteksi perubahan level yang nyata (di atas threshold noise ultrasonik)
    if (fabs(sensor.tankVolume - lastTankVolume) > WATER_LEVEL_NOISE_THRESHOLD) {
        lastTankVolume      = sensor.tankVolume;
        lastLevelChangeTime = millis();
    }

    // Overflow: warning non-fatal. Solenoid inlet wajib tutup, tetapi proses tetap
    // lanjut setelah level stabil karena volume aktual masih bisa divalidasi PPM.
    if (sensor.tankVolume > configManager.getTankCapacityLiter()) {
        closeWaterInlet();
        setWarning(ErrorCode::WATER_OVERFLOW);
    } else if (currentError == ErrorCode::WATER_OVERFLOW) {
        clearWarning();
    }

    // Solenoid NC dibuka selama volume belum mencapai target. Saat target tercapai,
    // solenoid ditutup dan sistem menunggu level stabil sebentar sebelum lanjut nutrisi.
    if (sensor.tankVolume >= configManager.getTargetFillVolume() ||
        sensor.tankVolume > configManager.getTankCapacityLiter()) {
        if (!_fillTargetReached) {
            _fillTargetReached   = true;
            _fillTargetReachedMs = millis();
        }
        closeWaterInlet();
        if (millis() - lastLevelChangeTime >= WATER_LEVEL_STABLE_TIMEOUT ||
            millis() - _fillTargetReachedMs >= WATER_LEVEL_STABLE_TIMEOUT) {
            prepareDailyRecipe();
            if (isPPMAcceptableForUse()) {
                relayManager.off(RELAY_PUMP_A);
                relayManager.off(RELAY_SOLENOID_A);
                relayManager.off(RELAY_PUMP_B);
                relayManager.off(RELAY_SOLENOID_B);
                if (!isPPMInRange()) {
                    setWarning(ErrorCode::OVER_PPM);
                } else if (currentError == ErrorCode::OVER_PPM) {
                    clearWarning();
                }
                logStateAction("[FSM] PPM already acceptable after fill — skip initial dosing");
                gotoReady();
                return;
            }

            // PPM belum cukup, lanjut dosing awal A/B.
            changeState(FertigationState::PRE_MIX_A);
        }
    } else if (!_fillTargetReached) {
        openWaterInlet();
        // Kirim progress fill ke MQTT secara periodik selama pengisian
        if (millis() - _lastRefillAlertMs >= NEED_REFILL_ALERT_INTERVAL_MS) {
            _lastRefillAlertMs        = millis();
            _refillDeficit            = configManager.getTargetFillVolume() - sensor.tankVolume;
            _needRefillAlertPending   = true;
        }
    }
}

void FertigationFSM::handlePreMixA() {
    if (!stateInitialized) {
        consumeRecovery();
        preMixNutrientA();
        stateInitialized = true;
        logStateAction("[FSM] Pre-Mix Nutrient A Tank");
    }

    if (isStateTimeout(PRE_MIX_TANK_TIME)) {
        changeState(FertigationState::ADD_NUTRIENT_A);
    }
}

void FertigationFSM::handleAddNutrientA() {
    if (isStateTimeout(NUTRIENT_TIMEOUT)) {
        relayManager.off(RELAY_PUMP_A);
        relayManager.off(RELAY_SOLENOID_A);
        gotoError(ErrorCode::NUTRIENT_A_TIMEOUT);
        return;
    }

    if (!stateInitialized) {
        bool wasRecovering = recovering;
        consumeRecovery();
        if (!wasRecovering) {
            nutrientAFlow.reset();
        }
        lastPulseTime     = millis();
        pulseOpenState    = true;
        _nutrientDraining = false;
        nutrientAFlow.setCountingEnabled(true);
        relayManager.on(RELAY_PUMP_A);
        relayManager.on(RELAY_SOLENOID_A);
        stateInitialized = true;
        logStateAction("[FSM] Dosing Nutrient A (5s ON / 1s OFF)");
    }

    // Phase drain: pompa sudah mati, solenoid terbuka, tunggu air turun ke toren
    if (_nutrientDraining) {
        if (millis() - lastPulseTime >= NUTRIENT_DRAIN_DELAY_MS) {
            relayManager.off(RELAY_SOLENOID_A);
            _nutrientDraining = false;
            changeState(FertigationState::MIX_A);
        }
        return;
    }

    // Target tercapai: matikan pompa, biarkan solenoid terbuka untuk drain
    if (sensor.flowA >= targetNutrientA) {
        nutrientAFlow.setCountingEnabled(false);
        relayManager.off(RELAY_PUMP_A);
        _nutrientDraining = true;
        lastPulseTime     = millis();
        logStateAction("[FSM] Nutrient A target reached — pompa mati, drain 1 menit");
        return;
    }

    // Pulsing 5s ON / 1s OFF — pompa A submersible perlu waktu lebih lama naikkan air
    unsigned long pulseDuration = pulseOpenState ? NUTRIENT_A_PULSE_ON_MS : NUTRIENT_A_PULSE_OFF_MS;
    if (millis() - lastPulseTime >= pulseDuration) {
        pulseOpenState = !pulseOpenState;
        lastPulseTime  = millis();
        if (pulseOpenState) {
            relayManager.on(RELAY_PUMP_A);
            relayManager.on(RELAY_SOLENOID_A);
        } else {
            relayManager.off(RELAY_PUMP_A);
            relayManager.off(RELAY_SOLENOID_A);
        }
    }
}

void FertigationFSM::handleMixA() {
    if (!stateInitialized) {
        if (!beginMixing()) {
            return;
        }
        logStateAction("[FSM] Mixing Larutan A (Main Chamber)");
    }

    if (updateMixing(MIX_A_TIME)) {
        changeState(FertigationState::PRE_MIX_B);
    }
}

void FertigationFSM::handlePreMixB() {
    if (!stateInitialized) {
        consumeRecovery();
        preMixNutrientB();
        stateInitialized = true;
        logStateAction("[FSM] Pre-Mix Nutrient B Tank");
    }

    if (isStateTimeout(PRE_MIX_TANK_TIME)) {
        changeState(FertigationState::ADD_NUTRIENT_B);
    }
}

void FertigationFSM::handleAddNutrientB() {
    if (isStateTimeout(NUTRIENT_TIMEOUT)) {
        relayManager.off(RELAY_PUMP_B);
        relayManager.off(RELAY_SOLENOID_B);
        gotoError(ErrorCode::NUTRIENT_B_TIMEOUT);
        return;
    }

    if (!stateInitialized) {
        bool wasRecovering = recovering;
        consumeRecovery();
        if (!wasRecovering) {
            nutrientBFlow.reset();
        }
        lastPulseTime     = millis();
        pulseOpenState    = true;
        _nutrientDraining = false;
        nutrientBFlow.setCountingEnabled(true);
        relayManager.on(RELAY_PUMP_B);
        relayManager.on(RELAY_SOLENOID_B);
        stateInitialized = true;
        logStateAction("[FSM] Dosing Nutrient B (1s ON / 1s OFF)");
    }

    // Phase drain: pompa sudah mati, solenoid terbuka, tunggu air turun ke toren
    if (_nutrientDraining) {
        if (millis() - lastPulseTime >= NUTRIENT_DRAIN_DELAY_MS) {
            relayManager.off(RELAY_SOLENOID_B);
            _nutrientDraining = false;
            changeState(FertigationState::MIX_B);
        }
        return;
    }

    // Target tercapai: matikan pompa, biarkan solenoid terbuka untuk drain
    if (sensor.flowB >= targetNutrientB) {
        nutrientBFlow.setCountingEnabled(false);
        relayManager.off(RELAY_PUMP_B);
        _nutrientDraining = true;
        lastPulseTime     = millis();
        logStateAction("[FSM] Nutrient B target reached — pompa mati, drain 1 menit");
        return;
    }

    // Pulsing 1s ON / 1s OFF
    unsigned long pulseDuration = pulseOpenState ? NUTRIENT_B_PULSE_ON_MS : NUTRIENT_B_PULSE_OFF_MS;
    if (millis() - lastPulseTime >= pulseDuration) {
        pulseOpenState = !pulseOpenState;
        lastPulseTime  = millis();
        if (pulseOpenState) {
            relayManager.on(RELAY_PUMP_B);
            relayManager.on(RELAY_SOLENOID_B);
        } else {
            relayManager.off(RELAY_PUMP_B);
            relayManager.off(RELAY_SOLENOID_B);
        }
    }
}

void FertigationFSM::handleMixB() {
    if (!stateInitialized) {
        if (!beginMixing()) {
            return;
        }
        logStateAction("[FSM] Mixing Larutan B (Main Chamber)");
    }

    if (updateMixing(MIX_B_TIME)) {
        gotoValidate();
    }
}

void FertigationFSM::handleValidate() {
    // Sensor TDS clip di ~900 ppm dan target resep di atas itu:
    // alih ke estimasi langsung (satu tembak), tidak pakai loop CORRECT_PPM.
    if (sensor.ppm >= TDS_SENSOR_CLIP_PPM && (float)targetPPM > TDS_SENSOR_CLIP_PPM) {
        if (sensor.tankVolume > 0.0f) {
            float mL = ((float)targetPPM - TDS_SENSOR_CLIP_PPM)
                       * sensor.tankVolume / PPM_PER_ML_PER_LITER_AB;
            _estimTargetA_L   = mL / 2.0f / 1000.0f;
            _estimTargetB_L   = mL / 2.0f / 1000.0f;
            _estimAnchorPPM   = sensor.ppm;
            _estimationActive = true;
            _estimDosedA_L    = 0.0f;
            _estimDosedB_L    = 0.0f;
            changeState(FertigationState::ESTIMATION_DOSE);
            return;
        }
    }

    if (isPPMAcceptableForUse()) {
        relayManager.off(RELAY_PUMP_A);
        relayManager.off(RELAY_SOLENOID_A);
        relayManager.off(RELAY_PUMP_B);
        relayManager.off(RELAY_SOLENOID_B);
        if (!isPPMInRange()) {
            setWarning(ErrorCode::OVER_PPM);
        } else if (currentError == ErrorCode::OVER_PPM) {
            clearWarning();
        }
        gotoReady();
    } else {
        gotoCorrection();
    }
}

void FertigationFSM::handlePreMixCorrection() {
    if (!stateInitialized) {
        consumeRecovery();
        
        preMixNutrientA();
        preMixNutrientB();

        stateInitialized = true;
        logStateAction("[FSM] Pre-Mix Nutrisi A & B sebelum Koreksi PPM");
    }

    if (isStateTimeout(PRE_MIX_CORRECTION_TIME)) {
        changeState(FertigationState::CORRECT_PPM);
    }
}

void FertigationFSM::handleCorrectPPM() {
    // Sensor clip sebelum target tercapai: hentikan loop 50 mL,
    // pindah ke estimation dose sekali tembak.
    if (sensor.ppm >= TDS_SENSOR_CLIP_PPM && (float)targetPPM > TDS_SENSOR_CLIP_PPM) {
        relayManager.off(RELAY_PUMP_A); relayManager.off(RELAY_SOLENOID_A);
        relayManager.off(RELAY_PUMP_B); relayManager.off(RELAY_SOLENOID_B);
        if (sensor.tankVolume > 0.0f) {
            float mL = ((float)targetPPM - TDS_SENSOR_CLIP_PPM)
                       * sensor.tankVolume / PPM_PER_ML_PER_LITER_AB;
            _estimTargetA_L   = mL / 2.0f / 1000.0f;
            _estimTargetB_L   = mL / 2.0f / 1000.0f;
            _estimAnchorPPM   = sensor.ppm;
            _estimationActive = true;
            _estimDosedA_L    = 0.0f;
            _estimDosedB_L    = 0.0f;
            changeState(FertigationState::ESTIMATION_DOSE);
            return;
        }
    }

    if (isPPMAcceptableForUse()) {
        relayManager.off(RELAY_PUMP_A);
        relayManager.off(RELAY_SOLENOID_A);
        relayManager.off(RELAY_PUMP_B);
        relayManager.off(RELAY_SOLENOID_B);
        if (!isPPMInRange()) {
            setWarning(ErrorCode::OVER_PPM);
        } else if (currentError == ErrorCode::OVER_PPM) {
            clearWarning();
        }
        gotoReady();
        return;
    }

    if (!stateInitialized) {
        bool wasRecovering = recovering;
        consumeRecovery();

        if (!wasRecovering) {
            nutrientAFlow.reset();
            nutrientBFlow.reset();
        }

        lastPulseTime = millis();
        pulseOpenState = true;

        if (sensor.flowA < CORRECTION_DOSE) {
            nutrientAFlow.setCountingEnabled(true);
            relayManager.on(RELAY_PUMP_A);
            relayManager.on(RELAY_SOLENOID_A);
        }
        if (sensor.flowB < CORRECTION_DOSE) {
            nutrientBFlow.setCountingEnabled(true);
            relayManager.on(RELAY_PUMP_B);
            relayManager.on(RELAY_SOLENOID_B);
        }

        stateInitialized = true;
        logStateAction("[FSM] Injeksi Dosis Koreksi PPM (Pulsed)");
    }

    // Pulsing solenoid Nutrisi A & B bersamaan (buka 1s, tutup 1s)
    if (millis() - lastPulseTime >= 1000) {
        pulseOpenState = !pulseOpenState;
        lastPulseTime = millis();

        if (pulseOpenState) {
            if (sensor.flowA < CORRECTION_DOSE) {
                relayManager.on(RELAY_PUMP_A);
                relayManager.on(RELAY_SOLENOID_A);
                logStateAction("[FSM] Nutrient A Solenoid OPEN (Correction)");
            }
            if (sensor.flowB < CORRECTION_DOSE) {
                relayManager.on(RELAY_PUMP_B);
                relayManager.on(RELAY_SOLENOID_B);
                logStateAction("[FSM] Nutrient B Solenoid OPEN (Correction)");
            }
        } else {
            relayManager.off(RELAY_PUMP_A);
            relayManager.off(RELAY_SOLENOID_A);
            relayManager.off(RELAY_PUMP_B);
            relayManager.off(RELAY_SOLENOID_B);
            logStateAction("[FSM] Nutrient A & B Solenoids CLOSED (Correction)");
        }
    }

    // Matikan segera jika sudah melampaui target
    if (sensor.flowA >= CORRECTION_DOSE) {
        nutrientAFlow.setCountingEnabled(false);
        relayManager.off(RELAY_PUMP_A);
        relayManager.off(RELAY_SOLENOID_A);
    }
    if (sensor.flowB >= CORRECTION_DOSE) {
        nutrientBFlow.setCountingEnabled(false);
        relayManager.off(RELAY_PUMP_B);
        relayManager.off(RELAY_SOLENOID_B);
    }

    if (sensor.flowA >= CORRECTION_DOSE && sensor.flowB >= CORRECTION_DOSE) {
        relayManager.off(RELAY_PUMP_A);
        relayManager.off(RELAY_SOLENOID_A);
        relayManager.off(RELAY_PUMP_B);
        relayManager.off(RELAY_SOLENOID_B);
        changeState(FertigationState::CORRECTION_MIX);
    }
}

void FertigationFSM::handleCorrectionMix() {
    if (!stateInitialized) {
        if (!beginMixing()) {
            return;
        }
        logStateAction("[FSM] Mixing Dosis Koreksi (Main Chamber)");
    }

    if (updateMixing(CORRECTION_MIX_TIME)) {
        gotoPostCorrection();
    }
}

void FertigationFSM::handleEstimationDose() {
    if (isStateTimeout(NUTRIENT_TIMEOUT)) {
        gotoError(ErrorCode::NUTRIENT_A_TIMEOUT);
        return;
    }

    if (!stateInitialized) {
        nutrientAFlow.reset();
        nutrientBFlow.reset();
        nutrientAFlow.setCountingEnabled(true);
        nutrientBFlow.setCountingEnabled(true);
        lastPulseTime  = millis();
        pulseOpenState = true;
        relayManager.on(RELAY_PUMP_A);
        relayManager.on(RELAY_SOLENOID_A);
        relayManager.on(RELAY_PUMP_B);
        relayManager.on(RELAY_SOLENOID_B);
        stateInitialized = true;
        Serial.printf(
            "t=%010lu | INFO  | FSM      | state=ESTIMATION_DOSE anchor=%.0fppm target_A=%.4fL target_B=%.4fL\n",
            millis(), _estimAnchorPPM, _estimTargetA_L, _estimTargetB_L
        );
    }

    // Pulsing A+B bersamaan (1s buka, 1s tutup) — sama dengan pola CORRECT_PPM
    if (millis() - lastPulseTime >= 1000) {
        pulseOpenState = !pulseOpenState;
        lastPulseTime  = millis();
        if (pulseOpenState) {
            if (sensor.flowA < _estimTargetA_L) {
                relayManager.on(RELAY_PUMP_A);
                relayManager.on(RELAY_SOLENOID_A);
            }
            if (sensor.flowB < _estimTargetB_L) {
                relayManager.on(RELAY_PUMP_B);
                relayManager.on(RELAY_SOLENOID_B);
            }
        } else {
            relayManager.off(RELAY_PUMP_A); relayManager.off(RELAY_SOLENOID_A);
            relayManager.off(RELAY_PUMP_B); relayManager.off(RELAY_SOLENOID_B);
        }
    }

    // Matikan masing-masing segera saat target tercapai
    if (sensor.flowA >= _estimTargetA_L) {
        nutrientAFlow.setCountingEnabled(false);
        relayManager.off(RELAY_PUMP_A);
        relayManager.off(RELAY_SOLENOID_A);
    }
    if (sensor.flowB >= _estimTargetB_L) {
        nutrientBFlow.setCountingEnabled(false);
        relayManager.off(RELAY_PUMP_B);
        relayManager.off(RELAY_SOLENOID_B);
    }

    // Keduanya selesai: simpan realisasi dari flow meter, lalu mix → READY
    if (sensor.flowA >= _estimTargetA_L && sensor.flowB >= _estimTargetB_L) {
        _estimDosedA_L = sensor.flowA;
        _estimDosedB_L = sensor.flowB;
        relayManager.off(RELAY_PUMP_A); relayManager.off(RELAY_SOLENOID_A);
        relayManager.off(RELAY_PUMP_B); relayManager.off(RELAY_SOLENOID_B);
        Serial.printf(
            "t=%010lu | INFO  | FSM      | state=ESTIMATION_DOSE done dosed_A=%.4fL dosed_B=%.4fL eff_ppm=%.0f\n",
            millis(), _estimDosedA_L, _estimDosedB_L, getEffectivePPM()
        );
        // correctionOrigin = ESTIMATION_DOSE memberi tahu gotoPostCorrection()
        // untuk ke READY, bukan balik ke VALIDATE (TDS tidak bisa validasi > 900 ppm).
        correctionOrigin = FertigationState::ESTIMATION_DOSE;
        changeState(FertigationState::CORRECTION_MIX);
    }
}

float FertigationFSM::getEffectivePPM() const {
    if (!_estimationActive) return sensor.ppm;
    if (sensor.tankVolume <= 0.0f) return sensor.ppm;

    // Saat sedang ESTIMATION_DOSE: flow meter masih inkremental jadi pakai
    // realtime sensor.flowA/B supaya PPM di web ikut naik smooth dari anchor
    // ~900 menuju target (mis. 1400) — bukan diam di 900 lalu tiba-tiba lompat.
    // Setelah dosing selesai, _estimDosed*_L sudah terisi (di-latch) dan
    // flow meter tidak di-reset sampai siklus harian berikutnya jadi tetap valid.
    float aL = _estimDosedA_L;
    float bL = _estimDosedB_L;
    if (state == FertigationState::ESTIMATION_DOSE) {
        if (sensor.flowA > aL) aL = sensor.flowA;
        if (sensor.flowB > bL) bL = sensor.flowB;
    }

    float dosedML = (aL + bL) * 1000.0f;
    return _estimAnchorPPM + (dosedML * PPM_PER_ML_PER_LITER_AB) / sensor.tankVolume;
}

void FertigationFSM::handleReady() {
    // Update monitor status
    soilHealthMonitor.update(sensor.soilADC, _irrigJustCompleted, currentIrrigation.wetThreshold);
    _irrigJustCompleted = false;

    // Jalankan stir schedule (stir sore sesuai jam config + stop timer jika durasi habis)
    handleStirSchedule();

    // Cek jadwal daily mix — jika jam mixing tiba di hari baru, mulai siklus mixing harian.
    // Guard per hari (day != lastMixDay) agar tidak retrigger dalam menit yang sama.
    {
        uint8_t  hour   = rtcManager.getHour();
        uint8_t  minute = rtcManager.getMinute();
        uint16_t day    = rtcManager.getPlantAgeDays();
        uint8_t  month  = rtcManager.getMonth();
        uint16_t year   = rtcManager.getYear();

        if (hour == rtcManager.getDailyMixHour() && minute == rtcManager.getDailyMixMinute()
            && day != lastMixDay) {
            lastMixDay   = day;
            lastMixMonth = month;
            lastMixYear  = year;
            logStateAction("[FSM] READY → Daily mix schedule triggered — mulai siklus mixing harian");
            changeState(FertigationState::PREPARE_DAILY_MIX);
            return;
        }
    }

    // Refresh daily checks (threshold irigasi + minimum-liter) — guard internal 1x/hari
    refreshDailyChecks();

    // Cek minimum-liter dinamis — blokir irigasi jika tangki tidak cukup.
    // Stir schedule tetap jalan meski irigasi diblokir.
    if (!checkMinimumWater()) {
        return;
    }

    // Check mode
    if (soilHealthMonitor.getMode() == IrrigationMode::TIMER) {
        handleTimerIrrigation();
        return;
    }

    // Normal HUMIDITY mode
    if (sensor.soilADC >= currentIrrigation.dryThreshold) {
        changeState(FertigationState::PRE_IRRIGATION_MIX);
    }
}

void FertigationFSM::handlePreIrrigationMix() {
    if (!stateInitialized) {
        if (!beginMixing()) {
            return;
        }
    }

    if (updateMixing(PRE_IRRIGATION_MIX_TIME)) {
        changeState(FertigationState::PRE_IRRIGATION_VALIDATE);
    }
}

void FertigationFSM::handlePreIrrigationValidate() {
    bool ppmOK = isPPMAcceptableForUse();
    bool phOK  = sensor.ph >= targetMinPH && sensor.ph <= targetMaxPH;

    if (ppmOK && phOK) {
        if (!isPPMInRange()) {
            setWarning(ErrorCode::OVER_PPM);
        } else if (currentError == ErrorCode::OVER_PPM) {
            clearWarning();
        }
        changeState(FertigationState::IRRIGATION);
    } else if (!ppmOK) {
        gotoCorrection();
    } else {
        gotoError(ErrorCode::PH_OUT_OF_RANGE);
    }
}

void FertigationFSM::handleIrrigation() {
    if (!stateInitialized) {
        startIrrigationOutput();
        stateInitialized = true;
        logStateAction("[FSM] Start Irrigation");
    }

    // TIMER mode: berhenti saat slot RTC berakhir (bukan berdasarkan soil ADC)
    if (soilHealthMonitor.getMode() == IrrigationMode::TIMER) {
        uint8_t  hour    = rtcManager.getHour();
        uint8_t  minute  = rtcManager.getMinute();
        uint16_t nowMin  = static_cast<uint16_t>(hour) * 60U + minute;

        bool slotStillActive = false;
        uint8_t numSlots = configManager.getNumIrrigationSlots();
        for (uint8_t i = 0; i < numSlots; i++) {
            IrrigationSlot slot = configManager.getIrrigationSlot(i);
            uint16_t startMin = static_cast<uint16_t>(slot.startHour) * 60U + slot.startMinute;
            uint16_t endMin   = static_cast<uint16_t>(slot.endHour)   * 60U + slot.endMinute;
            if (isMinuteInsideWindow(nowMin, startMin, endMin)) {
                slotStillActive = true;
                break;
            }
        }

        if (!slotStillActive) {
            stopIrrigationOutput();
            _timerSlotRunning   = false;
            _activeSlotIdx      = -1;
            _irrigJustCompleted = true;
            gotoReady();
        }
        return;
    }

    // HUMIDITY mode: berhenti saat tanah cukup lembab
    if (sensor.soilADC <= currentIrrigation.wetThreshold) {
        stopIrrigationOutput();
        _irrigJustCompleted = true; // Set flag for SoilHealthMonitor
        gotoReady();
    }
}

void FertigationFSM::handleTimerIrrigation() {
    uint8_t rtcHour = rtcManager.getHour();
    uint8_t rtcMinute = rtcManager.getMinute();
    uint16_t nowMinute = static_cast<uint16_t>(rtcHour) * 60U + rtcMinute;
    int8_t activeSlot = -1;

    uint8_t numSlots = configManager.getNumIrrigationSlots();
    for (uint8_t i = 0; i < numSlots; i++) {
        IrrigationSlot slot = configManager.getIrrigationSlot(i);
        uint16_t startMinute = static_cast<uint16_t>(slot.startHour) * 60U + slot.startMinute;
        uint16_t endMinute = static_cast<uint16_t>(slot.endHour) * 60U + slot.endMinute;

        if (isMinuteInsideWindow(nowMinute, startMinute, endMinute)) {
            activeSlot = i;
            break;
        }
    }

    if (activeSlot >= 0 && !_timerSlotRunning) {
        // FIX: transisi ke IRRIGATION state supaya event muncul di log MQTT.
        // handleIrrigation() akan otomatis handle TIMER mode (stop by slot end).
        irrigFlow.reset();
        _timerSlotRunning = true;
        _activeSlotIdx = activeSlot;
        changeState(FertigationState::IRRIGATION);
        return;
    }

    // NOTE: end-of-slot handling sekarang dilakukan di handleIrrigation()
    // saat sudah di state IRRIGATION.
}

void FertigationFSM::handleError() {
    if (!stateInitialized) {
        relayManager.allOff();
        logError();
        _errorEntryTime  = millis();
        stateInitialized = true;
    }

    // Auto-recover khusus WATER_TIMEOUT: setelah delay tertentu, coba lagi
    // FILL_WATER (relay sudah OFF karena enterError() memanggil allOff() —
    // ini juga membersihkan sisa SOLENOID_IRRIG yang mungkin bikin fill gagal).
    // Untuk error lain (pH, sensor, dsb.) tetap butuh remote reset via MQTT.
    if (currentError == ErrorCode::WATER_TIMEOUT &&
        waitingRecovery &&
        (millis() - _errorEntryTime) >= WATER_TIMEOUT_AUTO_RECOVER_MS) {
        logStateAction("[FSM] Auto-recover dari WATER_TIMEOUT — retry FILL_WATER");
        waitingRecovery = false;
    }

    if (waitingRecovery == false) {
        recoverFromError();
    }
}

//! LOG HELPERS
const char* FertigationFSM::stateToString(FertigationState s) {
    switch (s) {
        case FertigationState::IDLE:                    return "IDLE";
        case FertigationState::WAIT_DAILY_MIX:          return "WAIT_DAILY_MIX";
        case FertigationState::PREPARE_DAILY_MIX:       return "PREPARE_DAILY_MIX";
        case FertigationState::FILL_WATER:              return "FILL_WATER";
        case FertigationState::PRE_MIX_A:               return "PRE_MIX_A";
        case FertigationState::ADD_NUTRIENT_A:          return "ADD_NUTRIENT_A";
        case FertigationState::MIX_A:                   return "MIX_A";
        case FertigationState::PRE_MIX_B:               return "PRE_MIX_B";
        case FertigationState::ADD_NUTRIENT_B:          return "ADD_NUTRIENT_B";
        case FertigationState::MIX_B:                   return "MIX_B";
        case FertigationState::VALIDATE:                return "VALIDATE";
        case FertigationState::PRE_MIX_CORRECTION:      return "PRE_MIX_CORRECTION";
        case FertigationState::CORRECT_PPM:             return "CORRECT_PPM";
        case FertigationState::CORRECTION_MIX:          return "CORRECTION_MIX";
        case FertigationState::READY:                   return "READY";
        case FertigationState::PRE_IRRIGATION_MIX:      return "PRE_IRRIGATION_MIX";
        case FertigationState::PRE_IRRIGATION_VALIDATE: return "PRE_IRRIGATION_VALIDATE";
        case FertigationState::IRRIGATION:              return "IRRIGATION";
        case FertigationState::ERROR:                   return "ERROR";
        default:                                        return "UNKNOWN";
    }
}

void FertigationFSM::logStateTransition(FertigationState newState) {
    (void)newState;
}

void FertigationFSM::logStateAction(const char* message) {
    (void)message;
}

void FertigationFSM::logRecipe() {
}

void FertigationFSM::logError() {
}

void FertigationFSM::logError(const char* message) {
    (void)message;
}

void FertigationFSM::logRecovery(const char* message) {
    (void)message;
}

void FertigationFSM::logRecovery(
    const char* message,
    FertigationState recoveryState
) {
    (void)message;
    (void)recoveryState;
}

// ============================================================
// STATE DIAGRAM
// //
//   IDLE
//    |
//    | config hardcode sudah loaded
//    v
//   WAIT_DAILY_MIX
//    |
//    | RTC == daily_mix_hour:daily_mix_minute
//    v
//   PREPARE_DAILY_MIX
//    |
//    v
//   FILL_WATER
//    |
//    | tank_volume >= target_fill_volume
//    | level stabil
//    v
//   AMBIL RECIPE HARI INI
//    |
//    v
//   CEK PPM AWAL
//    |
//    +-- PPM sudah cukup?
//    |      |
//    |      +-- YA --> READY
//    |
//    +-- TIDAK
//           |
//           v
//      PRE_MIX_A
//           |
//           v
//      ADD_NUTRIENT_A
//           |
//           v
//      MIX_A
//           |
//           v
//      PRE_MIX_B
//           |
//           v
//      ADD_NUTRIENT_B
//           |
//           v
//      MIX_B
//           |
//           v
//      VALIDATE PPM
//           |
//           +-- PPM cukup --> READY
//           |
//           +-- PPM kurang --> PRE_MIX_CORRECTION
//                                 |
//                                 v
//                            CORRECT_PPM
//                                 |
//                                 v
//                            CORRECTION_MIX
//                                 |
//                                 v
//                            VALIDATE PPM

//   Saat sudah READY:

//   READY
//    |
//    +-- Timer irrigation mode aktif
//    |      |
//    |      +-- RTC masuk window timer --> IRIGASI ON
//    |      |
//    |      +-- RTC keluar window timer --> IRIGASI OFF
//    |
//    +-- Stir sore sesuai jam config
//    |
//    +-- MQTT publish telemetry kalau WiFi ada

//   Jadi besok kalau setelah FILL_WATER PPM sudah masuk target, dia langsung:

//   FILL_WATER
//    |
//   CEK PPM AWAL
//    |
//   READY

//   Tanpa dosing A/B.
  // ============================================================
