#ifndef MQTT_MANAGER_H
#define MQTT_MANAGER_H

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "../sensors/SensorManager.h"
#include "../actuators/RelayManager.h"
#include "../fsm/FertigationState.h"
#include "../config/ConfigManager.h"
#include "../rtc/RTCManager.h"
#include "../sensors/WaterLevel.h"
#include "../utils/ErrorCode.h"
#include "../fsm/SoilHealthMonitor.h"
#include "../fsm/FertigationFSM.h"

#include "MQTTConfig.h"

// MQTT sekarang dipakai publish-only pada mode sistem penuh. Input FSM/command
// diambil dari Master/data/FSMInputData.h sampai inbound web siap dipakai lagi.
// Makro ini sengaja TIDAK ikut pindah ke MQTTConfig.h: ia hanya mengatur
// perilaku MQTTManager, bukan RelayOnlyMQTT (yang selalu subscribe).
#define MQTT_RECEIVE_ENABLED 0

class MQTTManager {
public:
    MQTTManager(RelayManager& relay, ConfigManager& config,
                RTCManager& rtc, WaterLevel& waterLevel,
                SoilHealthMonitor& soilHealth, FertigationFSM& fsm);

    // Panggil di setup()
    void begin();

    // Panggil di loop() — handle reconnect + publish interval
    void update(const SensorData& sensorData, FertigationState fsmState, ErrorCode errorCode = ErrorCode::NONE);

    // Publish FSM state saat berubah (panggil dari FSM atau Main)
    void publishFSMState(FertigationState state, ErrorCode errorCode = ErrorCode::NONE);

    // Publish status relay saat ada perubahan
    void publishRelayStatus();

    // Publish status kesehatan sensor tanah + mode irigasi
    void publishSoilHealth();

    // Publish alert "butuh diisi" (dipanggil dari update() saat FSM set flag di FILL_WATER)
    void publishNeedRefillAlert(float deficitLiter);

    bool isConnected();

private:
    void connectWiFi();
    void connectMQTT();

    void publishSensors(const SensorData& data);
    void publishConfigAck(const char* configName, bool success);

    // Non-static handler — bisa akses member
    void handleMessage(const char* topic, const String& payload);

    // Config handlers
    void handleConfigPPM(const JsonDocument& doc);
    void handleConfigPH(const JsonDocument& doc);
    void handleConfigRecipe(const JsonDocument& doc);
    void handleDeleteRecipe(const JsonDocument& doc);
    void handleConfigIrrigation(const JsonDocument& doc);
    void handleConfigSystem(const JsonDocument& doc);
    void handleConfigSchedule(const JsonDocument& doc);
    void handleConfigTimerIrrigation(const JsonDocument& doc);
    void handleConfigMixScheduleExt(const JsonDocument& doc);
    void handleSoilResetMode(const String& payload);
    void handleRemoteReset(const String& payload);

    // Static callback PubSubClient — gunakan pointer ke instance
    static void onMessage(const char* topic, byte* payload, unsigned int length);
    static MQTTManager* _instance;

    WiFiClientSecure wifiClient;
    PubSubClient     mqttClient;
    RelayManager&    relayManager;
    ConfigManager&   configManager;
    RTCManager&      rtcManager;
    WaterLevel&      waterLevel;
    SoilHealthMonitor& soilHealth;
    FertigationFSM&  fsm;  // untuk polling tank-low alert
    bool executeRelayCommand(const JsonDocument& doc);
    bool parseRelayIndex(const JsonDocument& doc, uint8_t& relayIndex) const;
    RelayChannel relayIndexToChannel(uint8_t relayIndex) const;
    void publishRelayCommandAck(uint8_t relayIndex, const char* action, bool success, const char* reason = nullptr);

    unsigned long lastPublish = 0;
    unsigned long lastSoilPublish = 0;
    unsigned long lastRelayPublish = 0;
    FertigationState lastFSMState = FertigationState::IDLE;
    IrrigationMode lastIrrigMode = IrrigationMode::HUMIDITY;

    // Helper: konversi enum ke string
    const char* stateToString(FertigationState state);
    const char* errorCodeToString(ErrorCode error);
};

#endif
