#include <Wire.h>

#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_task_wdt.h>
#include <Preferences.h>

#include "config/PinConfig.h"
#include "config/ConfigManager.h"
#include "TestFlags.h"
#include "data/FSMStateSimulationData.h"
#include "data/FullSystemIntegrationTestData.h"
#include "data/MQTTConfigurationTestData.h"
#include "data/RelayHardwareTestData.h"
#include "data/FlowCalibrationTestData.h"
#include "data/FlowCalibrationTestData.h"
#include "FSMInputData.h"

#include "actuators/RelayManager.h"

#include "communication/MQTTManager.h"

#include "recipe/RecipeManager.h"
#include "recipe/IrrigationRecipe.h"

#include "rtc/RTCManager.h"

#include "sensors/SensorManager.h"

#include "sensors/PHSensor.h"
#include "sensors/TDSSensor.h"
#include "sensors/FlowMeter.h"
#include "sensors/WaterLevel.h"
#include "sensors/WaterTempSensor.h"

#include "fsm/FertigationFSM.h"
#include "fsm/SoilHealthMonitor.h"

#include "communication/ESPNowManager.h"

#include "utils/RecoveryManager.h"

RelayManager relay;

// FLOW METERS
// Gunakan PinConfig.h pins
FlowMeter flowA(FLOW_A_PIN);
FlowMeter flowB(FLOW_B_PIN);
FlowMeter flowIrrig(FLOW_IRRIG_PIN); // Flow sensor irigasi (Timer mode)

// Water Level
WaterLevel waterLevel(
    ULTRASONIC_TRIG,
    ULTRASONIC_ECHO
);

// Water Temperature
WaterTempSensor waterTemp(
    DS18B20_PIN
);

RTCManager rtcManager;

ESPNowManager espNow;

RecoveryManager recovery;

ConfigManager configManager;

SoilHealthMonitor soilHealth(espNow, configManager);

// PH TDS
PHSensor phSensor(PH_PIN);

TDSSensor tdsSensor(TDS_PIN);

// Sensor Manager
SensorManager sensorManager(
    waterTemp,
    phSensor,
    tdsSensor,
    waterLevel,
    espNow,
    flowA,
    flowB
);

// Recipe
RecipeManager recipeManager(configManager);
IrrigationRecipe irrigationRecipe(configManager);

// FSM
FertigationFSM fsm(
    sensorManager,
    relay,
    rtcManager,
    recipeManager,
    irrigationRecipe,
    flowA,
    flowB,
    flowIrrig,
    recovery,
    configManager,
    soilHealth
);

MQTTManager mqtt(relay, configManager, rtcManager, waterLevel, soilHealth, fsm);

static constexpr unsigned long STATUS_LOG_INTERVAL_MS = 5000UL;
static const char* FIRMWARE_BUILD_ID = __DATE__ " " __TIME__;

// Watchdog hardware: reset otomatis kalau loop() menggantung.
// 60 detik, bukan 30 — satu percobaan connect MQTT yang gagal masih bisa
// menahan loop sampai MQTT_SOCKET_TIMEOUT_SEC dan itu bukan hang.
static constexpr uint32_t WDT_TIMEOUT_S = 60;

void clearPreferencesNamespace(const char* ns) {
    Preferences prefs;
    prefs.begin(ns, false);
    prefs.clear();
    prefs.end();
}

void resetAppNVSOnFirmwareChange() {
    Preferences prefs;
    prefs.begin("fw_meta", false);
    String lastBuild = prefs.getString("build", "");

    if (lastBuild == FIRMWARE_BUILD_ID) {
        prefs.end();
        return;
    }

    prefs.end();

    clearPreferencesNamespace("cfg_meta");
    clearPreferencesNamespace("cfg_ppm");
    clearPreferencesNamespace("cfg_ph");
    clearPreferencesNamespace("cfg_recipe");
    clearPreferencesNamespace("cfg_irrig");
    clearPreferencesNamespace("cfg_sys");
    clearPreferencesNamespace("cfg_sched");
    clearPreferencesNamespace("cfg_timer");
    clearPreferencesNamespace("cfg_mix");
    clearPreferencesNamespace("cfg_soil");
    clearPreferencesNamespace("fertigation");

    prefs.begin("fw_meta", false);
    prefs.putString("build", FIRMWARE_BUILD_ID);
    prefs.end();

    logLine("INFO", "NVS", "app_config=reset reason=new_firmware");
}

#if ENABLE_FLOW_CALIBRATION_TEST_A
void runFlowCalibrationTestA() {
    static bool done             = false;
    static bool pulseOn          = false;
    static unsigned long lastPulseTime = 0;
    static unsigned long lastPrint     = 0;

    if (done) {
        if (millis() - lastPrint < 2000) return;
        lastPrint = millis();
        Serial.printf(
            "t=%010lu | INFO  | FLOW_CAL | channel=A status=DONE volume=%.4fL target=%.4fL -- ukur aktual dan bandingkan\n",
            millis(), flowA.getVolumeLiter(), FLOW_CAL_TARGET_A
        );
        return;
    }

    unsigned long pulseDuration = pulseOn ? FLOW_CAL_PULSE_ON_MS_A : FLOW_CAL_PULSE_OFF_MS_A;
    if (millis() - lastPulseTime >= pulseDuration) {
        pulseOn = !pulseOn;
        lastPulseTime = millis();
        if (pulseOn) {
            relay.on(RELAY_PUMP_A);
            relay.on(RELAY_SOLENOID_A);
        } else {
            relay.off(RELAY_PUMP_A);
            relay.off(RELAY_SOLENOID_A);
        }
    }

    if (flowA.getVolumeLiter() >= FLOW_CAL_TARGET_A) {
        relay.off(RELAY_PUMP_A);
        relay.off(RELAY_SOLENOID_A);
        flowA.setCountingEnabled(false);
        done = true;
        Serial.printf(
            "t=%010lu | INFO  | FLOW_CAL | channel=A status=TARGET_REACHED pulses=%lu volume=%.4fL target=%.4fL\n",
            millis(), flowA.pulseCount, flowA.getVolumeLiter(), FLOW_CAL_TARGET_A
        );
        return;
    }

    if (millis() - lastPrint < 1000) return;
    lastPrint = millis();
    Serial.printf(
        "t=%010lu | INFO  | FLOW_CAL | channel=A pulses=%lu volume=%.4fL target=%.4fL rate=%.3fL/min\n",
        millis(), flowA.pulseCount, flowA.getVolumeLiter(), FLOW_CAL_TARGET_A, flowA.getFlowRate()
    );
}
#endif

#if ENABLE_FLOW_CALIBRATION_TEST_B
void runFlowCalibrationTestB() {
    static bool done             = false;
    static bool pulseOn          = false;
    static unsigned long lastPulseTime = 0;
    static unsigned long lastPrint     = 0;

    if (done) {
        if (millis() - lastPrint < 2000) return;
        lastPrint = millis();
        Serial.printf(
            "t=%010lu | INFO  | FLOW_CAL | channel=B status=DONE volume=%.4fL target=%.4fL -- ukur aktual dan bandingkan\n",
            millis(), flowB.getVolumeLiter(), FLOW_CAL_TARGET_B
        );
        return;
    }

    unsigned long pulseDuration = pulseOn ? FLOW_CAL_PULSE_ON_MS_B : FLOW_CAL_PULSE_OFF_MS_B;
    if (millis() - lastPulseTime >= pulseDuration) {
        pulseOn = !pulseOn;
        lastPulseTime = millis();
        if (pulseOn) {
            relay.on(RELAY_PUMP_B);
            relay.on(RELAY_SOLENOID_B);
        } else {
            relay.off(RELAY_PUMP_B);
            relay.off(RELAY_SOLENOID_B);
        }
    }

    if (flowB.getVolumeLiter() >= FLOW_CAL_TARGET_B) {
        relay.off(RELAY_PUMP_B);
        relay.off(RELAY_SOLENOID_B);
        flowB.setCountingEnabled(false);
        done = true;
        Serial.printf(
            "t=%010lu | INFO  | FLOW_CAL | channel=B status=TARGET_REACHED pulses=%lu volume=%.4fL target=%.4fL\n",
            millis(), flowB.pulseCount, flowB.getVolumeLiter(), FLOW_CAL_TARGET_B
        );
        return;
    }

    if (millis() - lastPrint < 1000) return;
    lastPrint = millis();
    Serial.printf(
        "t=%010lu | INFO  | FLOW_CAL | channel=B pulses=%lu volume=%.4fL target=%.4fL rate=%.3fL/min\n",
        millis(), flowB.pulseCount, flowB.getVolumeLiter(), FLOW_CAL_TARGET_B, flowB.getFlowRate()
    );
}
#endif

#if ENABLE_RELAY_HARDWARE_TEST
void runRelayHardwareTest() {
    static uint8_t relayIndex = 0;
    static bool relayOn = false;
    static unsigned long lastChange = 0;

    unsigned long now = millis();
    unsigned long interval = relayOn ? RELAY_TEST_ON_MS : RELAY_TEST_OFF_MS;

    if (now - lastChange < interval) {
        return;
    }

    RelayChannel channel = RELAY_HARDWARE_TEST_SEQUENCE[relayIndex];
    if (relayOn) {
        relay.off(channel);
        relayOn = false;
        relayIndex = (relayIndex + 1) % RELAY_HARDWARE_TEST_COUNT;
    } else {
        relay.on(channel);
        relayOn = true;
    }

    lastChange = now;
}
#endif

const char* stateToString(FertigationState state) {
    switch (state) {
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
        case FertigationState::ESTIMATION_DOSE:         return "ESTIMATION_DOSE";
        case FertigationState::READY:                   return "READY";
        case FertigationState::PRE_IRRIGATION_MIX:      return "PRE_IRRIGATION_MIX";
        case FertigationState::PRE_IRRIGATION_VALIDATE: return "PRE_IRRIGATION_VALIDATE";
        case FertigationState::IRRIGATION:              return "IRRIGATION";
        case FertigationState::ERROR:                   return "ERROR";
        default:                                        return "UNKNOWN";
    }
}

const char* errorToString(ErrorCode error) {
    switch (error) {
        case ErrorCode::NONE:                return "NONE";
        case ErrorCode::WATER_TIMEOUT:       return "WATER_TIMEOUT";
        case ErrorCode::NUTRIENT_A_TIMEOUT:  return "NUTRIENT_A_TIMEOUT";
        case ErrorCode::NUTRIENT_B_TIMEOUT:  return "NUTRIENT_B_TIMEOUT";
        case ErrorCode::MIXER_DRY_RUN:       return "MIXER_DRY_RUN";
        case ErrorCode::RTC_FAILURE:         return "RTC_FAILURE";
        case ErrorCode::PH_SENSOR_FAILURE:   return "PH_SENSOR_FAILURE";
        case ErrorCode::TDS_SENSOR_FAILURE:  return "TDS_SENSOR_FAILURE";
        case ErrorCode::ULTRASONIC_FAILURE:  return "ULTRASONIC_FAILURE";
        case ErrorCode::OVER_PPM:            return "OVER_PPM";
        case ErrorCode::PH_OUT_OF_RANGE:     return "PH_OUT_OF_RANGE";
        case ErrorCode::CORRECTION_FAILED:   return "CORRECTION_FAILED";
        case ErrorCode::WATER_OVERFLOW:      return "WATER_OVERFLOW";
        case ErrorCode::WAITING_FOR_FILL:    return "WAITING_FOR_FILL";
        default:                             return "UNKNOWN_ERROR";
    }
}

const char* modeToString(IrrigationMode mode) {
    return mode == IrrigationMode::TIMER ? "TIMER" : "HUMIDITY";
}

const char* phStatus(float ph) {
    if (ph < 5.9f) return "PH_UP";
    if (ph > 6.5f) return "PH_DOWN";
    return "OK";
}

void logLine(const char* level, const char* component, const char* message) {
    Serial.printf(
        "t=%010lu | %-5s | %-8s | %s\n",
        millis(),
        level,
        component,
        message
    );
}

void logBootStep(const char* component, const char* status) {
    char message[96];
    snprintf(message, sizeof(message), "init=%s", status);
    logLine("INFO", component, message);
}

void logRTCStatus() {
    DateTime now = rtcManager.now();
    char message[128];
    snprintf(
        message,
        sizeof(message),
        "time=%04u-%02u-%02u %02u:%02u:%02u plant_age=%u sync=%s lost_power=%s",
        now.year(),
        now.month(),
        now.day(),
        now.hour(),
        now.minute(),
        now.second(),
        rtcManager.getPlantAgeDays(),
        rtcManager.wasSyncedFromBuildTime() ? "BUILD_TIME" : "RTC",
        rtcManager.lostPower() ? "YES" : "NO"
    );
    logLine("INFO", "RTC", message);
}

void appendRelay(char* buffer, size_t size, bool& first, uint8_t relayIndex) {
    size_t used = strlen(buffer);
    snprintf(
        buffer + used,
        size - used,
        "%s%u",
        first ? "" : ",",
        relayIndex
    );
    first = false;
}

void formatActiveRelays(char* buffer, size_t size) {
    snprintf(buffer, size, "[");
    bool first = true;
    if (relay.isOn(RELAY_MIXER_STIR)) appendRelay(buffer, size, first, 1);
    if (relay.isOn(RELAY_SOLENOID_A)) appendRelay(buffer, size, first, 2);
    if (relay.isOn(RELAY_SOLENOID_B)) appendRelay(buffer, size, first, 3);
    if (relay.isOn(RELAY_SOLENOID_IRRIG)) appendRelay(buffer, size, first, 4);
    if (relay.isOn(RELAY_WATER_INLET)) appendRelay(buffer, size, first, 5);
    if (relay.isOn(RELAY_PUMP_A)) appendRelay(buffer, size, first, 6);
    if (relay.isOn(RELAY_PUMP_B)) appendRelay(buffer, size, first, 7);
    if (relay.isOn(RELAY_PUMP_MIX)) appendRelay(buffer, size, first, 8);
    strncat(buffer, "]", size - strlen(buffer) - 1);
}

void formatSoilRules(char* buffer, size_t size, const SoilRuleFlags& rules) {
    snprintf(
        buffer,
        size,
        "%c%c%c%c",
        rules.heartbeatTimeout ? 'H' : '-',
        rules.outOfRange ? 'O' : '-',
        rules.flatline ? 'F' : '-',
        rules.noResponse ? 'N' : '-'
    );
}

void logSystemStatus(const SensorData& data) {
    char activeRelays[32];
    formatActiveRelays(activeRelays, sizeof(activeRelays));

    SoilRuleFlags rules = soilHealth.getActiveRules();
    char soilRules[8];
    formatSoilRules(soilRules, sizeof(soilRules), rules);

    unsigned long now = millis();
    bool espNowReceived = espNow.hasReceivedData();
    unsigned long espNowAge = espNowReceived ? (now - espNow.getLastReceiveTime()) : 0;
    char espNowSource[18];
    espNow.getLastSenderMac(espNowSource, sizeof(espNowSource));
    uint8_t wifiChannel = 0;
    wifi_second_chan_t wifiSecondChannel = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&wifiChannel, &wifiSecondChannel);

    Serial.printf(
        "t=%010lu | INFO  | STATUS   | state=%s error=%s\n",
        now,
        stateToString(fsm.getState()),
        errorToString(fsm.getError())
    );
    DateTime rtcNow = rtcManager.now();
    Serial.printf(
        "t=%010lu | INFO  | RTC      | time=%04u-%02u-%02u %02u:%02u:%02u plant_age=%u sync=%s lost_power=%s\n",
        now,
        rtcNow.year(),
        rtcNow.month(),
        rtcNow.day(),
        rtcNow.hour(),
        rtcNow.minute(),
        rtcNow.second(),
        rtcManager.getPlantAgeDays(),
        rtcManager.wasSyncedFromBuildTime() ? "BUILD_TIME" : "RTC",
        rtcManager.lostPower() ? "YES" : "NO"
    );
    Serial.printf(
        "t=%010lu | INFO  | NETWORK  | wifi=%s mqtt=%s ip=%s\n",
        now,
        WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
        mqtt.isConnected() ? "UP" : "DOWN",
        WiFi.localIP().toString().c_str()
    );
    Serial.printf(
        "t=%010lu | INFO  | SENSOR   | temp=%.1fC ph=%.2f/%s ppm=%.0f tank=%.1fL humidity_avg=%u\n",
        now,
        data.temperature,
        data.ph,
        phStatus(data.ph),
        data.ppm,
        data.tankVolume,
        data.soilADC
    );
    Serial.printf(
        "t=%010lu | INFO  | FLOW     | A=%.3fL B=%.3fL relays=%s\n",
        now,
        data.flowA,
        data.flowB,
        activeRelays
    );
    Serial.printf(
        "t=%010lu | INFO  | SOIL     | mode=%s health=%u rules=%s master_mac=%s ch=%u espnow_rx=%s rx_count=%lu age_ms=%lu len=%d src=%s s1=%u s2=%u s3=%u s4=%u avg=%u\n",
        now,
        modeToString(soilHealth.getMode()),
        soilHealth.getHealthScore(),
        soilRules,
        WiFi.macAddress().c_str(),
        wifiChannel,
        espNowReceived ? "YES" : "NO",
        (unsigned long)espNow.getPacketCount(),
        espNowAge,
        espNow.getLastPacketSize(),
        espNowSource,
        data.soilSensor1,
        data.soilSensor2,
        data.soilSensor3,
        data.soilSensor4,
        data.soilADC
    );
}

void logStateEvent(FertigationState state, ErrorCode error) {
    char message[96];
    snprintf(
        message,
        sizeof(message),
        "state=%s error=%s",
        stateToString(state),
        errorToString(error)
    );
    logLine(error == ErrorCode::NONE ? "INFO" : "WARN", "FSM", message);
}

void logConnectionEvent(bool mqttConnected) {
    char message[64];
    snprintf(message, sizeof(message), "mqtt=%s", mqttConnected ? "UP" : "DOWN");
    logLine(mqttConnected ? "INFO" : "WARN", "NETWORK", message);
}

void setupWatchdog() {
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    esp_task_wdt_config_t wdtConfig = {
        .timeout_ms     = WDT_TIMEOUT_S * 1000U,
        .idle_core_mask = 0,
        .trigger_panic  = true
    };
    esp_task_wdt_reconfigure(&wdtConfig);
#else
    esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
    esp_task_wdt_add(NULL);

    char message[64];
    snprintf(message, sizeof(message), "watchdog=armed timeout=%us", (unsigned)WDT_TIMEOUT_S);
    logLine("INFO", "WDT", message);
}

void IRAM_ATTR flowAISR() {
    flowA.recordPulseFromISR();
}

void IRAM_ATTR flowBISR() {
    flowB.recordPulseFromISR();
}

void IRAM_ATTR flowIrrigISR() {
    flowIrrig.recordPulseFromISR();
}

void setup() {
    Serial.begin(115200);
    // Tunggu USB CDC siap (wajib untuk ESP32-S3 USB CDC)
    unsigned long t = millis();
    while (!Serial && (millis() - t) < 3000) delay(10);
    logLine("INFO", "BOOT", "serial=ready");

    resetAppNVSOnFirmwareChange();

    configManager.begin();
#if ENABLE_FSM_SIMULATION_TEST
    loadFSMStateSimulationConfiguration(configManager);
#elif ENABLE_FULL_SYSTEM_TEST
    loadFullSystemIntegrationTestConfiguration(configManager);
#elif ENABLE_MQTT_CONFIGURATION_TEST
    loadMQTTConfigurationTestData(configManager);
#else
    loadHardcodedFSMInputData(configManager);
#endif
    logBootStep("CONFIG", configManager.isConfigured() ? "configured" : "waiting_config");

    relay.begin();
    logBootStep("RELAY", "ready");

#if ENABLE_RELAY_HARDWARE_TEST
    logLine("INFO", "TEST", "mode=relay_hardware");
    return;
#endif

#if !ENABLE_FSM_SIMULATION_TEST
    flowA.begin(flowAISR);
    flowB.begin(flowBISR);
    flowIrrig.begin(flowIrrigISR);
    logBootStep("FLOW", "ready");

#if ENABLE_FLOW_CALIBRATION_TEST_A
    flowA.setCountingEnabled(true);
    logLine("INFO", "TEST", "mode=flow_calibration_A target=set relay=pulsing others=OFF");
    return;
#elif ENABLE_FLOW_CALIBRATION_TEST_B
    flowB.setCountingEnabled(true);
    logLine("INFO", "TEST", "mode=flow_calibration_B target=set relay=pulsing others=OFF");
    return;
#endif

    Wire.begin(
        I2C_SDA_PIN,
        I2C_SCL_PIN
    );
    logBootStep("I2C", "ready");

    waterLevel.begin();
    waterLevel.setTankCapacity(configManager.getTankCapacityLiter());
    waterLevel.setTankHeight(configManager.getTankHeightCM());
    waterLevel.setTankDiameter(configManager.getTankDiameterCM());
    logBootStep("WATERLVL", "ready");

    waterTemp.begin();
    logBootStep("TEMP", "ready");

    rtcManager.begin();
    rtcManager.setPlantingDate(configManager.getPlantYear(), configManager.getPlantMonth(), configManager.getPlantDay());
    rtcManager.setDailyMixSchedule(configManager.getDailyMixHour(), configManager.getDailyMixMinute());
    logBootStep("RTC", rtcManager.isOk() ? "ready" : "error");
    if (rtcManager.isOk()) {
        logRTCStatus();
    }

    recovery.begin();
    logBootStep("RECOVERY", "ready");

    // recovery.clear();

    phSensor.begin();
    tdsSensor.begin();
    logBootStep("PH_TDS", "ready");

    soilHealth.begin();
    logBootStep("SOIL", modeToString(soilHealth.getMode()));
#else
    rtcManager.setPlantingDate(configManager.getPlantYear(), configManager.getPlantMonth(), configManager.getPlantDay());
    rtcManager.setDailyMixSchedule(configManager.getDailyMixHour(), configManager.getDailyMixMinute());
    rtcManager.setTestDateTime(2026, 7, 17, configManager.getDailyMixHour(), configManager.getDailyMixMinute());
    logRTCStatus();
    recovery.begin();
    logBootStep("RECOVERY", "ready");
    logBootStep("RTC", "test_ready");
    logBootStep("SOIL", "test_ready");
#endif

    fsm.begin();
    logStateEvent(fsm.getState(), fsm.getError());

#if !ENABLE_ANY_TEST_MODE
    mqtt.begin();
    logConnectionEvent(mqtt.isConnected());
#else
    logLine("INFO", "TEST", "mqtt=disabled");
#endif

#if !ENABLE_FSM_SIMULATION_TEST
    bool espNowOk = espNow.begin();
    logBootStep("ESPNOW", espNowOk ? "ready" : "error");
    Serial.printf(
        "t=%010lu | INFO  | ESPNOW   | master_mac=%s\n",
        millis(),
        WiFi.macAddress().c_str()
    );
#endif

    logLine("INFO", "BOOT", "system=ready");
    delay(5000);

    // Didaftarkan paling akhir: mqtt.begin() bisa membuka captive portal
    // WiFiManager yang blocking sampai 180 detik dan akan memicu reset palsu
    // kalau watchdog sudah aktif duluan.
    setupWatchdog();
}

void loop() {
    // Di awal loop supaya jalur test yang return lebih awal ikut memberi makan
    // watchdog. Aman kalau belum di-arm (mode test keluar dari setup lebih
    // awal): esp_task_wdt_reset() cuma mengembalikan error, tidak crash.
    esp_task_wdt_reset();

#if ENABLE_FLOW_CALIBRATION_TEST_A
    runFlowCalibrationTestA();
    return;
#elif ENABLE_FLOW_CALIBRATION_TEST_B
    runFlowCalibrationTestB();
    return;
#endif

#if ENABLE_RELAY_HARDWARE_TEST
    runRelayHardwareTest();
    return;
#endif

#if ENABLE_FSM_SIMULATION_TEST
    sensorManager.setTestData(getFSMStateSimulationData(fsm.getState()));
#endif

    fsm.update();

    SensorData sensorData = sensorManager.getData();
#if !ENABLE_ANY_TEST_MODE
    mqtt.update(sensorData, fsm.getState(), fsm.getError());
#endif

    static FertigationState lastLoggedState = fsm.getState();
    static ErrorCode lastLoggedError = fsm.getError();
#if !ENABLE_ANY_TEST_MODE
    static bool lastLoggedMqtt = mqtt.isConnected();
#endif
    static unsigned long lastStatusLog = 0;

    FertigationState currentState = fsm.getState();
    ErrorCode currentError = fsm.getError();
#if !ENABLE_ANY_TEST_MODE
    bool mqttConnected = mqtt.isConnected();
#endif

    if (currentState != lastLoggedState || currentError != lastLoggedError) {
        lastLoggedState = currentState;
        lastLoggedError = currentError;
        logStateEvent(currentState, currentError);
    }

#if !ENABLE_ANY_TEST_MODE
    if (mqttConnected != lastLoggedMqtt) {
        lastLoggedMqtt = mqttConnected;
        logConnectionEvent(mqttConnected);
    }
#endif

    if (millis() - lastStatusLog >= STATUS_LOG_INTERVAL_MS) {
        lastStatusLog = millis();
        logSystemStatus(sensorData);
    }
}
