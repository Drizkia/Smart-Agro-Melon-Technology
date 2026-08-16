#include "TimerIrrigationScheduler.h"
#include "../utils/TimeWindow.h"

TimerIrrigationScheduler::TimerIrrigationScheduler(RTCManager& rtc,
                                                   RelayManager& relay,
                                                   ConfigManager& config)
    : _rtc(rtc),
      _relay(relay),
      _config(config)
{}

void TimerIrrigationScheduler::begin() {
    _state      = TimerState::IDLE;
    _activeSlot = -1;

    uint8_t slotCount = _config.getNumIrrigationSlots();
    Serial.printf(
        "t=%010lu | INFO  | SCHED    | init=ready slots=%u\n",
        millis(),
        slotCount
    );

    if (slotCount == 0) {
        Serial.printf(
            "t=%010lu | WARN  | SCHED    | tidak ada slot jadwal - irigasi otomatis tidak akan jalan\n",
            millis()
        );
    }
}

void TimerIrrigationScheduler::update() {
    _rtc.refresh();

    // Satu-satunya penanganan kegagalan: tanpa jam yang benar, jadwal tidak
    // punya arti. Fail-safe OFF, bukan fail-safe ON.
    if (!_rtc.isOk()) {
        if (_state == TimerState::IRRIGATING) {
            stopIrrigation();
        }
        if (_state != TimerState::RTC_ERROR) {
            _state = TimerState::RTC_ERROR;
            Serial.printf(
                "t=%010lu | WARN  | SCHED    | rtc=ERROR irigasi=OFF\n",
                millis()
            );
        }
        return;
    }

    uint16_t nowMinute = static_cast<uint16_t>(_rtc.getHour()) * 60U + _rtc.getMinute();
    int8_t   slot      = findActiveSlot(nowMinute);

    if (_state == TimerState::IRRIGATING) {
        if (slot < 0) {
            stopIrrigation();
            _state = TimerState::IDLE;
            return;
        }

        // Masih di dalam slot: JANGAN sentuh relay. Kalau operator mematikannya
        // dari web, biarkan mati sampai slot berikutnya.
        // Indeks tetap diperbarui agar telemetri benar bila dua slot bersambung.
        _activeSlot = slot;
        return;
    }

    // _state == IDLE atau RTC_ERROR
    if (slot >= 0) {
        startIrrigation(slot);
        _state = TimerState::IRRIGATING;
        return;
    }

    _state = TimerState::IDLE;
}

const char* TimerIrrigationScheduler::stateName() const {
    switch (_state) {
        case TimerState::IDLE:       return "IDLE";
        case TimerState::IRRIGATING: return "IRRIGATING";
        case TimerState::RTC_ERROR:  return "RTC_ERROR";
    }
    return "UNKNOWN";
}

bool TimerIrrigationScheduler::getNextSlot(uint8_t& hour, uint8_t& minute) {
    if (!_rtc.isOk()) {
        return false;
    }

    uint16_t nowMinute = static_cast<uint16_t>(_rtc.getHour()) * 60U + _rtc.getMinute();
    uint8_t  count     = _config.getNumIrrigationSlots();

    bool     found      = false;
    uint16_t bestMinute = 0;

    for (uint8_t i = 0; i < count; i++) {
        IrrigationSlot slot = _config.getIrrigationSlot(i);
        uint16_t startMinute = static_cast<uint16_t>(slot.startHour) * 60U + slot.startMinute;

        if (startMinute <= nowMinute) continue;

        if (!found || startMinute < bestMinute) {
            bestMinute = startMinute;
            found      = true;
        }
    }

    if (!found) {
        return false;
    }

    hour   = static_cast<uint8_t>(bestMinute / 60U);
    minute = static_cast<uint8_t>(bestMinute % 60U);
    return true;
}

void TimerIrrigationScheduler::startIrrigation(int8_t slotIndex) {
    _activeSlot = slotIndex;

    _relay.on(RELAY_PUMP_MIX);
    _relay.on(RELAY_SOLENOID_IRRIG);

    IrrigationSlot slot = _config.getIrrigationSlot(static_cast<uint8_t>(slotIndex));
    Serial.printf(
        "t=%010lu | INFO  | SCHED    | irigasi=ON slot=%d window=%02u:%02u-%02u:%02u relay=[4,8]\n",
        millis(),
        slotIndex,
        slot.startHour, slot.startMinute,
        slot.endHour,   slot.endMinute
    );
}

void TimerIrrigationScheduler::stopIrrigation() {
    _relay.off(RELAY_SOLENOID_IRRIG);
    _relay.off(RELAY_PUMP_MIX);

    Serial.printf(
        "t=%010lu | INFO  | SCHED    | irigasi=OFF slot=%d\n",
        millis(),
        _activeSlot
    );

    _activeSlot = -1;
}

int8_t TimerIrrigationScheduler::findActiveSlot(uint16_t nowMinute) const {
    uint8_t count = _config.getNumIrrigationSlots();

    for (uint8_t i = 0; i < count; i++) {
        IrrigationSlot slot = _config.getIrrigationSlot(i);
        uint16_t startMinute = static_cast<uint16_t>(slot.startHour) * 60U + slot.startMinute;
        uint16_t endMinute   = static_cast<uint16_t>(slot.endHour)   * 60U + slot.endMinute;

        if (isMinuteInsideWindow(nowMinute, startMinute, endMinute)) {
            return static_cast<int8_t>(i);
        }
    }

    return -1;
}
