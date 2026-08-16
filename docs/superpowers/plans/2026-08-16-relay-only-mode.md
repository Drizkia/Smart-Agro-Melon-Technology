# RELAY_ONLY_MODE Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Menambahkan mode compile-time di firmware `Master/` yang mematikan seluruh FSM dan sensor, menyisakan kendali relay on/off lewat MQTT ditambah penyiraman otomatis berbasis jadwal timer dari `Master/data/TimerIrrigationData.h`.

**Architecture:** Satu flag `RELAY_ONLY_MODE` di `Master/src/config/SystemConfig.h` mempercabangkan `Main.ino` menjadi dua jalur eksekusi. Jalur baru hanya membuat lima objek: `ConfigManager`, `RelayManager`, `RTCManager`, `TimerIrrigationScheduler` (baru), dan `RelayOnlyMQTT` (baru). Penjadwal bersifat edge-triggered — ia hanya menyentuh relay saat slot dimulai dan berakhir, sehingga perintah manual dari operator tidak pernah dilawan. Kode FSM, sensor, dan ESP-NOW tetap utuh dan aktif kembali saat flag dikembalikan ke 0.

**Tech Stack:** C++ / Arduino framework, ESP32-S3, PlatformIO 6.1.19, PubSubClient (MQTT over TLS), ArduinoJson, WiFiManager, RTClib (PCF8563).

**Spec:** `docs/superpowers/specs/2026-08-16-relay-only-mode-design.md`

## Global Constraints

- Board dan environment PlatformIO: `esp32-s3-devkitm-1`, satu-satunya env di `Master/platformio.ini`.
- `Master/platformio.ini` punya `build_flags = -I test -I data`, sehingga `#include "TestFlags.h"` menunjuk ke `Master/test/TestFlags.h` dan `#include "FSMInputData.h"` menunjuk ke `Master/data/FSMInputData.h`. Jangan ubah baris ini.
- Relay penyiraman: `RELAY_PUMP_MIX` (indeks 8) dan `RELAY_SOLENOID_IRRIG` (indeks 4). Tidak ada relay lain yang boleh disentuh penjadwal.
- Nama topik MQTT yang sudah dipakai web tidak boleh berubah: `greenhouse/actuators/cmd`, `greenhouse/actuators/status`, `greenhouse/config/ack`, `greenhouse/control/reset`, `greenhouse/control/reset/ack`. Topik baru: `greenhouse/timer/status`.
- Format payload perintah relay tidak boleh berubah: `{"relay": <1..8>, "action": "on"|"off"|"toggle"|"all_off"}`, dengan `relay_id` sebagai alias `relay`, dan `state`/`cmd` sebagai alias `action`.
- Interval publish: `MQTT_PUBLISH_INTERVAL` = `1000UL` ms.
- Kredensial broker tidak berubah: `aead004bf5144152b88233f1a1949418.s1.eu.hivemq.cloud:8883`, client id `greenhouse-master-01`.
- Bahasa komentar dan log: Bahasa Indonesia, mengikuti gaya file sekitarnya.
- Format log serial yang wajib diikuti: `Serial.printf("t=%010lu | %-5s | %-8s | %s\n", millis(), level, component, message)` — level `INFO `/`WARN `/`ERROR`, komponen dipad ke 8 karakter.
- Repo ini **tidak** punya framework unit test runtime. Verifikasi otomatis dilakukan lewat `static_assert` (Task 1) dan kompilasi `pio run`; sisanya verifikasi hardware manual di Task 7.

---

### Task 1: Ekstrak `isMinuteInsideWindow` ke header bersama + uji `static_assert`

Fungsi pembanding window waktu saat ini `static` di dalam `FertigationFSM.cpp` dan tidak bisa dipakai modul lain. Task ini memindahkannya ke header bersama sekaligus memberinya uji otomatis yang berjalan saat kompilasi.

**Files:**
- Create: `Master/src/utils/TimeWindow.h`
- Create: `Master/test/TimeWindowTests.h`
- Modify: `Master/src/fsm/FertigationFSM.cpp:1-11` (hapus definisi `static`, tambah include)
- Modify: `Master/src/Main.ino` (tambah satu include untuk menjalankan uji)

**Interfaces:**
- Consumes: tidak ada.
- Produces: `constexpr bool isMinuteInsideWindow(uint16_t nowMinute, uint16_t startMinute, uint16_t endMinute)` dari `"utils/TimeWindow.h"`. Dipakai Task 3.

- [ ] **Step 1: Tulis uji yang gagal**

Buat `Master/test/TimeWindowTests.h`:

```cpp
#pragma once

// Uji isMinuteInsideWindow() yang dievaluasi saat kompilasi.
// Tidak ada kode yang dihasilkan; bila salah satu assert gagal, build berhenti.
// Di-include dari Main.ino supaya ikut dievaluasi setiap `pio run`.

#include "../src/utils/TimeWindow.h"

// Window normal 07:00-07:03 -> 420..423
static_assert(isMinuteInsideWindow(420, 420, 423), "menit start harus inklusif");
static_assert(isMinuteInsideWindow(422, 420, 423), "menit di tengah window harus true");
static_assert(!isMinuteInsideWindow(423, 420, 423), "menit end harus eksklusif");
static_assert(!isMinuteInsideWindow(419, 420, 423), "sebelum start harus false");
static_assert(!isMinuteInsideWindow(500, 420, 423), "jauh setelah end harus false");

// Window kosong (start == end)
static_assert(!isMinuteInsideWindow(420, 420, 420), "start == end harus selalu false");
static_assert(!isMinuteInsideWindow(0, 0, 0), "start == end == 0 harus false");

// Window melewati tengah malam 23:30-00:30 -> 1410..30
static_assert(isMinuteInsideWindow(1410, 1410, 30), "start harus inklusif saat wrap");
static_assert(isMinuteInsideWindow(1439, 1410, 30), "menit terakhir hari harus true saat wrap");
static_assert(isMinuteInsideWindow(0, 1410, 30), "tengah malam harus true saat wrap");
static_assert(isMinuteInsideWindow(29, 1410, 30), "menit sebelum end harus true saat wrap");
static_assert(!isMinuteInsideWindow(30, 1410, 30), "end harus eksklusif saat wrap");
static_assert(!isMinuteInsideWindow(720, 1410, 30), "siang hari harus false saat wrap");
```

Lalu tambahkan include-nya di `Master/src/Main.ino`, tepat setelah baris `#include "FSMInputData.h"` (baris 16):

```cpp
#include "TimeWindowTests.h"
```

- [ ] **Step 2: Jalankan build untuk memastikan gagal**

Run: `pio run -d Master`
Expected: FAIL. Compiler berhenti di `TimeWindowTests.h` dengan pesan semacam
`fatal error: ../src/utils/TimeWindow.h: No such file or directory`
karena headernya belum ada.

- [ ] **Step 3: Tulis implementasi minimal**

Buat `Master/src/utils/TimeWindow.h`:

```cpp
#ifndef TIME_WINDOW_H
#define TIME_WINDOW_H

#include <stdint.h>

// Apakah nowMinute (menit-hari, 0..1439) berada di dalam window [start, end)?
//   start == end  -> window kosong, selalu false
//   start <  end  -> window normal dalam satu hari
//   start >  end  -> window melewati tengah malam (mis. 23:30 -> 00:30)
//
// Ditulis sebagai satu ekspresi return agar tetap valid constexpr di C++11,
// sehingga bisa diuji lewat static_assert di Master/test/TimeWindowTests.h.
constexpr bool isMinuteInsideWindow(uint16_t nowMinute,
                                    uint16_t startMinute,
                                    uint16_t endMinute) {
    return (startMinute == endMinute)
        ? false
        : (startMinute < endMinute)
            ? (nowMinute >= startMinute && nowMinute < endMinute)
            : (nowMinute >= startMinute || nowMinute < endMinute);
}

#endif
```

- [ ] **Step 4: Jalankan build untuk memastikan lolos**

Run: `pio run -d Master`
Expected: SUCCESS. Bila ada `static_assert` yang gagal, compiler menyebut baris assert-nya — perbaiki `TimeWindow.h`, bukan assert-nya.

- [ ] **Step 5: Hapus definisi lama di FertigationFSM.cpp**

Di `Master/src/fsm/FertigationFSM.cpp`, ganti baris 1-11 yang sekarang berbunyi:

```cpp
#include "FertigationFSM.h"
#include "../config/Constants.h"
#include "../config/SystemConfig.h"

static bool isMinuteInsideWindow(uint16_t nowMinute, uint16_t startMinute, uint16_t endMinute) {
    if (startMinute == endMinute) return false;
    if (startMinute < endMinute) {
        return nowMinute >= startMinute && nowMinute < endMinute;
    }
    return nowMinute >= startMinute || nowMinute < endMinute;
}
```

menjadi:

```cpp
#include "FertigationFSM.h"
#include "../config/Constants.h"
#include "../config/SystemConfig.h"
#include "../utils/TimeWindow.h"
```

Dua pemakaiannya di `FertigationFSM.cpp` (di dalam `handleIrrigation()` dan `handleTimerIrrigation()`) tidak berubah — nama dan tanda tangannya sama persis.

- [ ] **Step 6: Build ulang dan pastikan tidak ada definisi ganda**

Run: `pio run -d Master`
Expected: SUCCESS, tanpa peringatan `redefinition` atau `unused function`.

Run: `grep -rn "isMinuteInsideWindow" Master/src Master/test`
Expected: tepat satu definisi (di `TimeWindow.h`); sisanya pemakaian di `FertigationFSM.cpp` dan include di `TimeWindowTests.h`.

- [ ] **Step 7: Commit**

```bash
git add Master/src/utils/TimeWindow.h Master/test/TimeWindowTests.h \
        Master/src/fsm/FertigationFSM.cpp Master/src/Main.ino
git commit -m "Refactor: pindahkan isMinuteInsideWindow ke utils/TimeWindow.h + uji static_assert"
```

---

### Task 2: Pisahkan konfigurasi broker & topik MQTT ke header bersama

Agar `RelayOnlyMQTT` (Task 4) dan `MQTTManager` tidak punya salinan kredensial broker yang bisa berbeda.

**Files:**
- Create: `Master/src/communication/MQTTConfig.h`
- Modify: `Master/src/communication/MQTTManager.h:19-71`

**Interfaces:**
- Consumes: tidak ada.
- Produces: makro `MQTT_BROKER`, `MQTT_PORT`, `MQTT_CLIENT_ID`, `MQTT_USER`, `MQTT_PASS`, seluruh `TOPIC_*` termasuk `TOPIC_TIMER_STATUS` yang baru, `MQTT_PUBLISH_INTERVAL`, dan `WIFI_BLOCKING_PORTAL_ENABLED`. Dipakai Task 4.

- [ ] **Step 1: Buat header bersama**

Buat `Master/src/communication/MQTTConfig.h`:

```cpp
#ifndef MQTT_CONFIG_H
#define MQTT_CONFIG_H

// Konfigurasi broker + daftar topik, dipakai bersama oleh MQTTManager
// (mode sistem penuh) dan RelayOnlyMQTT (mode RELAY_ONLY_MODE).
// WiFi dikonfigurasi via captive portal (WiFiManager) — tidak ada SSID hardcoded.

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
#define TOPIC_SOIL_HEALTH   "greenhouse/soil/health"
#define TOPIC_ALERT_TANK_LOW "greenhouse/alert/tank_low"

// Status penjadwal timer — hanya dipublish saat RELAY_ONLY_MODE.
#define TOPIC_TIMER_STATUS  "greenhouse/timer/status"

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
#define TOPIC_CMD_RESET         "greenhouse/control/reset"
#define TOPIC_CMD_RESET_ACK     "greenhouse/control/reset/ack"

// Interval publish (ms)
#define MQTT_PUBLISH_INTERVAL 1000UL

// Jika 0, firmware tidak akan membuka captive portal/blocking WiFi saat boot.
#define WIFI_BLOCKING_PORTAL_ENABLED 1

#endif
```

- [ ] **Step 2: Hapus makro yang dipindah dari MQTTManager.h**

Di `Master/src/communication/MQTTManager.h`, hapus seluruh blok makro dari baris 19 (`// WiFi dikonfigurasi via captive portal ...`) sampai baris 71 (`#define WIFI_BLOCKING_PORTAL_ENABLED 1`), lalu ganti dengan:

```cpp
#include "MQTTConfig.h"

// MQTT sekarang dipakai publish-only pada mode sistem penuh. Input FSM/command
// diambil dari Master/data/FSMInputData.h sampai inbound web siap dipakai lagi.
#define MQTT_RECEIVE_ENABLED 0
```

`MQTT_RECEIVE_ENABLED` sengaja tetap di sini: makro itu hanya mengatur perilaku `MQTTManager`, bukan `RelayOnlyMQTT` (yang selalu subscribe).

Baris `#include <ArduinoJson.h>` dan include lain di atasnya jangan diubah.

- [ ] **Step 3: Build untuk memastikan tidak ada makro yang hilang**

Run: `pio run -d Master`
Expected: SUCCESS. Bila muncul `'TOPIC_X' was not declared`, berarti ada makro yang belum ikut dipindah — tambahkan ke `MQTTConfig.h`.

- [ ] **Step 4: Pastikan tidak ada duplikat definisi**

Run: `grep -n "define MQTT_BROKER\|define TOPIC_" Master/src/communication/MQTTManager.h`
Expected: tidak ada hasil (semuanya sudah pindah).

- [ ] **Step 5: Commit**

```bash
git add Master/src/communication/MQTTConfig.h Master/src/communication/MQTTManager.h
git commit -m "Refactor: pisahkan konfigurasi broker & topik MQTT ke MQTTConfig.h"
```

---

### Task 3: `TimerIrrigationScheduler`

Penjadwal penyiraman berbasis RTC + slot dari `ConfigManager`. Tidak mengenal sensor, FSM, maupun flow meter.

**Files:**
- Create: `Master/src/scheduler/TimerIrrigationScheduler.h`
- Create: `Master/src/scheduler/TimerIrrigationScheduler.cpp`

**Interfaces:**
- Consumes: `isMinuteInsideWindow()` dari Task 1. `ConfigManager::getNumIrrigationSlots()` dan `ConfigManager::getIrrigationSlot(uint8_t)` yang mengembalikan `IrrigationSlot{startHour, startMinute, endHour, endMinute}` (`Master/src/config/ConfigManager.h:31`). `RTCManager::refresh()`, `isOk()`, `getHour()`, `getMinute()` — perhatikan `getHour()`/`getMinute()` **bukan** metode `const`, jadi metode yang memakainya juga tidak boleh `const`.
- Produces:
  - `enum class TimerState : uint8_t { IDLE, IRRIGATING, RTC_ERROR }`
  - `TimerIrrigationScheduler(RTCManager&, RelayManager&, ConfigManager&)`
  - `void begin()`, `void update()`
  - `TimerState getState() const`, `const char* stateName() const`, `bool isIrrigating() const`, `int8_t getActiveSlotIndex() const`
  - `bool getNextSlot(uint8_t& hour, uint8_t& minute)` — **non-const**
  Dipakai Task 4 dan Task 6.

- [ ] **Step 1: Tulis header**

Buat `Master/src/scheduler/TimerIrrigationScheduler.h`:

```cpp
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
```

- [ ] **Step 2: Tulis implementasi**

Buat `Master/src/scheduler/TimerIrrigationScheduler.cpp`:

```cpp
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
```

- [ ] **Step 3: Build**

PlatformIO mengkompilasi seluruh `.cpp` di `Master/src/`, jadi file ini ikut terkompilasi meski belum dipakai siapa pun.

Run: `pio run -d Master`
Expected: SUCCESS.

- [ ] **Step 4: Periksa isolasi modul**

Run: `grep -n "SensorManager\|SoilHealth\|FertigationFSM\|FlowMeter\|ESPNow" Master/src/scheduler/TimerIrrigationScheduler.h Master/src/scheduler/TimerIrrigationScheduler.cpp`
Expected: tidak ada hasil. Bila ada, modulnya bocor ke jalur sensor dan harus diperbaiki.

- [ ] **Step 5: Commit**

```bash
git add Master/src/scheduler/
git commit -m "Feat: TimerIrrigationScheduler - penyiraman berbasis slot RTC tanpa sensor"
```

---

### Task 4: `RelayOnlyMQTT`

Klien MQTT ringan: subscribe hanya perintah relay dan reset, publish status relay + status timer, dan membersihkan retained topik sensor lama saat boot.

**Files:**
- Create: `Master/src/communication/RelayOnlyMQTT.h`
- Create: `Master/src/communication/RelayOnlyMQTT.cpp`

**Interfaces:**
- Consumes: makro dari `MQTTConfig.h` (Task 2). `TimerIrrigationScheduler` (Task 3): `stateName()`, `getActiveSlotIndex()`, `getNextSlot(uint8_t&, uint8_t&)`. `RelayManager::on/off/isOn/allOff/isValidRelayIndex`. `RTCManager::isOk/getYear/getMonth/getDay/getHour/getMinute`.
- Produces: `RelayOnlyMQTT(RelayManager&, RTCManager&, TimerIrrigationScheduler&)`, `void begin()`, `void update()`, `bool isConnected()`. Dipakai Task 6.

- [ ] **Step 1: Tulis header**

Buat `Master/src/communication/RelayOnlyMQTT.h`:

```cpp
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
```

- [ ] **Step 2: Tulis implementasi**

Buat `Master/src/communication/RelayOnlyMQTT.cpp`:

```cpp
#include "RelayOnlyMQTT.h"
#include <WiFiManager.h>

RelayOnlyMQTT* RelayOnlyMQTT::_instance = nullptr;

RelayOnlyMQTT::RelayOnlyMQTT(RelayManager& relay,
                             RTCManager& rtc,
                             TimerIrrigationScheduler& sched)
    : mqttClient(wifiClient),
      relayManager(relay),
      rtcManager(rtc),
      scheduler(sched)
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

    for (uint8_t i = 0; i < sizeof(staleTopics) / sizeof(staleTopics[0]); i++) {
        mqttClient.publish(staleTopics[i], "", true);
    }

    Serial.printf(
        "t=%010lu | INFO  | MQTT     | retained_lama=dibersihkan count=%u\n",
        millis(),
        (unsigned)(sizeof(staleTopics) / sizeof(staleTopics[0]))
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

void RelayOnlyMQTT::handleRemoteReset(const String& payload) {
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
    mqttClient.loop();

    Serial.printf(
        "t=%010lu | WARN  | RESET    | remote_reset triggered reason=%s\n",
        millis(),
        reason.c_str()
    );

    delay(300);
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
    if (relayManager.isOn(RELAY_MIXER_STIR))    active.add(1);
    if (relayManager.isOn(RELAY_SOLENOID_A))    active.add(2);
    if (relayManager.isOn(RELAY_SOLENOID_B))    active.add(3);
    if (relayManager.isOn(RELAY_SOLENOID_IRRIG)) active.add(4);
    if (relayManager.isOn(RELAY_WATER_INLET))   active.add(5);
    if (relayManager.isOn(RELAY_PUMP_A))        active.add(6);
    if (relayManager.isOn(RELAY_PUMP_B))        active.add(7);
    if (relayManager.isOn(RELAY_PUMP_MIX))      active.add(8);

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

    uint8_t nextHour = 0;
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
```

Catatan: `publishTimerStatus()` memanggil `scheduler.getNextSlot()` yang non-const, karena `RTCManager::getHour()` bukan metode `const`. Itu sebabnya field `scheduler` disimpan sebagai referensi non-const.

Catatan kedua: `connectWiFi()` di sini memakai `WiFiManager` dengan SSID/password AP yang sama seperti `MQTTManager`, tapi tanpa CSS kustom portal. Kredensial WiFi tersimpan di NVS oleh `WiFiManager`, jadi portal hanya muncul bila belum pernah dikonfigurasi.

- [ ] **Step 3: Build**

Run: `pio run -d Master`
Expected: SUCCESS.

- [ ] **Step 4: Pastikan tidak ada topik konfigurasi yang ikut di-subscribe**

Run: `grep -n "subscribe" Master/src/communication/RelayOnlyMQTT.cpp`
Expected: tepat dua baris — `TOPIC_CMD` dan `TOPIC_CMD_RESET`.

Run: `grep -n "TOPIC_SENSORS\|TOPIC_FSM_STATE\|TOPIC_SOIL_HEALTH\|TOPIC_ALERT_TANK_LOW" Master/src/communication/RelayOnlyMQTT.cpp`
Expected: hanya muncul di dalam `clearStaleRetainedTopics()`, tidak di jalur publish berkala.

- [ ] **Step 5: Commit**

```bash
git add Master/src/communication/RelayOnlyMQTT.h Master/src/communication/RelayOnlyMQTT.cpp
git commit -m "Feat: RelayOnlyMQTT - kendali relay + status timer tanpa jalur sensor"
```

---

### Task 5: Flag `RELAY_ONLY_MODE` dan guard mode test

**Files:**
- Modify: `Master/src/config/SystemConfig.h`

**Interfaces:**
- Consumes: `ENABLE_ANY_TEST_MODE` dari `Master/test/TestFlags.h:42`.
- Produces: makro `RELAY_ONLY_MODE`. Dipakai Task 6.

- [ ] **Step 1: Tambah flag dan guard**

Ganti isi `Master/src/config/SystemConfig.h` menjadi:

```cpp
#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#include "TestFlags.h"

#define DEBUG_SERIAL true

// 1 = mode relay-only. FertigationFSM dan SELURUH sensor tidak dijalankan.
//     Web hanya bisa on/off relay lewat MQTT; penyiraman otomatis murni dari
//     jadwal slot di Master/data/TimerIrrigationData.h.
//     Dipakai selama sensor sedang dimaintain.
// 0 = sistem fertigasi penuh (FSM + sensor + ESP-NOW).
#define RELAY_ONLY_MODE 1

// Mode test bekerja lewat FSM, sedangkan RELAY_ONLY_MODE tidak menjalankan FSM
// sama sekali. Menggabungkan keduanya membuat flag test diam-diam tidak berefek.
#if RELAY_ONLY_MODE && ENABLE_ANY_TEST_MODE
#error "RELAY_ONLY_MODE tidak bisa digabung dengan mode test — matikan salah satu."
#endif

// Mode irigasi DEFAULT (dipakai HANYA saat first boot / NVS kosong):
// 0 = ESP-NOW soil sensor (humidity threshold)
// 1 = Timer schedule (pakai data slot di Master/data/TimerIrrigationData.h)
//
// PENTING: setelah first boot, mode aktif disimpan di NVS namespace "cfg_soil"
// dan dapat diubah runtime lewat MQTT topic greenhouse/soil/reset_mode
// (dari tombol "Set Mode" di web). Nilai NVS akan menang di boot-boot berikutnya.
// Tidak berpengaruh saat RELAY_ONLY_MODE = 1.
#define IRRIGATION_MODE_SOURCE 1

// Set true untuk skip jadwal harian dan langsung mix saat startup (mode testing)
// Set false untuk production — sistem menunggu jadwal jam DAILY_MIX_HOUR
// Tidak berpengaruh saat RELAY_ONLY_MODE = 1.
#define SKIP_DAILY_SCHEDULE false

#endif
```

- [ ] **Step 2: Build — jalur sistem penuh masih harus utuh**

Belum ada yang membaca `RELAY_ONLY_MODE`, jadi build harus tetap sukses apa adanya.

Run: `pio run -d Master`
Expected: SUCCESS.

- [ ] **Step 3: Verifikasi guard benar-benar memicu error**

Ubah sementara `Master/test/TestFlags.h:10` menjadi `#define ENABLE_IRRIGATION_TEST 1`.

Run: `pio run -d Master`
Expected: FAIL dengan pesan `RELAY_ONLY_MODE tidak bisa digabung dengan mode test — matikan salah satu.`

Kembalikan `ENABLE_IRRIGATION_TEST` ke `0`.

Run: `pio run -d Master`
Expected: SUCCESS.

- [ ] **Step 4: Commit**

```bash
git add Master/src/config/SystemConfig.h
git commit -m "Feat: flag RELAY_ONLY_MODE + guard terhadap mode test"
```

---

### Task 6: Percabangan `Main.ino`

**Files:**
- Modify: `Master/src/Main.ino`

**Interfaces:**
- Consumes: `RELAY_ONLY_MODE` (Task 5), `TimerIrrigationScheduler` (Task 3), `RelayOnlyMQTT` (Task 4), `loadTimerIrrigationData(ConfigManager&)` dari `Master/data/TimerIrrigationData.h:79`.
- Produces: firmware yang bisa dibangun di kedua nilai flag.

- [ ] **Step 1: Tambah include modul baru**

Di `Master/src/Main.ino`, setelah baris `#include "communication/MQTTManager.h"` (baris 20), tambahkan:

```cpp
#include "communication/RelayOnlyMQTT.h"
#include "scheduler/TimerIrrigationScheduler.h"
#include "TimerIrrigationData.h"
```

`TimerIrrigationData.h` sudah ikut ter-include lewat `FSMInputData.h`, tapi ditulis eksplisit agar jalur relay-only tidak bergantung pada include tak langsung.

- [ ] **Step 2: Pisahkan deklarasi objek global**

Bungkus seluruh blok deklarasi objek global yang ada sekarang (mulai `RelayManager relay;` di baris 42 sampai `MQTTManager mqtt(...);` di baris 106) menjadi:

```cpp
#if RELAY_ONLY_MODE

// Mode relay-only: hanya lima objek. Tidak ada sensor, ESP-NOW, recovery,
// recipe, maupun FSM yang dibuat — jadi tidak ada satu pun yang di-init.
RelayManager  relay;
RTCManager    rtcManager;
ConfigManager configManager;

TimerIrrigationScheduler scheduler(rtcManager, relay, configManager);
RelayOnlyMQTT            mqtt(relay, rtcManager, scheduler);

#else

// ... seluruh blok deklarasi lama, TIDAK diubah isinya ...

#endif
```

Urutan deklarasi penting: `scheduler` memerlukan ketiga objek di atasnya, dan `mqtt` memerlukan `scheduler`.

- [ ] **Step 3: Pisahkan fungsi bantu yang menyentuh sensor/FSM**

Fungsi yang boleh tetap di luar guard (dipakai kedua jalur):
`clearPreferencesNamespace()`, `resetAppNVSOnFirmwareChange()`, `logLine()`, `logBootStep()`, konstanta `STATUS_LOG_INTERVAL_MS` dan `FIRMWARE_BUILD_ID`.

Semua fungsi lain menyentuh objek yang tidak ada di mode relay-only dan harus masuk ke dalam `#else`: `runFlowCalibrationTestA/B()`, `runRelayHardwareTest()`, `stateToString()`, `errorToString()`, `modeToString()`, `phStatus()`, `logRTCStatus()`, `appendRelay()`, `formatActiveRelays()`, `formatSoilRules()`, `logSystemStatus()`, `logStateEvent()`, `logConnectionEvent()`, `flowAISR()`, `flowBISR()`, `flowIrrigISR()`.

Cara paling aman: buka satu blok `#if RELAY_ONLY_MODE` tepat setelah `resetAppNVSOnFirmwareChange()`, isi dengan kode mode relay-only (Step 4 dan 5), lalu `#else`, lalu seluruh sisa file yang sekarang tanpa diubah, lalu `#endif` di akhir file.

- [ ] **Step 4: Tulis log status mode relay-only**

Di dalam blok `#if RELAY_ONLY_MODE`, tambahkan:

```cpp
void logRelayOnlyStatus() {
    char activeRelays[32];
    snprintf(activeRelays, sizeof(activeRelays), "[");
    bool first = true;
    for (uint8_t i = 1; i <= 8; i++) {
        if (!relay.isOn(static_cast<RelayChannel>(i))) continue;
        size_t used = strlen(activeRelays);
        snprintf(activeRelays + used, sizeof(activeRelays) - used, "%s%u", first ? "" : ",", i);
        first = false;
    }
    strncat(activeRelays, "]", sizeof(activeRelays) - strlen(activeRelays) - 1);

    Serial.printf(
        "t=%010lu | INFO  | STATUS   | mode=RELAY_ONLY sched=%s slot=%d relays=%s\n",
        millis(),
        scheduler.stateName(),
        scheduler.getActiveSlotIndex(),
        activeRelays
    );

    Serial.printf(
        "t=%010lu | INFO  | RTC      | ok=%s time=%04u-%02u-%02u %02u:%02u\n",
        millis(),
        rtcManager.isOk() ? "YES" : "NO",
        rtcManager.getYear(), rtcManager.getMonth(), rtcManager.getDay(),
        rtcManager.getHour(), rtcManager.getMinute()
    );

    Serial.printf(
        "t=%010lu | INFO  | NETWORK  | wifi=%s mqtt=%s ip=%s\n",
        millis(),
        WiFi.status() == WL_CONNECTED ? "UP" : "DOWN",
        mqtt.isConnected() ? "UP" : "DOWN",
        WiFi.localIP().toString().c_str()
    );
}
```

- [ ] **Step 5: Tulis `setup()` dan `loop()` mode relay-only**

Masih di dalam blok `#if RELAY_ONLY_MODE`:

```cpp
void setup() {
    Serial.begin(115200);
    unsigned long t = millis();
    while (!Serial && (millis() - t) < 3000) delay(10);
    logLine("INFO", "BOOT", "serial=ready mode=RELAY_ONLY");

    resetAppNVSOnFirmwareChange();

    // Hanya slot jadwal yang dimuat. loadHardcodedFSMInputData() sengaja TIDAK
    // dipanggil: resep ppm, pH, volume tangki, dan jadwal mixing tidak relevan
    // tanpa FSM.
    configManager.begin();
    loadTimerIrrigationData(configManager);
    logBootStep("CONFIG", "timer_slots_loaded");

    relay.begin();   // sudah memanggil allOff()
    logBootStep("RELAY", "ready");

    Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
    logBootStep("I2C", "ready");

    rtcManager.begin();
    logBootStep("RTC", rtcManager.isOk() ? "ready" : "error");

    scheduler.begin();

    mqtt.begin();
    logBootStep("MQTT", mqtt.isConnected() ? "connected" : "disconnected");

    logLine("INFO", "BOOT", "system=ready mode=RELAY_ONLY fsm=disabled sensors=disabled");
}

void loop() {
    scheduler.update();
    mqtt.update();

    static unsigned long lastStatusLog = 0;
    if (millis() - lastStatusLog >= STATUS_LOG_INTERVAL_MS) {
        lastStatusLog = millis();
        logRelayOnlyStatus();
    }
}
```

- [ ] **Step 6: Build jalur relay-only**

Pastikan `RELAY_ONLY_MODE` bernilai `1` di `SystemConfig.h`.

Run: `pio run -d Master`
Expected: SUCCESS.

- [ ] **Step 7: Build jalur sistem penuh**

Ubah sementara `RELAY_ONLY_MODE` menjadi `0`.

Run: `pio run -d Master`
Expected: SUCCESS. Ini yang membuktikan sistem lama tidak ikut rusak.

Kembalikan `RELAY_ONLY_MODE` ke `1`.

Run: `pio run -d Master`
Expected: SUCCESS.

- [ ] **Step 8: Pastikan tidak ada objek sensor yang dibuat di jalur relay-only**

Run: `grep -n "SensorManager sensorManager\|ESPNowManager espNow\|SoilHealthMonitor soilHealth\|FertigationFSM fsm\|RecoveryManager recovery\|RecipeManager recipeManager\|IrrigationRecipe irrigationRecipe\|MQTTManager mqtt\|PHSensor \|TDSSensor \|WaterLevel \|WaterTempSensor \|FlowMeter " Master/src/Main.ino`
Expected: seluruh hasil berada di dalam blok `#else` (jalur sistem penuh), tidak satu pun di blok `#if RELAY_ONLY_MODE`. Periksa manual nomor barisnya terhadap posisi `#if`/`#else`/`#endif`.

- [ ] **Step 9: Commit**

```bash
git add Master/src/Main.ino
git commit -m "Feat: percabangan Main.ino untuk RELAY_ONLY_MODE"
```

---

### Task 7: Verifikasi hardware

Task ini tidak mengubah kode. Deliverable-nya adalah catatan hasil uji, dan perbaikan bila ada yang gagal.

**Files:**
- Modify: `Master/data/TimerIrrigationData.h` (sementara, untuk Step 2 — dikembalikan di Step 8)

**Interfaces:**
- Consumes: seluruh hasil Task 1-6.
- Produces: konfirmasi bahwa mode ini berperilaku sesuai spec di hardware asli.

- [ ] **Step 1: Flash dan amati boot**

Run: `pio run -d Master -t upload && pio device monitor -d Master`
Expected: log boot memuat `mode=RELAY_ONLY`, `CONFIG | init=timer_slots_loaded`, `SCHED | init=ready slots=10`, dan `system=ready mode=RELAY_ONLY fsm=disabled sensors=disabled`. Tidak boleh ada baris `SENSOR`, `FLOW`, `SOIL`, atau `FSM`.

- [ ] **Step 2: Uji jadwal**

Ubah sementara slot pertama di `Master/data/TimerIrrigationData.h` menjadi beberapa menit dari waktu RTC sekarang, dengan durasi 2 menit. Flash ulang.

Expected: pada menit mulai muncul `SCHED | irigasi=ON slot=0 ...` dan relay ch4 + ch8 menyala; pada menit selesai muncul `SCHED | irigasi=OFF slot=0` dan keduanya mati.

- [ ] **Step 3: Uji operator menang**

Saat slot dari Step 2 sedang aktif, publish ke `greenhouse/actuators/cmd`:

```json
{"relay": 4, "action": "off"}
```

Expected: relay ch4 mati dan **tidak** menyala sendiri sampai slot berikutnya. Log `CMD | relay=4 action=off sumber=manual` muncul, dan tidak ada `SCHED | irigasi=ON` menyusul.

- [ ] **Step 4: Uji seluruh 8 relay dari web**

Kirim `{"relay": N, "action": "on"}` lalu `{"relay": N, "action": "off"}` untuk N = 1..8, satu per satu.

Expected: setiap relay menyala dan mati sesuai perintah. Perhatikan khusus ch5 (`RELAY_WATER_INLET`) yang memakai workaround `INPUT_PULLUP` di `RelayManager::off()` — pastikan benar-benar mati, bukan sekadar berubah di telemetri.

- [ ] **Step 5: Uji fail-safe RTC**

Saat slot aktif, putus koneksi modul RTC (cabut kabel I2C).

Expected: relay ch4 dan ch8 mati, log `SCHED | rtc=ERROR irigasi=OFF`, dan `greenhouse/timer/status` menampilkan `"state":"RTC_ERROR"` dan `"rtc":"ERROR"`. Setelah RTC disambung kembali dan masih di dalam window slot, penyiraman menyala lagi.

- [ ] **Step 6: Uji offline**

Matikan akses point WiFi.

Expected: penjadwal tetap menjalankan slot berikutnya; log `NETWORK | wifi=DOWN mqtt=DOWN` muncul tapi `SCHED` tetap bekerja.

- [ ] **Step 7: Periksa topik di broker**

Subscribe ke `greenhouse/#` di HiveMQ.

Expected:
- `greenhouse/actuators/status` terisi dan formatnya sama seperti sebelum perubahan.
- `greenhouse/timer/status` terisi dengan field `rtc`, `time`, `state`, `active_slot`, `next_slot`, `wifi`, `uptime_ms`.
- `greenhouse/sensors`, `greenhouse/fsm/state`, `greenhouse/soil/health`, `greenhouse/alert/tank_low` **kosong** (retained sudah terhapus). Klien baru yang subscribe tidak menerima payload apa pun dari empat topik itu.

- [ ] **Step 8: Kembalikan jadwal asli dan commit**

Kembalikan `Master/data/TimerIrrigationData.h` ke 10 slot aslinya (07:00-07:03 sampai 16:00-16:03), flash ulang, dan pastikan log boot kembali menunjukkan `slots=10`.

```bash
git add Master/data/TimerIrrigationData.h
git commit -m "Chore: kembalikan jadwal irigasi asli setelah uji hardware"
```

Bila tidak ada perubahan tersisa pada file itu, lewati commit.
