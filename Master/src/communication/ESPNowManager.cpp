#include "ESPNowManager.h"
#include <esp_wifi.h>

SoilData ESPNowManager::receivedData;

static constexpr uint8_t ESPNOW_CHANNEL = 1;

volatile bool          ESPNowManager::newData        = false;
volatile unsigned long ESPNowManager::_lastReceiveMs = 0;
volatile uint32_t      ESPNowManager::_packetCount   = 0;
volatile int           ESPNowManager::_lastPacketSize = 0;
uint8_t                ESPNowManager::_lastSenderMac[6] = {0};

bool ESPNowManager::begin() {
    WiFi.mode(WIFI_STA);
    WiFi.setSleep(false);
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (WiFi.status() != WL_CONNECTED) {
        WiFi.disconnect();
        esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    }

    esp_err_t initResult = esp_now_init();
    if(initResult != ESP_OK && initResult != ESP_ERR_ESPNOW_EXIST) {
        return false;
    }

    esp_now_register_recv_cb(onDataRecv);

    return true;
}

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
void ESPNowManager::onDataRecv(
    const esp_now_recv_info_t *esp_now_info,
    const uint8_t *incomingData,
    int len
) {
    const uint8_t *mac = esp_now_info ? esp_now_info->src_addr : nullptr;
#else
void ESPNowManager::onDataRecv(
    const uint8_t *mac,
    const uint8_t *incomingData,
    int len
) {
#endif
    _lastPacketSize = len;

    // Reject packets from incompatible Sleeve firmware.
    if (len != sizeof(receivedData)) {
        return;
    }

    memcpy(
        &receivedData,
        incomingData,
        sizeof(receivedData)
    );

    if (mac) {
        memcpy(_lastSenderMac, mac, 6);
    }
    _lastReceiveMs = millis();
    _packetCount++;
    newData = true;
}

bool ESPNowManager::hasNewData()
const {
    return newData;
}

SoilData ESPNowManager::getData()
const {
    return receivedData;
}

void ESPNowManager::clearFlag() {
    newData = false;
}

unsigned long ESPNowManager::getLastReceiveTime() const {
    return _lastReceiveMs;
}

bool ESPNowManager::hasReceivedData() const {
    return _lastReceiveMs != 0;
}

uint32_t ESPNowManager::getPacketCount() const {
    return _packetCount;
}

int ESPNowManager::getLastPacketSize() const {
    return _lastPacketSize;
}

void ESPNowManager::getLastSenderMac(char* buffer, size_t size) const {
    if (buffer == nullptr || size == 0) {
        return;
    }

    if (!hasReceivedData()) {
        snprintf(buffer, size, "none");
        return;
    }

    snprintf(
        buffer,
        size,
        "%02X:%02X:%02X:%02X:%02X:%02X",
        _lastSenderMac[0],
        _lastSenderMac[1],
        _lastSenderMac[2],
        _lastSenderMac[3],
        _lastSenderMac[4],
        _lastSenderMac[5]
    );
}
