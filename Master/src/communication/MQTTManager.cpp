#include "MQTTManager.h"
#include <WiFiManager.h>
#include "../config/PinConfig.h"

// Static member definitions
MQTTManager* MQTTManager::_instance      = nullptr;

// =========================================
// retryBackoff() — backoff eksponensial dengan cap
// attempt=1 -> base, 2 -> base*2, 3 -> base*4, ... dibatasi maxMs.
// =========================================
static unsigned long retryBackoff(uint8_t attempt, unsigned long base, unsigned long maxMs) {
    unsigned long backoff = base;
    for (uint8_t i = 1; i < attempt && backoff < maxMs; i++) {
        backoff <<= 1;
    }
    return backoff > maxMs ? maxMs : backoff;
}

// =========================================
// Constructor
// =========================================
MQTTManager::MQTTManager(RelayManager& relay, ConfigManager& config,
                         RTCManager& rtc, WaterLevel& wl,
                         SoilHealthMonitor& sh, FertigationFSM& f)
    : mqttClient(wifiClient),
      relayManager(relay),
      configManager(config),
      rtcManager(rtc),
      waterLevel(wl),
      soilHealth(sh),
      fsm(f)
{
    _instance = this;
}

// =========================================
// begin() — koneksi WiFi + MQTT
// =========================================
void MQTTManager::begin() {
    Serial.printf("t=%010lu | INFO  | NETWORK  | wifi=begin\n", millis());
    connectWiFi();
    Serial.printf(
        "t=%010lu | INFO  | NETWORK  | wifi=%s ip=%s\n",
        millis(),
        WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
        WiFi.localIP().toString().c_str()
    );

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
#if MQTT_RECEIVE_ENABLED
    mqttClient.setCallback(onMessage);
#endif
    mqttClient.setBufferSize(1024);
    mqttClient.setKeepAlive(MQTT_KEEPALIVE_SEC);
    mqttClient.setSocketTimeout(MQTT_SOCKET_TIMEOUT_SEC);

    // Auto-reconnect driver WiFi sebagai lapisan pertama; maintainWiFi()
    // tetap jadi jaring pengaman kalau driver menyerah.
    WiFi.setAutoReconnect(true);

    lastWiFiUp   = (WiFi.status() == WL_CONNECTED);
    lastMqttOkMs = millis();

    if (WiFi.status() == WL_CONNECTED) {
        connectMQTT();
    }
}

// =========================================
// update() — dipanggil tiap loop()
// =========================================
void MQTTManager::update(
    const SensorData& sensorData,
    FertigationState fsmState,
    ErrorCode errorCode
) {
    // Retry WiFi non-blocking. Sebelumnya update() langsung return di sini,
    // sehingga sekali WiFi putus di tengah jalan koneksi tidak pernah pulih
    // sampai perangkat di-reboot — FSM tetap jalan, tapi web tidak menerima
    // apa pun dan menyimpulkan ESP mati.
    maintainWiFi();

    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    // Retry MQTT dengan backoff. Sebelumnya connectMQTT() dipanggil tiap
    // iterasi loop saat terputus; tiap percobaan melakukan TLS handshake yang
    // memblokir sampai MQTT_SOCKET_TIMEOUT_SEC dan menyendat fsm.update().
    maintainMQTT();

    if (!mqttClient.connected()) {
        return;
    }

    mqttClient.loop();

    if (fsmState != lastFSMState) {
        publishFSMState(fsmState, errorCode);
        publishRelayStatus();
        lastFSMState = fsmState;
    }

    IrrigationMode currentIrrigMode = soilHealth.getMode();
    if (currentIrrigMode != lastIrrigMode) {
        publishSoilHealth();
        lastIrrigMode = currentIrrigMode;
    }

    // Polling need-refill alert dari FSM (flag + getter pattern, tanpa coupling balik)
    if (fsm.isNeedRefillAlertPending()) {
        publishNeedRefillAlert(fsm.getRefillDeficit());
        fsm.clearNeedRefillAlert();
    }

    unsigned long now = millis();
    if (now - lastPublish >= MQTT_PUBLISH_INTERVAL) {
        lastPublish = now;
        publishSensors(sensorData);
    }

    if (now - lastSoilPublish >= MQTT_PUBLISH_INTERVAL) {
        lastSoilPublish = now;
        publishSoilHealth();
    }

    if (now - lastRelayPublish >= MQTT_PUBLISH_INTERVAL) {
        lastRelayPublish = now;
        publishRelayStatus();
    }
}

// =========================================
// publishSensors()
// =========================================
void MQTTManager::publishSensors(const SensorData& data) {
    JsonDocument doc;

    doc["device_id"] = MQTT_CLIENT_ID;
    doc["timestamp"] = millis();

    JsonObject sensors   = doc["sensors"].to<JsonObject>();
    sensors["ph"]              = round(data.ph * 100.0f) / 100.0f;
    sensors["ppm"]             = (int)data.ppm;
    sensors["effective_ppm"]   = (int)fsm.getEffectivePPM();
    sensors["estimation_mode"] = fsm.isEstimationMode();
    sensors["temperature"]     = round(data.temperature * 10.0f) / 10.0f;
    float tankCap = configManager.getTankCapacityLiter();
    float fillPct = (tankCap > 0.0f) ? (data.tankVolume / tankCap * 100.0f) : 0.0f;
    if (fillPct > 100.0f) fillPct = 100.0f;
    sensors["water_level"] = round(fillPct * 10.0f) / 10.0f;  // persen isi tangki (0-100)
    sensors["tank_volume"] = round(data.tankVolume * 10.0f) / 10.0f;
    sensors["soil_adc"]    = data.soilADC;

    JsonObject flow = doc["flow"].to<JsonObject>();
    flow["water"] = round(data.flowWater * 100.0f) / 100.0f;
    flow["a"]     = round(data.flowA * 100.0f) / 100.0f;
    flow["b"]     = round(data.flowB * 100.0f) / 100.0f;

    if (fsm.getState() == FertigationState::FILL_WATER) {
        JsonObject fill = doc["fill_progress"].to<JsonObject>();
        float target = configManager.getTargetFillVolume();
        float current = data.tankVolume;
        float remaining = target - current;
        if (remaining < 0.0f) remaining = 0.0f;

        fill["status"] = (current >= target) ? "target_reached" : "filling";
        fill["current_liter"] = round(current * 10.0f) / 10.0f;
        fill["target_liter"] = round(target * 10.0f) / 10.0f;
        float filled = current - fsm.getFillStartVolume();
        if (filled < 0.0f) filled = 0.0f;
        fill["filled_liter"] = round(filled * 10.0f) / 10.0f;
        fill["remaining_liter"] = round(remaining * 10.0f) / 10.0f;
    }

    char buf[512];
    serializeJson(doc, buf);

    mqttClient.publish(TOPIC_SENSORS, buf, true);
}

// =========================================
// publishFSMState()
// =========================================
void MQTTManager::publishFSMState(FertigationState state, ErrorCode errorCode) {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;
    doc["state"]     = stateToString(state);
    doc["timestamp"] = millis();

    if (errorCode != ErrorCode::NONE) {
        doc["error_code"] = errorCodeToString(errorCode);
    }

    char buf[160];
    serializeJson(doc, buf);

    mqttClient.publish(TOPIC_FSM_STATE, buf, true);
}

// =========================================
// publishRelayStatus()
// =========================================
void MQTTManager::publishRelayStatus() {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;
    doc["timestamp"] = millis();

    JsonObject relays = doc["relays"].to<JsonObject>();
    relays["relay_1"] = relayManager.isOn(RELAY_MIXER_STIR);
    relays["relay_2"] = relayManager.isOn(RELAY_SOLENOID_A);
    relays["relay_3"] = relayManager.isOn(RELAY_SOLENOID_B);
    relays["relay_4"] = relayManager.isOn(RELAY_SOLENOID_IRRIG);
    relays["relay_5"] = relayManager.isOn(RELAY_WATER_INLET);
    relays["relay_6"] = relayManager.isOn(RELAY_PUMP_A);
    relays["relay_7"] = relayManager.isOn(RELAY_PUMP_B);
    relays["relay_8"] = relayManager.isOn(RELAY_PUMP_MIX);

    JsonArray active = doc["active_relays"].to<JsonArray>();
    if (relayManager.isOn(RELAY_MIXER_STIR)) active.add(1);
    if (relayManager.isOn(RELAY_SOLENOID_A)) active.add(2);
    if (relayManager.isOn(RELAY_SOLENOID_B)) active.add(3);
    if (relayManager.isOn(RELAY_SOLENOID_IRRIG)) active.add(4);
    if (relayManager.isOn(RELAY_WATER_INLET)) active.add(5);
    if (relayManager.isOn(RELAY_PUMP_A)) active.add(6);
    if (relayManager.isOn(RELAY_PUMP_B)) active.add(7);
    if (relayManager.isOn(RELAY_PUMP_MIX)) active.add(8);

    char buf[256];
    serializeJson(doc, buf);

    mqttClient.publish(TOPIC_RELAY_STATUS, buf, true);
}

// =========================================
// publishConfigAck()
// =========================================
void MQTTManager::publishConfigAck(const char* configName, bool success) {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;
    doc["config"]    = configName;
    doc["status"]    = success ? "ok" : "error";
    doc["timestamp"] = millis();

    char buf[128];
    serializeJson(doc, buf);

    mqttClient.publish(TOPIC_CONFIG_ACK, buf, false);
}

// =========================================
// Command handling
// =========================================
bool MQTTManager::isConnected() {
    return mqttClient.connected();
}

bool MQTTManager::isOfflineTooLong() const {
    return offlineAlertRaised;
}

unsigned long MQTTManager::getOfflineDurationMs() const {
    // lastMqttOkMs di-refresh tiap update() selama link hidup, jadi nilainya
    // mendekati 0 saat terhubung tanpa perlu cek connected() (yang non-const).
    return millis() - lastMqttOkMs;
}

// =========================================
// onMessage() — static callback PubSubClient
// Delegate ke non-static handleMessage()
// =========================================
void MQTTManager::onMessage(
    const char* topic,
    byte* payload,
    unsigned int length
) {
    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    if (_instance) {
        _instance->handleMessage(topic, msg);
    }
}

// =========================================
// handleMessage() — non-static, bisa akses
// configManager, rtcManager, waterLevel
// =========================================
void MQTTManager::handleMessage(const char* topic, const String& payload) {
    if (String(topic) == TOPIC_CMD) {
        JsonDocument cmdDoc;
        DeserializationError cmdErr = deserializeJson(cmdDoc, payload);
        if (cmdErr) {
            publishRelayCommandAck(0, "invalid", false, "json_parse_error");
            return;
        }

        if (!executeRelayCommand(cmdDoc)) {
            publishRelayCommandAck(0, "invalid", false, "invalid_payload");
        }
        return;
    }

    if (String(topic) == TOPIC_CMD_SOIL_RESET) {
        handleSoilResetMode(payload);
        return;
    }

    if (String(topic) == TOPIC_CMD_RESET) {
        handleRemoteReset(payload);
        return;
    }

    // Config topics — parse JSON
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
        publishConfigAck(topic, false);
        return;
    }

    const String t = String(topic);
    if      (t == TOPIC_CFG_PPM)      handleConfigPPM(doc);
    else if (t == TOPIC_CFG_PH)       handleConfigPH(doc);
    else if (t == TOPIC_CFG_RECIPE)   handleConfigRecipe(doc);
    else if (t == TOPIC_CFG_RECIPE_DELETE) handleDeleteRecipe(doc);
    else if (t == TOPIC_CFG_IRRIG)    handleConfigIrrigation(doc);
    else if (t == TOPIC_CFG_SYSTEM)   handleConfigSystem(doc);
    else if (t == TOPIC_CFG_SCHEDULE) handleConfigSchedule(doc);
    else if (t == TOPIC_CFG_TIMER_IRRIG) handleConfigTimerIrrigation(doc);
    else if (t == TOPIC_CFG_MIX_EXT)  handleConfigMixScheduleExt(doc);
}

bool MQTTManager::parseRelayIndex(const JsonDocument& doc, uint8_t& relayIndex) const {
    if (doc["relay"].is<uint8_t>()) {
        relayIndex = doc["relay"].as<uint8_t>();
        return relayManager.isValidRelayIndex(relayIndex);
    }

    if (doc["relay_id"].is<uint8_t>()) {
        relayIndex = doc["relay_id"].as<uint8_t>();
        return relayManager.isValidRelayIndex(relayIndex);
    }

    return false;
}

RelayChannel MQTTManager::relayIndexToChannel(uint8_t relayIndex) const {
    switch (relayIndex) {
        case 1: return RELAY_MIXER_STIR;
        case 2: return RELAY_SOLENOID_A;
        case 3: return RELAY_SOLENOID_B;
        case 4: return RELAY_SOLENOID_IRRIG;
        case 5: return RELAY_WATER_INLET;
        case 6: return RELAY_PUMP_A;
        case 7: return RELAY_PUMP_B;
        case 8: return RELAY_PUMP_MIX;
        default: return RELAY_MIXER_STIR;
    }
}

void MQTTManager::publishRelayCommandAck(uint8_t relayIndex, const char* action, bool success, const char* reason) {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;
    doc["relay"] = relayIndex;
    doc["action"] = action;
    doc["status"] = success ? "ok" : "error";
    if (reason) {
        doc["reason"] = reason;
    }
    doc["timestamp"] = millis();

    char buf[160];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_CONFIG_ACK, buf, false);
}

bool MQTTManager::executeRelayCommand(const JsonDocument& doc) {
    uint8_t relayIndex = 0;
    if (!parseRelayIndex(doc, relayIndex)) {
        return false;
    }

    const char* action = doc["action"] | doc["state"] | doc["cmd"] | "toggle";
    RelayChannel channel = relayIndexToChannel(relayIndex);
    bool ok = true;

    if (strcmp(action, "on") == 0) {
        relayManager.on(channel);
    } else if (strcmp(action, "off") == 0) {
        relayManager.off(channel);
    } else if (strcmp(action, "toggle") == 0) {
        if (relayManager.isOn(channel)) relayManager.off(channel);
        else relayManager.on(channel);
    } else if (strcmp(action, "all_off") == 0) {
        relayManager.allOff();
    } else {
        ok = false;
    }

    if (!ok) {
        publishRelayCommandAck(relayIndex, action, false, "unknown_action");
        return false;
    }

    publishRelayCommandAck(relayIndex, action, true);
    publishRelayStatus();
    return true;
}

// =========================================
// Config Handlers
// =========================================

void MQTTManager::handleConfigPPM(const JsonDocument& doc) {
    float tolerance = doc["ppm_tolerance"] | configManager.getPPMTolerance();
    float initA     = doc["initial_a"]     | configManager.getInitialNutrientA();
    float initB     = doc["initial_b"]     | configManager.getInitialNutrientB();

    configManager.setPPMConfig(tolerance, initA, initB);
    publishConfigAck("ppm", true);
}

void MQTTManager::handleConfigPH(const JsonDocument& doc) {
    float minPH = doc["min_ph"] | configManager.getDefaultMinPH();
    float maxPH = doc["max_ph"] | configManager.getDefaultMaxPH();

    configManager.setPHConfig(minPH, maxPH);
    publishConfigAck("ph", true);
}

void MQTTManager::handleConfigRecipe(const JsonDocument& doc) {
    JsonArrayConst arr = doc["stages"].as<JsonArrayConst>();
    if (arr.isNull()) {
        publishConfigAck("recipe", false);
        return;
    }

    RecipeStageConfig stages[MAX_RECIPE_STAGES];
    uint8_t count = 0;

    for (JsonObjectConst s : arr) {
        if (count >= MAX_RECIPE_STAGES) break;
        stages[count].maxAgeDays = s["max_age_days"] | (uint16_t)0;
        stages[count].targetPPM  = s["target_ppm"]   | 0.0f;
        stages[count].minPH      = s["min_ph"]        | configManager.getDefaultMinPH();
        stages[count].maxPH      = s["max_ph"]        | configManager.getDefaultMaxPH();
        count++;
    }

    if (count == 0) {
        publishConfigAck("recipe", false);
        return;
    }

    configManager.setRecipeStages(stages, count);
    publishConfigAck("recipe", true);
}

void MQTTManager::handleDeleteRecipe(const JsonDocument& doc) {
    bool deleted = false;

    if (doc["index"].is<uint8_t>()) {
        deleted = configManager.deleteRecipeStage(doc["index"].as<uint8_t>());
    } else if (doc["stage"].is<uint8_t>()) {
        uint8_t stage = doc["stage"].as<uint8_t>();
        if (stage > 0) {
            deleted = configManager.deleteRecipeStage(stage - 1);
        }
    } else if (doc["max_age_days"].is<uint16_t>()) {
        deleted = configManager.deleteRecipeStageByMaxAge(doc["max_age_days"].as<uint16_t>());
    }

    publishConfigAck("recipe/delete", deleted);
}

void MQTTManager::handleConfigIrrigation(const JsonDocument& doc) {
    JsonArrayConst arr = doc["stages"].as<JsonArrayConst>();
    if (arr.isNull()) {
        publishConfigAck("irrigation", false);
        return;
    }

    IrrigationStageConfig stages[MAX_IRRIGATION_STAGES];
    uint8_t count = 0;

    for (JsonObjectConst s : arr) {
        if (count >= MAX_IRRIGATION_STAGES) break;
        stages[count].maxAgeDays    = s["max_age_days"]   | (uint16_t)0;
        stages[count].dryThreshold  = s["dry_threshold"]  | (uint16_t)3900;
        stages[count].wetThreshold  = s["wet_threshold"]  | (uint16_t)3650;
        count++;
    }

    if (count == 0) {
        publishConfigAck("irrigation", false);
        return;
    }

    configManager.setIrrigationStages(stages, count);
    publishConfigAck("irrigation", true);
}

void MQTTManager::handleConfigSystem(const JsonDocument& doc) {
    uint16_t plants      = doc["total_plants"]              | configManager.getTotalPlants();
    float    maxCons     = doc["max_consumption_per_plant"] | configManager.getMaxConsumptionPerPlant();
    float    targetFill  = doc["target_fill_volume"]        | configManager.getTargetFillVolume();
    float    tankCap     = doc["tank_capacity_liter"]       | configManager.getTankCapacityLiter();
    float    tankHeight  = doc["tank_height_cm"]            | configManager.getTankHeightCM();
    float    tankDiam    = doc["tank_diameter_cm"]          | configManager.getTankDiameterCM();
    if (tankHeight <= 0.0f) tankHeight = 45.0f;
    if (tankDiam <= 0.0f) tankDiam = 20.6f;
    if (tankCap > 0.0f && targetFill > tankCap) targetFill = tankCap;

    configManager.setSystemConfig(plants, maxCons, targetFill, tankCap, tankHeight, tankDiam);

    // Propagate ke WaterLevel sensor langsung
    waterLevel.setTankCapacity(tankCap);
    waterLevel.setTankHeight(tankHeight);
    waterLevel.setTankDiameter(tankDiam);

    publishConfigAck("system", true);
}

void MQTTManager::handleConfigSchedule(const JsonDocument& doc) {
    uint16_t year   = doc["plant_year"]      | configManager.getPlantYear();
    uint8_t  month  = doc["plant_month"]     | configManager.getPlantMonth();
    uint8_t  day    = doc["plant_day"]       | configManager.getPlantDay();
    uint8_t  hour   = doc["daily_mix_hour"]  | configManager.getDailyMixHour();
    uint8_t  minute = doc["daily_mix_minute"]| configManager.getDailyMixMinute();

    configManager.setScheduleConfig(year, month, day, hour, minute);

    // Propagate ke RTCManager langsung
    rtcManager.setPlantingDate(year, month, day);
    rtcManager.setDailyMixSchedule(hour, minute);

    publishConfigAck("schedule", true);
}

// =========================================
// maintainWiFi() — reconnect non-blocking, dipanggil tiap update()
//
// TIDAK memakai WiFiManager: wm.autoConnect() blocking sampai 180 detik dan
// akan menahan fsm.update() selama itu — berbahaya kalau pompa sedang menyala.
// WiFi.reconnect() memakai kredensial yang sudah tersimpan di NVS dan langsung
// kembali (esp_wifi_disconnect + esp_wifi_connect, keduanya asinkron).
// =========================================
void MQTTManager::maintainWiFi() {
    bool up = (WiFi.status() == WL_CONNECTED);

    if (up != lastWiFiUp) {
        lastWiFiUp = up;
        Serial.printf(
            "t=%010lu | %s | WIFI     | link=%s ip=%s\n",
            millis(),
            up ? "INFO " : "WARN ",
            up ? "UP" : "DOWN",
            WiFi.localIP().toString().c_str()
        );
    }

    if (up) {
        wifiRetryCount = 0;
        return;
    }

    unsigned long now = millis();
    if (wifiRetryCount > 0 &&
        (now - lastWiFiRetryMs) < retryBackoff(wifiRetryCount, WIFI_RETRY_BASE_MS, WIFI_RETRY_MAX_MS)) {
        return;
    }

    lastWiFiRetryMs = now;
    if (wifiRetryCount < 255) wifiRetryCount++;

    // reconnect() gagal kalau config driver sudah hilang; begin() tanpa argumen
    // membaca ulang kredensial dari NVS sebagai fallback.
    if (!WiFi.reconnect()) {
        WiFi.begin();
    }

    Serial.printf(
        "t=%010lu | WARN  | WIFI     | reconnect=attempt n=%u next_retry_in=%lums\n",
        now,
        wifiRetryCount,
        retryBackoff(wifiRetryCount, WIFI_RETRY_BASE_MS, WIFI_RETRY_MAX_MS)
    );
}

// =========================================
// maintainMQTT() — reconnect berbackoff + alert offline
//
// Alert tidak menyentuh relay: MQTT di sini publish-only (telemetri), dan FSM
// memang dirancang jalan offline. Memutus fertigasi karena telemetri hilang
// berarti gangguan jaringan menghentikan penyiraman. Batas durasi pompa tetap
// dipegang timeout per-state di FertigationFSM.
// =========================================
void MQTTManager::maintainMQTT() {
    unsigned long now = millis();

    if (mqttClient.connected()) {
        mqttRetryCount = 0;
        lastMqttOkMs   = now;
        if (offlineAlertRaised) {
            offlineAlertRaised = false;
            Serial.printf("t=%010lu | INFO  | MQTT     | offline_alert=cleared\n", now);
        }
        return;
    }

    if (!offlineAlertRaised && (now - lastMqttOkMs) >= MQTT_OFFLINE_ALERT_MS) {
        offlineAlertRaised = true;
        Serial.printf(
            "t=%010lu | WARN  | MQTT     | offline_for=%lus relay=UNTOUCHED fsm=RUNNING\n",
            now,
            (now - lastMqttOkMs) / 1000UL
        );
    }

    if (mqttRetryCount > 0 &&
        (now - lastMQTTRetryMs) < retryBackoff(mqttRetryCount, MQTT_RETRY_BASE_MS, MQTT_RETRY_MAX_MS)) {
        return;
    }

    lastMQTTRetryMs = now;
    if (mqttRetryCount < 255) mqttRetryCount++;

    connectMQTT();
}

// =========================================
// connectWiFi() — via WiFiManager captive portal
// Kredensial disimpan di NVS oleh WiFiManager.
// Jika belum pernah dikonfigurasi / koneksi gagal, buka AP "AgroTech Melon"
// dengan captive portal bertema hijau-putih selama 180 detik.
// =========================================
void MQTTManager::connectWiFi() {
    if (WiFi.status() == WL_CONNECTED) return;

#if !WIFI_BLOCKING_PORTAL_ENABLED
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    unsigned long startMs = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startMs < 3000UL) {
        delay(100);
    }

    Serial.printf(
        "t=%010lu | %s | WIFI     | connected=%s status=%d ip=%s mode=non_blocking\n",
        millis(),
        WiFi.status() == WL_CONNECTED ? "INFO " : "WARN ",
        WiFi.status() == WL_CONNECTED ? "true" : "false",
        WiFi.status(),
        WiFi.localIP().toString().c_str()
    );
    return;
#endif

    static const char* customCSS = R"rawhtml(
<style>
  body { background-color: #f1f8e9; font-family: sans-serif; color: #1b5e20; margin: 0; padding: 0; }
  h1   { color: #2e7d32; }
  h3   { color: #388e3c; }
  .c   { background: #ffffff; border-radius: 8px; padding: 16px; }
  input[type=text], input[type=password] {
    border: 1px solid #4caf50; border-radius: 4px;
    padding: 8px; width: 100%; box-sizing: border-box;
  }
  input[type=submit] {
    background-color: #4caf50; color: white;
    border: none; border-radius: 4px; padding: 10px 20px;
    cursor: pointer; width: 100%;
  }
  input[type=submit]:hover { background-color: #2e7d32; }
  a { color: #2e7d32; }
  .q { background-color: #4caf50; }
</style>
)rawhtml";

    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setCustomHeadElement(customCSS);
    wm.setConfigPortalTimeout(180);
    wm.setTitle("\xF0\x9F\x8C\xB1 Greenhouse Melon");  // 🌱 Greenhouse Melon

    Serial.printf("t=%010lu | INFO  | WIFI     | portal=ready ssid=\"AgroTech Melon\"\n", millis());
    bool connected = wm.autoConnect("AgroTech Melon", "KebonagungXUPNVYK");
    Serial.printf(
        "t=%010lu | %s | WIFI     | connected=%s status=%d ip=%s\n",
        millis(),
        connected ? "INFO " : "WARN ",
        connected ? "true" : "false",
        WiFi.status(),
        WiFi.localIP().toString().c_str()
    );
}

// =========================================
// connectMQTT()
// =========================================
void MQTTManager::connectMQTT() {
    if (mqttClient.connected()) return;
    if (WiFi.status() != WL_CONNECTED) return;

    wifiClient.setInsecure();

    // Last Will: broker menerbitkan "offline" retained ke TOPIC_STATUS kalau
    // koneksi hilang tanpa DISCONNECT. Web membaca satu topic ini untuk
    // liveness, bukan menebak dari umur pesan greenhouse/sensors.
    bool ok = mqttClient.connect(
        MQTT_CLIENT_ID,
        MQTT_USER,
        MQTT_PASS,
        TOPIC_STATUS,
        1,      // willQos
        true,   // willRetain
        MQTT_STATUS_OFFLINE
    );

    if (ok) {
        mqttClient.publish(TOPIC_STATUS, MQTT_STATUS_ONLINE, true);
        Serial.printf("t=%010lu | INFO  | MQTT     | connected=true status=online\n", millis());
#if MQTT_RECEIVE_ENABLED
        mqttClient.subscribe(TOPIC_CMD);
        mqttClient.subscribe(TOPIC_CFG_PPM);
        mqttClient.subscribe(TOPIC_CFG_PH);
        mqttClient.subscribe(TOPIC_CFG_RECIPE);
        mqttClient.subscribe(TOPIC_CFG_RECIPE_DELETE);
        mqttClient.subscribe(TOPIC_CFG_IRRIG);
        mqttClient.subscribe(TOPIC_CFG_SYSTEM);
        mqttClient.subscribe(TOPIC_CFG_SCHEDULE);
        mqttClient.subscribe(TOPIC_CFG_TIMER_IRRIG);
        mqttClient.subscribe(TOPIC_CFG_MIX_EXT);
        mqttClient.subscribe(TOPIC_CMD_SOIL_RESET);
        mqttClient.subscribe(TOPIC_CMD_RESET);
#else
        Serial.printf("t=%010lu | INFO  | MQTT     | mode=publish_only receive=disabled\n", millis());
#endif
    } else {
        Serial.printf(
            "t=%010lu | WARN  | MQTT     | connected=false state=%d wifi=%s\n",
            millis(),
            mqttClient.state(),
            WiFi.status() == WL_CONNECTED ? "UP" : "DOWN"
        );
    }
}

// =========================================
// stateToString()
// =========================================
const char* MQTTManager::stateToString(FertigationState state) {
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

// =========================================
// errorCodeToString()
// =========================================
const char* MQTTManager::errorCodeToString(ErrorCode error) {
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

// =========================================
// handleConfigTimerIrrigation()
// =========================================
void MQTTManager::handleConfigTimerIrrigation(const JsonDocument& doc) {
    JsonArrayConst arr = doc["slots"].as<JsonArrayConst>();
    
    if (arr.isNull()) {
        publishConfigAck("timer_irrigation", false);
        return;
    }

    IrrigationSlot slots[MAX_IRRIG_SLOTS];
    uint8_t count = 0;

    for (JsonObjectConst s : arr) {
        if (count >= MAX_IRRIG_SLOTS) break;
        slots[count].startHour = s["start_hour"] | s["hour"] | (uint8_t)0;
        slots[count].startMinute = s["start_minute"] | s["minute"] | (uint8_t)0;
        slots[count].endHour = s["end_hour"] | slots[count].startHour;
        slots[count].endMinute = s["end_minute"] | slots[count].startMinute;
        count++;
    }

    configManager.setTimerIrrigationConfig(slots, count);
    publishConfigAck("timer_irrigation", true);
}

// =========================================
// handleConfigMixScheduleExt()
// Topic: greenhouse/config/mix_schedule_ext
// Payload contoh:
// {
//   "stir_evening_hour":    18,
//   "stir_evening_minute":  0,
//   "stir_duration_seconds": 300
// }
// Field lama "mix_interval_days" dan "per_plant_need_liter" diabaikan dengan aman.
// =========================================
void MQTTManager::handleConfigMixScheduleExt(const JsonDocument& doc) {
    uint8_t  stirEvHour = doc["stir_evening_hour"]     | configManager.getStirEveningHour();
    uint8_t  stirEvMin  = doc["stir_evening_minute"]   | configManager.getStirEveningMinute();
    uint32_t stirDurSec = doc["stir_duration_seconds"] | (uint32_t)(configManager.getStirDurationMs() / 1000UL);

    configManager.setStirSchedule(stirEvHour, stirEvMin, stirDurSec * 1000UL);
    publishConfigAck("mix_schedule_ext", true);
}

// =========================================
// handleRemoteReset() — restart ESP32 dari web
// Matikan semua relay dulu (aman untuk aktuator), publish ACK, lalu ESP.restart().
// Payload boleh apa pun; jika JSON valid dengan field "reason" akan diteruskan ke ACK.
// =========================================
void MQTTManager::handleRemoteReset(const String& payload) {
    // Safety: pastikan tidak ada relay yang tertinggal aktif setelah restart trigger
    relayManager.allOff();

    // Coba parse JSON untuk ambil reason (opsional)
    String reason = "manual";
    JsonDocument doc;
    if (deserializeJson(doc, payload) == DeserializationError::Ok
        && doc["reason"].is<const char*>()) {
        reason = String(doc["reason"].as<const char*>());
    } else if (payload.length() > 0 && payload.length() < 64) {
        reason = payload;
    }

    JsonDocument ack;
    ack["device_id"] = MQTT_CLIENT_ID;
    ack["status"]    = "restarting";
    ack["reason"]    = reason;
    ack["timestamp"] = millis();

    char buf[192];
    serializeJson(ack, buf);
    mqttClient.publish(TOPIC_CMD_RESET_ACK, buf, false);

    // Restart terencana: tandai offline sendiri supaya web tidak membacanya
    // sebagai crash. Tanpa ini, LWT yang menerbitkannya dan keduanya jadi
    // tak terbedakan.
    mqttClient.publish(TOPIC_STATUS, MQTT_STATUS_OFFLINE, true);
    mqttClient.loop();  // flush ACK + status ke broker sebelum restart

    Serial.printf(
        "t=%010lu | WARN  | RESET    | remote_reset triggered reason=%s\n",
        millis(),
        reason.c_str()
    );

    delay(300);   // beri waktu ACK terkirim
    ESP.restart();
}

// =========================================
// handleSoilResetMode()
// =========================================
void MQTTManager::handleSoilResetMode(const String& payload) {
    JsonDocument modeDoc;
    if (deserializeJson(modeDoc, payload) == DeserializationError::Ok
        && modeDoc["mode"].is<const char*>()) {
        const char* mode = modeDoc["mode"].as<const char*>();
        if (strcmp(mode, "TIMER") == 0) {
            soilHealth.switchToTimerMode();
        } else {
            soilHealth.resetToHumidityMode();
        }
    } else {
        soilHealth.resetToHumidityMode();
    }
    publishSoilHealth();
}

// =========================================
// publishNeedRefillAlert()
// Dipanggil dari update() saat FSM set _needRefillAlertPending di FILL_WATER.
// =========================================
void MQTTManager::publishNeedRefillAlert(float deficitLiter) {
    JsonDocument doc;
    doc["device_id"]      = MQTT_CLIENT_ID;
    doc["status"]         = (waterLevel.getVolumeLiter() >= configManager.getTargetFillVolume())
                                ? "target_reached"
                                : "filling";
    doc["tank_volume"]    = round(waterLevel.getVolumeLiter() * 10.0f) / 10.0f;
    doc["target_volume"]  = round(configManager.getTargetFillVolume() * 10.0f) / 10.0f;
    doc["deficit_liter"]  = round(deficitLiter * 100.0f) / 100.0f;
    doc["timestamp"]      = millis();

    char buf[192];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_ALERT_TANK_LOW, buf, false);
}

// =========================================
// publishSoilHealth()
// =========================================
void MQTTManager::publishSoilHealth() {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;
    doc["timestamp"] = millis();

    doc["mode"] = (soilHealth.getMode() == IrrigationMode::TIMER) ? "TIMER" : "HUMIDITY";
    doc["health_score"] = soilHealth.getHealthScore();

    SoilRuleFlags rules = soilHealth.getActiveRules();
    JsonObject r = doc["active_rules"].to<JsonObject>();
    r["heartbeat_timeout"] = rules.heartbeatTimeout;
    r["out_of_range"]      = rules.outOfRange;
    r["flatline"]          = rules.flatline;
    r["no_response"]       = rules.noResponse;

    char buf[256];
    serializeJson(doc, buf);

    mqttClient.publish(TOPIC_SOIL_HEALTH, buf, true);
}
