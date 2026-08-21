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

// WiFi dikonfigurasi via captive portal (WiFiManager) — tidak ada kredensial hardcoded

#define MQTT_BROKER    "aead004bf5144152b88233f1a1949418.s1.eu.hivemq.cloud"
#define MQTT_PORT      8883
#define MQTT_CLIENT_ID "greenhouse-master-01"
#define MQTT_USER      "greenhouse_esp32"
#define MQTT_PASS      "Kebonagungpanenmelon1"

// =========================================
// MQTT TOPICS — Telemetry (publish)
// =========================================
#define TOPIC_SENSORS       "greenhouse/sensors"
#define TOPIC_FSM_STATE     "greenhouse/fsm/state"
#define TOPIC_RELAY_STATUS  "greenhouse/actuators/status"
#define TOPIC_CONFIG_ACK    "greenhouse/config/ack"

// =========================================
// MQTT TOPICS — Commands (subscribe)
// =========================================
#define TOPIC_CMD           "greenhouse/actuators/cmd"
#define TOPIC_CFG_PPM       "greenhouse/config/ppm"
#define TOPIC_CFG_PH        "greenhouse/config/ph"
#define TOPIC_CFG_RECIPE    "greenhouse/config/recipe"
#define TOPIC_CFG_RECIPE_DELETE "greenhouse/config/recipe/delete"
#define TOPIC_CFG_IRRIG     "greenhouse/config/irrigation"
#define TOPIC_CFG_SYSTEM    "greenhouse/config/system"
#define TOPIC_CFG_SCHEDULE  "greenhouse/config/schedule"
#define TOPIC_CFG_TIMER_IRRIG   "greenhouse/config/timer_irrigation"
#define TOPIC_CFG_MIX_EXT       "greenhouse/config/mix_schedule_ext"
#define TOPIC_CMD_SOIL_RESET    "greenhouse/soil/reset_mode"

// Remote control — restart ESP32 dari web tanpa menyentuh perangkat fisik.
// Payload bebas (misal "restart" atau JSON {"action":"restart"}). Terima pesan
// apa pun di topic ini → ESP.restart() setelah publish ACK.
#define TOPIC_CMD_RESET         "greenhouse/control/reset"
#define TOPIC_CMD_RESET_ACK     "greenhouse/control/reset/ack"

// =========================================
// MQTT TOPICS — Telemetry (publish)
// =========================================
#define TOPIC_SOIL_HEALTH       "greenhouse/soil/health"
#define TOPIC_ALERT_TANK_LOW    "greenhouse/alert/tank_low"

// =========================================
// MQTT TOPIC — Liveness (LWT, retained)
// =========================================
// Satu-satunya sumber kebenaran status koneksi untuk web. Nilainya "online"
// (dipublish retained saat connect sukses) atau "offline" (dipasang sebagai
// Last Will, diterbitkan broker otomatis kalau koneksi hilang tanpa DISCONNECT
// — mati listrik, WiFi putus, ESP crash).
//
// PENTING untuk sisi web: "offline" berarti LINK MQTT putus, BUKAN perangkat
// mati. FSM tetap jalan offline dan relay tidak disentuh saat status offline.
#define TOPIC_STATUS            "greenhouse/status"
#define MQTT_STATUS_ONLINE      "online"
#define MQTT_STATUS_OFFLINE     "offline"

// Interval publish sensor (ms)
#define MQTT_PUBLISH_INTERVAL 1000UL

// =========================================
// Reconnect non-blocking — backoff eksponensial
// =========================================
// WiFi maupun MQTT di-retry dari update(), bukan sekali di begin(). Backoff
// dipakai supaya percobaan yang gagal tidak menahan loop() tiap iterasi.
#define WIFI_RETRY_BASE_MS      5000UL
#define WIFI_RETRY_MAX_MS       60000UL
#define MQTT_RETRY_BASE_MS      5000UL
#define MQTT_RETRY_MAX_MS       60000UL

// Keepalive broker (detik). Default PubSubClient 15 detik terlalu sensitif —
// broker menyatakan klien hilang setelah ~22 detik, sehingga hiccup WiFi
// sebentar saja sudah memicu "offline" palsu di web.
#define MQTT_KEEPALIVE_SEC      60

// Socket timeout (detik). Default PubSubClient 15 detik: satu percobaan
// connect yang gagal memblokir loop() selama itu dan menyendat FSM.
#define MQTT_SOCKET_TIMEOUT_SEC 5

// Ambang alert MQTT putus. Hanya menaikkan WARN di serial + flag —
// TIDAK menyentuh relay. Durasi pompa tetap dibatasi timeout per-state FSM
// (WATER_FILL_TIMEOUT, NUTRIENT_TIMEOUT) supaya gangguan jaringan tidak
// menghentikan fertigasi yang sedang berjalan.
#define MQTT_OFFLINE_ALERT_MS   300000UL

// MQTT sekarang dipakai publish-only. Input FSM/command diambil dari
// Master/data/FSMInputData.h sampai inbound web siap dipakai lagi.
#define MQTT_RECEIVE_ENABLED 0

// Jika 0, firmware tidak akan membuka captive portal/blocking WiFi saat boot.
// FSM tetap jalan offline; MQTT publish dicoba hanya saat WiFi sudah terhubung.
#define WIFI_BLOCKING_PORTAL_ENABLED 1

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

    // True kalau link MQTT sudah putus lebih lama dari MQTT_OFFLINE_ALERT_MS.
    // Murni observasi — pemanggil tidak boleh memakai ini untuk mematikan relay.
    bool isOfflineTooLong() const;

    // Lama link MQTT putus (ms). 0 kalau sedang terhubung.
    unsigned long getOfflineDurationMs() const;

private:
    void connectWiFi();
    void connectMQTT();

    // Dipanggil tiap update() — retry non-blocking dengan backoff.
    void maintainWiFi();
    void maintainMQTT();

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

    // State reconnect WiFi
    unsigned long lastWiFiRetryMs = 0;
    uint8_t       wifiRetryCount  = 0;
    bool          lastWiFiUp      = false;

    // State reconnect MQTT
    unsigned long lastMQTTRetryMs = 0;
    uint8_t       mqttRetryCount  = 0;
    unsigned long lastMqttOkMs    = 0;
    bool          offlineAlertRaised = false;
    FertigationState lastFSMState = FertigationState::IDLE;
    IrrigationMode lastIrrigMode = IrrigationMode::HUMIDITY;

    // Helper: konversi enum ke string
    const char* stateToString(FertigationState state);
    const char* errorCodeToString(ErrorCode error);
};

#endif
