#ifndef RELAY_ONLY_MQTT_H
#define RELAY_ONLY_MQTT_H

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "MQTTConfig.h"
#include "../actuators/RelayManager.h"
#include "../rtc/RTCManager.h"
#include "../scheduler/TimerIrrigationScheduler.h"

// Klien MQTT untuk RELAY_ONLY_MODE.
//
// Subscribe HANYA dua topik: perintah relay dan remote reset. Tidak ada topik
// konfigurasi — seluruh konfigurasi mode ini berasal dari Master/data/.
//
// Publish: status relay (format identik dengan MQTTManager agar web tidak perlu
// diubah) dan status penjadwal timer.
class RelayOnlyMQTT {
public:
    RelayOnlyMQTT(RelayManager& relay, RTCManager& rtc, TimerIrrigationScheduler& scheduler);

    void begin();
    void update();
    bool isConnected();

private:
    void connectWiFi();
    void connectMQTT();

    // Hapus retained payload topik sensor/FSM lama dari broker supaya web tidak
    // menampilkan nilai basi sebagai data hidup.
    void clearStaleRetainedTopics();

    void handleMessage(const char* topic, const String& payload);
    bool executeRelayCommand(const JsonDocument& doc);
    bool parseRelayIndex(const JsonDocument& doc, uint8_t& relayIndex) const;
    RelayChannel relayIndexToChannel(uint8_t relayIndex) const;
    void handleRemoteReset(const String& payload);

    void publishRelayCommandAck(uint8_t relayIndex, const char* action,
                                bool success, const char* reason = nullptr);
    void publishRelayStatus();
    void publishTimerStatus();

    static void onMessage(const char* topic, byte* payload, unsigned int length);
    static RelayOnlyMQTT* _instance;

    WiFiClientSecure          wifiClient;
    PubSubClient              mqttClient;
    RelayManager&             relayManager;
    RTCManager&               rtcManager;
    TimerIrrigationScheduler& scheduler;

    unsigned long lastRelayPublish = 0;
    unsigned long lastTimerPublish = 0;
};

#endif
