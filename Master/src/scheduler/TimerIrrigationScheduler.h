#ifndef TIMER_IRRIGATION_SCHEDULER_H
#define TIMER_IRRIGATION_SCHEDULER_H

#include <Arduino.h>

#include "../actuators/RelayManager.h"
#include "../rtc/RTCManager.h"
#include "../config/ConfigManager.h"

enum class TimerState : uint8_t {
    IDLE,        // di luar semua slot, relay irigasi tidak disentuh
    IRRIGATING,  // di dalam slot, relay irigasi sudah dinyalakan saat masuk slot
    RTC_ERROR    // jam tidak terbaca — tidak ada slot yang dijalankan
};

// Penjadwal penyiraman berbasis jadwal slot RTC.
//
// Sifatnya EDGE-TRIGGERED: relay hanya disentuh dua kali per slot — saat menit
// mulai dan saat menit selesai. Di antara itu penjadwal tidak menyentuh relay
// sama sekali, sehingga perintah manual operator dari web tidak pernah dilawan.
//
// Modul ini tidak mengenal SensorManager, SoilHealthMonitor, FertigationFSM,
// maupun FlowMeter. Satu-satunya kegagalan yang ditangani adalah RTC.
class TimerIrrigationScheduler {
public:
    TimerIrrigationScheduler(RTCManager& rtc, RelayManager& relay, ConfigManager& config);

    // Menyamakan state internal dengan kondisi awal relay. TIDAK menyentuh relay:
    // RelayManager::begin() yang dipanggil lebih dulu di setup() sudah allOff().
    void begin();

    // Dipanggil tiap loop().
    void update();

    TimerState  getState()           const { return _state; }
    bool        isIrrigating()       const { return _state == TimerState::IRRIGATING; }
    int8_t      getActiveSlotIndex() const { return _activeSlot; }
    const char* stateName()          const;

    // Slot berikutnya hari ini yang menit mulainya > menit sekarang.
    // Return false bila tidak ada lagi hari ini atau RTC bermasalah.
    bool getNextSlot(uint8_t& hour, uint8_t& minute);

private:
    void   startIrrigation(int8_t slotIndex);
    void   stopIrrigation();
    int8_t findActiveSlot(uint16_t nowMinute) const;

    RTCManager&    _rtc;
    RelayManager&  _relay;
    ConfigManager& _config;

    TimerState _state      = TimerState::IDLE;
    int8_t     _activeSlot = -1;   // -1 = tidak ada slot aktif
};

#endif
