#include "ESPNowSender.h"
#include "../config/PinConfig.h"
#include <esp_wifi.h>

static uint8_t currentChannel = 1;
bool ESPNowSender::_lastSendOk = false;

static const uint8_t BROADCAST_MAC[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

bool ESPNowSender::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    if (esp_now_init() != ESP_OK) {
        return false;
    }

    esp_now_register_send_cb(onSendCallback);

    // Register Unicast Peer (Master MAC: AC:A7:04:13:6E:8C)
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, ESPNOW_TARGET_MAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);

    // Register Broadcast Peer as fallback
    memcpy(peerInfo.peer_addr, BROADCAST_MAC, 6);
    esp_now_add_peer(&peerInfo);

    return true;
}

bool ESPNowSender::send(const SoilData& data) {
    esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);

    esp_err_t result = esp_now_send(
        ESPNOW_TARGET_MAC,
        (const uint8_t*)&data,
        sizeof(SoilData)
    );

    if (result != ESP_OK) {
        result = esp_now_send(
            BROADCAST_MAC,
            (const uint8_t*)&data,
            sizeof(SoilData)
        );
    }

    currentChannel++;
    if (currentChannel > 13) {
        currentChannel = 1;
    }

    return (result == ESP_OK);
}

bool ESPNowSender::wasLastSendDelivered() const {
    return _lastSendOk;
}

void ESPNowSender::getTargetMac(char* buffer, size_t size) const {
    if (buffer == nullptr || size == 0) {
        return;
    }

    snprintf(
        buffer,
        size,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        ESPNOW_TARGET_MAC[0],
        ESPNOW_TARGET_MAC[1],
        ESPNOW_TARGET_MAC[2],
        ESPNOW_TARGET_MAC[3],
        ESPNOW_TARGET_MAC[4],
        ESPNOW_TARGET_MAC[5]
    );
}

void ESPNowSender::onSendCallback(
    const uint8_t* mac,
    esp_now_send_status_t status
) {
    _lastSendOk = (status == ESP_NOW_SEND_SUCCESS);
}
