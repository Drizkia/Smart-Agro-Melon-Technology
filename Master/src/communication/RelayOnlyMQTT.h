#ifndef RELAY_ONLY_MQTT_H
#define RELAY_ONLY_MQTT_H

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "MQTTConfig.h"
#include "../actuators/RelayManager.h"
#include "../rtc/RTCManager.h"
#include "../sensors/FlowMeter.h"
#include "../scheduler/TimerIrrigationScheduler.h"

// Klien MQTT untuk RELAY_ONLY_MODE.
//
// Subscribe HANYA dua topik: perintah relay dan remote reset. Tidak ada topik
// konfigurasi — seluruh konfigurasi mode ini berasal dari Master/data/.
//
// Publish: status relay (format identik dengan MQTTManager agar web tidak perlu
// diubah), status penjadwal timer, dan pembacaan flow meter nutrisi A & B.
//
// Flow meter di sini murni pembacaan: ia tidak pernah bisa menggerakkan relay.
// Hubungan satu arahnya cuma auto-reset — penghitung liter ditolkan saat pompa
// yang bersangkutan berpindah dari OFF ke ON, supaya angkanya berarti
// "liter sesi dosis ini", bukan akumulasi sejak boot.
class RelayOnlyMQTT {
public:
    RelayOnlyMQTT(RelayManager& relay, RTCManager& rtc, TimerIrrigationScheduler& scheduler,
                  FlowMeter& flowA, FlowMeter& flowB);

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

    // Tolkan penghitung flow bila channel ini baru saja berpindah OFF -> ON.
    // wasOn = keadaan relay SEBELUM aksi diterapkan.
    void resetFlowOnPumpStart(RelayChannel channel, bool wasOn);

    void publishRelayCommandAck(uint8_t relayIndex, const char* action,
                                bool success, const char* reason = nullptr);
    void publishRelayStatus();
    void publishTimerStatus();
    void publishFlowStatus();

    static void onMessage(const char* topic, byte* payload, unsigned int length);
    static RelayOnlyMQTT* _instance;

    WiFiClientSecure          wifiClient;
    PubSubClient              mqttClient;
    RelayManager&             relayManager;
    RTCManager&               rtcManager;
    TimerIrrigationScheduler& scheduler;
    FlowMeter&                flowA;
    FlowMeter&                flowB;

    unsigned long lastRelayPublish = 0;
    unsigned long lastTimerPublish = 0;
    unsigned long lastFlowPublish  = 0;
};

#endif
