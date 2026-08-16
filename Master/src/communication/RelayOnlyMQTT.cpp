#include "RelayOnlyMQTT.h"
#include <WiFiManager.h>

RelayOnlyMQTT* RelayOnlyMQTT::_instance = nullptr;

RelayOnlyMQTT::RelayOnlyMQTT(RelayManager& relay,
                             RTCManager& rtc,
                             TimerIrrigationScheduler& sched,
                             FlowMeter& a,
                             FlowMeter& b)
    : mqttClient(wifiClient),
      relayManager(relay),
      rtcManager(rtc),
      scheduler(sched),
      flowA(a),
      flowB(b)
{
    _instance = this;
}

void RelayOnlyMQTT::begin() {
    Serial.printf("t=%010lu | INFO  | NETWORK  | wifi=begin mode=relay_only\n", millis());
    connectWiFi();

    mqttClient.setServer(MQTT_BROKER, MQTT_PORT);
    mqttClient.setCallback(onMessage);
    mqttClient.setBufferSize(1024);

    if (WiFi.status() == WL_CONNECTED) {
        connectMQTT();
    }
}

void RelayOnlyMQTT::update() {
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }

    if (!mqttClient.connected()) {
        connectMQTT();
    }

    if (!mqttClient.connected()) {
        return;
    }

    mqttClient.loop();

    unsigned long now = millis();

    if (now - lastRelayPublish >= MQTT_PUBLISH_INTERVAL) {
        lastRelayPublish = now;
        publishRelayStatus();
    }

    if (now - lastTimerPublish >= MQTT_PUBLISH_INTERVAL) {
        lastTimerPublish = now;
        publishTimerStatus();
    }

    if (now - lastFlowPublish >= MQTT_PUBLISH_INTERVAL) {
        lastFlowPublish = now;
        publishFlowStatus();
    }
}

bool RelayOnlyMQTT::isConnected() {
    return mqttClient.connected();
}

// =========================================
// Koneksi
// =========================================
void RelayOnlyMQTT::connectWiFi() {
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

    // Kredensial WiFi disimpan di NVS oleh WiFiManager, jadi portal hanya muncul
    // bila belum pernah dikonfigurasi atau koneksi gagal.
    WiFiManager wm;
    wm.setDebugOutput(false);
    wm.setConfigPortalTimeout(180);
    wm.setTitle("Greenhouse Melon");

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

void RelayOnlyMQTT::connectMQTT() {
    if (mqttClient.connected()) return;
    if (WiFi.status() != WL_CONNECTED) return;

    wifiClient.setInsecure();

    bool ok = mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASS);

    if (!ok) {
        Serial.printf(
            "t=%010lu | WARN  | MQTT     | connected=false state=%d wifi=%s\n",
            millis(),
            mqttClient.state(),
            WiFi.status() == WL_CONNECTED ? "UP" : "DOWN"
        );
        return;
    }

    Serial.printf("t=%010lu | INFO  | MQTT     | connected=true mode=relay_only\n", millis());

    mqttClient.subscribe(TOPIC_CMD);
    mqttClient.subscribe(TOPIC_CMD_RESET);

    clearStaleRetainedTopics();
}

void RelayOnlyMQTT::clearStaleRetainedTopics() {
    // Firmware sistem penuh mem-publish topik ini dengan retained=true, jadi
    // nilai terakhirnya tetap tersimpan di broker meski mode ini tidak pernah
    // mengirimnya lagi. Payload kosong + retained menghapusnya.
    static const char* staleTopics[] = {
        TOPIC_SENSORS,
        TOPIC_FSM_STATE,
        TOPIC_SOIL_HEALTH,
        TOPIC_ALERT_TANK_LOW
    };

    const uint8_t count = sizeof(staleTopics) / sizeof(staleTopics[0]);
    for (uint8_t i = 0; i < count; i++) {
        mqttClient.publish(staleTopics[i], "", true);
    }

    Serial.printf(
        "t=%010lu | INFO  | MQTT     | retained_lama=dibersihkan count=%u\n",
        millis(),
        count
    );
}

// =========================================
// Inbound
// =========================================
void RelayOnlyMQTT::onMessage(const char* topic, byte* payload, unsigned int length) {
    String msg;
    msg.reserve(length);
    for (unsigned int i = 0; i < length; i++) {
        msg += (char)payload[i];
    }

    if (_instance) {
        _instance->handleMessage(topic, msg);
    }
}

void RelayOnlyMQTT::handleMessage(const char* topic, const String& payload) {
    if (String(topic) == TOPIC_CMD_RESET) {
        handleRemoteReset(payload);
        return;
    }

    if (String(topic) != TOPIC_CMD) {
        return;
    }

    JsonDocument cmdDoc;
    if (deserializeJson(cmdDoc, payload)) {
        publishRelayCommandAck(0, "invalid", false, "json_parse_error");
        return;
    }

    if (!executeRelayCommand(cmdDoc)) {
        publishRelayCommandAck(0, "invalid", false, "invalid_payload");
    }
}

bool RelayOnlyMQTT::parseRelayIndex(const JsonDocument& doc, uint8_t& relayIndex) const {
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

RelayChannel RelayOnlyMQTT::relayIndexToChannel(uint8_t relayIndex) const {
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

bool RelayOnlyMQTT::executeRelayCommand(const JsonDocument& doc) {
    uint8_t relayIndex = 0;
    if (!parseRelayIndex(doc, relayIndex)) {
        return false;
    }

    const char* action = doc["action"] | doc["state"] | doc["cmd"] | "toggle";
    RelayChannel channel = relayIndexToChannel(relayIndex);

    // Keadaan relay SEBELUM aksi diterapkan. Dipakai mendeteksi transisi
    // OFF -> ON untuk auto-reset penghitung flow.
    bool wasOn = relayManager.isOn(channel);
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

    resetFlowOnPumpStart(channel, wasOn);

    Serial.printf(
        "t=%010lu | INFO  | CMD      | relay=%u action=%s sumber=manual\n",
        millis(),
        relayIndex,
        action
    );

    publishRelayCommandAck(relayIndex, action, true);
    publishRelayStatus();
    return true;
}

void RelayOnlyMQTT::resetFlowOnPumpStart(RelayChannel channel, bool wasOn) {
    // Sudah menyala sebelumnya — perintah "on" berulang tidak boleh menolkan
    // penghitung di tengah sesi dosis.
    if (wasOn) return;

    // Aksi tidak menghasilkan keadaan ON (mis. "off" atau "all_off").
    if (!relayManager.isOn(channel)) return;

    if (channel == RELAY_PUMP_A) {
        flowA.reset();
        Serial.printf("t=%010lu | INFO  | FLOW     | A=reset alasan=pump_a_start\n", millis());
    } else if (channel == RELAY_PUMP_B) {
        flowB.reset();
        Serial.printf("t=%010lu | INFO  | FLOW     | B=reset alasan=pump_b_start\n", millis());
    }
}

void RelayOnlyMQTT::handleRemoteReset(const String& payload) {
    // Safety: pastikan tidak ada relay yang tertinggal aktif setelah restart trigger
    relayManager.allOff();

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
    mqttClient.loop();  // flush ACK ke broker sebelum restart

    Serial.printf(
        "t=%010lu | WARN  | RESET    | remote_reset triggered reason=%s\n",
        millis(),
        reason.c_str()
    );

    delay(300);   // beri waktu ACK terkirim
    ESP.restart();
}

// =========================================
// Outbound
// =========================================
void RelayOnlyMQTT::publishRelayCommandAck(uint8_t relayIndex, const char* action,
                                           bool success, const char* reason) {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;
    doc["relay"]     = relayIndex;
    doc["action"]    = action;
    doc["status"]    = success ? "ok" : "error";
    if (reason) {
        doc["reason"] = reason;
    }
    doc["timestamp"] = millis();

    char buf[160];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_CONFIG_ACK, buf, false);
}

void RelayOnlyMQTT::publishRelayStatus() {
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
    if (relayManager.isOn(RELAY_MIXER_STIR))     active.add(1);
    if (relayManager.isOn(RELAY_SOLENOID_A))     active.add(2);
    if (relayManager.isOn(RELAY_SOLENOID_B))     active.add(3);
    if (relayManager.isOn(RELAY_SOLENOID_IRRIG)) active.add(4);
    if (relayManager.isOn(RELAY_WATER_INLET))    active.add(5);
    if (relayManager.isOn(RELAY_PUMP_A))         active.add(6);
    if (relayManager.isOn(RELAY_PUMP_B))         active.add(7);
    if (relayManager.isOn(RELAY_PUMP_MIX))       active.add(8);

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_RELAY_STATUS, buf, true);
}

void RelayOnlyMQTT::publishTimerStatus() {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;

    bool rtcOk = rtcManager.isOk();
    doc["rtc"] = rtcOk ? "OK" : "ERROR";

    if (rtcOk) {
        char timeBuf[20];
        snprintf(
            timeBuf, sizeof(timeBuf),
            "%04u-%02u-%02u %02u:%02u",
            rtcManager.getYear(), rtcManager.getMonth(), rtcManager.getDay(),
            rtcManager.getHour(), rtcManager.getMinute()
        );
        doc["time"] = timeBuf;
    } else {
        doc["time"] = nullptr;
    }

    doc["state"]       = scheduler.stateName();
    doc["active_slot"] = scheduler.getActiveSlotIndex();

    uint8_t nextHour   = 0;
    uint8_t nextMinute = 0;
    if (scheduler.getNextSlot(nextHour, nextMinute)) {
        JsonObject next = doc["next_slot"].to<JsonObject>();
        next["hour"]   = nextHour;
        next["minute"] = nextMinute;
    } else {
        doc["next_slot"] = nullptr;
    }

    doc["wifi"]      = WiFi.status() == WL_CONNECTED ? "UP" : "DOWN";
    doc["uptime_ms"] = millis();

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_TIMER_STATUS, buf, true);
}

void RelayOnlyMQTT::publishFlowStatus() {
    JsonDocument doc;
    doc["device_id"] = MQTT_CLIENT_ID;

    // CATATAN: FlowMeter::getFlowRate() menghitung laju sejak panggilan
    // TERAKHIR-nya, jadi ia hanya boleh dipanggil dari satu tempat. Fungsi ini
    // satu-satunya pemanggil; log serial di Main.ino sengaja hanya menampilkan
    // liter, bukan laju, supaya perhitungannya tidak terpotong.
    JsonObject a = doc["a"].to<JsonObject>();
    a["liter"]    = round(flowA.getVolumeLiter() * 1000.0f) / 1000.0f;
    a["rate_lpm"] = round(flowA.getFlowRate() * 100.0f) / 100.0f;
    a["pump_on"]  = relayManager.isOn(RELAY_PUMP_A);

    JsonObject b = doc["b"].to<JsonObject>();
    b["liter"]    = round(flowB.getVolumeLiter() * 1000.0f) / 1000.0f;
    b["rate_lpm"] = round(flowB.getFlowRate() * 100.0f) / 100.0f;
    b["pump_on"]  = relayManager.isOn(RELAY_PUMP_B);

    doc["uptime_ms"] = millis();

    char buf[256];
    serializeJson(doc, buf);
    mqttClient.publish(TOPIC_FLOW_STATUS, buf, true);
}
