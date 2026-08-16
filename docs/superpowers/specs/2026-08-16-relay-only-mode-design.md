# Desain: RELAY_ONLY_MODE — Kendali Relay via MQTT + Penyiraman Timer

Tanggal: 2026-08-16
Branch: `relay-only`
Status: disetujui untuk masuk rencana implementasi

## 1. Latar Belakang & Tujuan

Seluruh sensor sistem sedang dimaintain: pH (`PH_PIN`), TDS (`TDS_PIN`), suhu air
(DS18B20), ultrasonic level tangki, tiga flow meter, dan sensor kelembapan tanah
yang datang lewat ESP-NOW dari `Sleeve/`. Tanpa sensor, `FertigationFSM` tidak
bisa dijalankan — hampir semua transisinya bergantung pada pembacaan sensor, dan
`FertigationFSM::begin()` langsung masuk `ERROR` bila RTC bermasalah.

Yang dibutuhkan selama masa maintenance:

1. Web (lewat MQTT) hanya bisa menyalakan/mematikan relay. Tidak terhubung ke
   state FSM, tidak ada error handling sensor.
2. Penyiraman otomatis berbasis timer, memakai jadwal slot yang sudah ada di
   `Master/data/TimerIrrigationData.h`, juga tanpa keterkaitan ke state FSM
   maupun error handling sensor.
3. Urutan fertigasi (isi air, dosis A, dosis B, aduk) dijalankan **manual** oleh
   operator lewat tombol relay di web. FSM tidak dijalankan sama sekali.

Non-tujuan: mengubah aplikasi web, mengubah firmware `Sleeve/`, memperbaiki atau
mengganti sensor apa pun, dan mengubah perilaku sistem penuh saat mode ini mati.

## 2. Keputusan Desain

Ditetapkan bersama pemilik proyek sebelum desain ini ditulis:

| Keputusan | Pilihan |
|---|---|
| Arti "FSM manual" | FSM mati total; operator menekan tombol relay satu per satu |
| Cara mematikan | Flag compile-time `RELAY_ONLY_MODE`, bukan mencabut kode |
| Telemetri | Status relay + status timer + heartbeat; topik sensor tidak dipublish |
| Konflik manual vs jadwal | Operator menang; penjadwal edge-triggered |
| Basis kode | Kembangkan `Master/`, bukan project `Timing/` |
| Relay penyiraman | `RELAY_PUMP_MIX` (ch8) + `RELAY_SOLENOID_IRRIG` (ch4) |
| Flow meter irigasi | Tidak dipasang |

### 2.1 Kenapa `Master/`, bukan `Timing/`

`Timing/` sudah berisi firmware relay+RTC+penjadwal tanpa sensor
(`Timing/src/scheduler/TimerIrrigationScheduler.cpp`), dan logika edge-trigger
serta fail-safe RTC-nya dipakai sebagai acuan desain ini. Tapi `Timing/` tidak
dipakai sebagai basis karena:

- `Timing/src/actuators/RelayManager.cpp` menganggap **semua** kanal active-HIGH,
  sedangkan `Master/src/actuators/RelayManager.cpp` memakai active-LOW untuk
  `RELAY_PUMP_A`, `RELAY_PUMP_B`, `RELAY_WATER_INLET`, `RELAY_PUMP_MIX`, dan
  punya workaround `INPUT_PULLUP` khusus `RELAY_WATER_INLET` (board relay cacat:
  tetap ON saat digerakkan HIGH). Driver Master yang cocok dengan hardware saat
  ini.
- `Timing/` belum punya MQTT, WiFi provisioning, maupun `ConfigManager`/NVS —
  semuanya sudah matang di `Master/`.
- Dua firmware paralel harus dijaga sinkron.

`Timing/` dibiarkan apa adanya sebagai referensi; desain ini tidak mengubahnya.

### 2.2 Kenapa kelas MQTT baru, bukan `#if` di `MQTTManager`

Konstruktor `MQTTManager` (`Master/src/communication/MQTTManager.h:75`) menuntut
`FertigationFSM&`, `SoilHealthMonitor&`, dan `WaterLevel&` — persis tiga objek
yang mode ini ingin cabut. File-nya juga sudah 813 baris. Menaburkan `#if` di
sana mencampur dua jalur eksekusi dalam satu file dan memperbesar risiko regresi
saat flag dikembalikan ke 0. Biayanya: sekitar 60 baris logika connect
WiFi/MQTT yang mirip. Untuk mencegah kredensial broker jadi bercabang, seluruh
`#define` broker dan topik dipindah ke header bersama.

## 3. Arsitektur

```
RELAY_ONLY_MODE = 1                         RELAY_ONLY_MODE = 0 (sistem penuh)
------------------------------------        ---------------------------------
Main.ino                                    Main.ino
  ├─ ConfigManager  (slot jadwal saja)        ├─ ConfigManager
  ├─ RelayManager                             ├─ RelayManager
  ├─ RTCManager                               ├─ RTCManager
  ├─ TimerIrrigationScheduler   <── baru      ├─ SensorManager + 6 sensor
  └─ RelayOnlyMQTT              <── baru      ├─ ESPNowManager + SoilHealthMonitor
                                              ├─ RecoveryManager
                                              ├─ FertigationFSM
                                              └─ MQTTManager
```

Di mode relay-only, satu-satunya hal yang bergerak sendiri adalah
`TimerIrrigationScheduler`. Semua relay lain sepenuhnya di tangan operator.

### 3.1 Modul baru: `Master/src/scheduler/TimerIrrigationScheduler.{h,cpp}`

Ketergantungan: `RTCManager&`, `RelayManager&`, `ConfigManager&`. Modul ini tidak
mengenal `SensorManager`, `SoilHealthMonitor`, `FertigationFSM`, maupun
`FlowMeter`.

```cpp
enum class TimerState : uint8_t { IDLE, IRRIGATING, RTC_ERROR };

class TimerIrrigationScheduler {
public:
    TimerIrrigationScheduler(RTCManager&, RelayManager&, ConfigManager&);

    void begin();     // state = IDLE, activeSlot = -1; tidak menyentuh relay
    void update();    // dipanggil tiap loop()

    TimerState  getState()          const;
    const char* stateName()         const;
    bool        isIrrigating()      const;
    int8_t      getActiveSlotIndex() const;   // -1 bila tidak ada
    bool        getNextSlot(uint8_t& hour, uint8_t& minute) const;
};
```

`begin()` hanya menyetel state internal ke `IDLE` dan `activeSlot = -1`; ia tidak
menyentuh relay, karena `RelayManager::begin()` yang dipanggil lebih dulu di
`setup()` sudah memanggil `allOff()` (`RelayManager.cpp:69`).

Alur `update()`:

1. `rtc.refresh()`.
2. Bila `!rtc.isOk()`: bila sedang `IRRIGATING`, matikan `RELAY_PUMP_MIX` dan
   `RELAY_SOLENOID_IRRIG`; set state `RTC_ERROR`; `return`. Tidak ada slot yang
   dijalankan sampai RTC pulih (fail-safe OFF).
3. Hitung `nowMinute = hour * 60 + minute`.
4. Cari slot aktif: iterasi `0 .. configManager.getNumIrrigationSlots()-1`,
   bandingkan dengan `isMinuteInsideWindow()`.
5. Deteksi tepi terhadap state sebelumnya:
   - `IDLE`/`RTC_ERROR` → slot ditemukan: `relay.on(RELAY_PUMP_MIX)`,
     `relay.on(RELAY_SOLENOID_IRRIG)`, state `IRRIGATING`, simpan indeks slot.
   - `IRRIGATING` → tidak ada slot: `relay.off()` keduanya, state `IDLE`,
     `activeSlot = -1`.
   - Tidak ada perubahan tepi: **tidak menyentuh relay sama sekali**.

Poin nomor 5 itulah yang mewujudkan "operator menang": bila operator mematikan
relay di tengah slot, penjadwal tidak menyalakannya kembali karena state
internalnya masih `IRRIGATING` dan tidak ada tepi baru sampai slot berakhir.

`isMinuteInsideWindow()` sekarang `static` di `FertigationFSM.cpp:5` dan dipakai
dua kali di file yang sama (`:1289`, `:1325`). Fungsi ini dipindah ke
`Master/src/utils/TimeWindow.h` sebagai `inline`, lalu `FertigationFSM.cpp` dan
scheduler sama-sama meng-include-nya. Perilakunya tidak diubah, termasuk
penanganan window yang melewati tengah malam.

### 3.2 Modul baru: `Master/src/communication/RelayOnlyMQTT.{h,cpp}`

Ketergantungan: `RelayManager&`, `RTCManager&`, `TimerIrrigationScheduler&`.

```cpp
class RelayOnlyMQTT {
public:
    RelayOnlyMQTT(RelayManager&, RTCManager&, TimerIrrigationScheduler&);
    void begin();     // WiFi (WiFiManager) → MQTT → subscribe → bersihkan retained lama
    void update();    // reconnect + mqttClient.loop() + publish berkala
    bool isConnected();
};
```

Subscribe **hanya**:

- `greenhouse/actuators/cmd`
- `greenhouse/control/reset`

Logika perintah relay (validasi indeks 1..8, aksi `on`/`off`/`toggle`/`all_off`,
penerimaan field `relay` maupun `relay_id`, ACK) disalin dari
`MQTTManager::executeRelayCommand()`, `parseRelayIndex()`,
`relayIndexToChannel()`, dan `publishRelayCommandAck()` agar kontrak ke web
identik.

Tidak ada topik konfigurasi yang di-subscribe: seluruh konfigurasi di mode ini
berasal dari `Master/data/`.

### 3.3 Modul baru: `Master/src/communication/MQTTConfig.h`

Berisi pindahan `#define` dari `MQTTManager.h:21..63` — alamat broker, port,
client id, user, password, dan seluruh nama topik. `MQTTManager.h` meng-include
header ini sehingga jalur sistem penuh tidak berubah perilakunya;
`RelayOnlyMQTT.h` meng-include header yang sama. Tujuannya agar kredensial
broker tidak bercabang antara dua klien.

`MQTT_RECEIVE_ENABLED` dan `WIFI_BLOCKING_PORTAL_ENABLED` tetap di
`MQTTManager.h` karena keduanya hanya mengatur perilaku `MQTTManager`.

### 3.4 Perubahan `Master/src/config/SystemConfig.h`

```c
// 1 = mode relay-only: FSM & seluruh sensor tidak dijalankan. Web hanya bisa
//     on/off relay; penyiraman otomatis murni dari jadwal timer di
//     Master/data/TimerIrrigationData.h.
// 0 = sistem fertigasi penuh (FSM + sensor + ESP-NOW).
#define RELAY_ONLY_MODE 1

#if RELAY_ONLY_MODE && ENABLE_ANY_TEST_MODE
#error "RELAY_ONLY_MODE tidak bisa digabung dengan mode test — matikan salah satu."
#endif
```

Guard tersebut mencegah kebingungan: dengan FSM tidak dijalankan, seluruh flag di
`Master/test/TestFlags.h` yang bekerja lewat FSM menjadi tidak berpengaruh —
termasuk `ENABLE_IRRIGATION_TEST` yang jalurnya ada di `FertigationFSM.cpp:88-91`.
`SystemConfig.h` perlu meng-include `TestFlags.h` agar `ENABLE_ANY_TEST_MODE`
terdefinisi.

### 3.5 Perubahan `Master/src/Main.ino`

Satu percabangan `#if RELAY_ONLY_MODE` di tiga tempat: deklarasi objek global,
`setup()`, dan `loop()`.

`setup()` jalur relay-only, berurutan:

1. `Serial.begin(115200)` + tunggu USB CDC (sama seperti sekarang).
2. `resetAppNVSOnFirmwareChange()` — dipertahankan.
3. `configManager.begin()` lalu `loadTimerIrrigationData(configManager)` saja.
   `loadHardcodedFSMInputData()` tidak dipanggil: resep ppm, pH, volume tangki,
   dan jadwal mixing tidak relevan tanpa FSM.
4. `relay.begin()`.
5. `Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN)`.
6. `rtcManager.begin()` + log `isOk()`.
7. `scheduler.begin()`.
8. `mqtt.begin()`.

Tidak di-`begin()`, tidak dibuat objeknya: `PHSensor`, `TDSSensor`, `WaterLevel`,
`WaterTempSensor`, `FlowMeter` (ketiganya, termasuk pemasangan ISR),
`ESPNowManager`, `SoilHealthMonitor`, `SensorManager`, `RecipeManager`,
`IrrigationRecipe`, `RecoveryManager`, `FertigationFSM`, `MQTTManager`.

`loop()` jalur relay-only:

```cpp
scheduler.update();
mqtt.update();
// log status serial tiap STATUS_LOG_INTERVAL_MS
```

Log status serial di mode ini memuat: jam RTC, status RTC, state penjadwal,
indeks slot aktif, daftar relay yang menyala, status WiFi dan MQTT. Tidak ada
baris `SENSOR`, `FLOW`, maupun `SOIL`.

## 4. Kontrak MQTT

### 4.1 Masuk

```
greenhouse/actuators/cmd
{"relay": 4, "action": "on"}
```

- `relay` (atau `relay_id`): 1..8, dipetakan sama seperti sekarang —
  1 `MIXER_STIR`, 2 `SOLENOID_A`, 3 `SOLENOID_B`, 4 `SOLENOID_IRRIG`,
  5 `WATER_INLET`, 6 `PUMP_A`, 7 `PUMP_B`, 8 `PUMP_MIX`.
- `action` (atau `state`/`cmd`): `on`, `off`, `toggle`, `all_off`. Default
  `toggle` bila field tidak ada.

```
greenhouse/control/reset
```
Payload bebas; ACK ke `greenhouse/control/reset/ack` lalu `ESP.restart()`.

Karena formatnya identik dengan yang sudah diimplementasikan `MQTTManager`,
aplikasi web tidak perlu diubah untuk tombol relay.

### 4.2 Keluar

Interval publish 1 detik (`MQTT_PUBLISH_INTERVAL`), retained.

`greenhouse/actuators/status` — format tidak berubah dari
`MQTTManager::publishRelayStatus()`: objek `relays` berisi `relay_1..relay_8`
bertipe boolean, plus array `active_relays`.

`greenhouse/timer/status` — baru:

```json
{
  "device_id": "greenhouse-master-01",
  "rtc": "OK",
  "time": "2026-08-16 14:03",
  "state": "IRRIGATING",
  "active_slot": 7,
  "next_slot": {"hour": 15, "minute": 0},
  "slot_count": 10,
  "wifi": "UP",
  "uptime_ms": 123456
}
```

- `rtc`: `"OK"` atau `"ERROR"`.
- `state`: `"IDLE"`, `"IRRIGATING"`, atau `"RTC_ERROR"`.
- `active_slot`: indeks slot yang sedang berjalan, `-1` bila tidak ada.
- `next_slot`: `null` bila tidak ada slot berikutnya hari ini.

`greenhouse/config/ack` — ACK per perintah relay, format tidak berubah dari
`MQTTManager::publishRelayCommandAck()`.

### 4.3 Pembersihan retained topik lama

`greenhouse/sensors`, `greenhouse/fsm/state`, `greenhouse/soil/health`, dan
`greenhouse/alert/tank_low` dipublish `retained` oleh firmware sebelumnya,
sehingga nilai terakhir tetap tersimpan di broker meski firmware ini tidak
pernah mengirimnya lagi — web bisa menampilkannya seolah data hidup. Karena itu
`RelayOnlyMQTT::begin()`, tepat setelah koneksi MQTT berhasil, mengirim satu
payload kosong `retained` ke empat topik tersebut untuk menghapusnya dari
broker.

## 5. Penanganan Kegagalan

| Kejadian | Perilaku |
|---|---|
| Sensor apa pun | Tidak ada penanganan, karena tidak ada sensor yang di-init. Tidak ada jalur kode yang bisa memicu `ErrorCode`. |
| RTC gagal dibaca | Relay irigasi dimatikan bila sedang menyiram; state `RTC_ERROR`; tidak ada slot dijalankan sampai pulih. |
| WiFi/MQTT putus | Penjadwal jalan terus offline. Saat reconnect tidak ada relay yang disentuh; hanya publish status. |
| Reboot di tengah slot | Semua relay mati oleh `RelayManager::begin()`, dan penjadwal mulai dari anggapan "di luar slot", jadi tick pertama mendeteksi tepi masuk dan menyalakan irigasi kembali. |
| Reboot setelah operator mematikan di tengah slot | Irigasi menyala kembali. Konsekuensi yang diterima demi tidak menulis state ke NVS tiap slot. |
| `getNumIrrigationSlots() == 0` | Penjadwal tetap `IDLE` selamanya; tidak ada relay yang disentuh. Dicatat di log serial saat boot. |

RTC adalah satu-satunya penanganan kegagalan yang dipertahankan, karena tanpa jam
yang benar penjadwal tidak punya arti. Perlu dicatat: `SYNC_RTC_FROM_BUILD_TIME`
(`Master/test/TestFlags.h:27`) saat ini `0`, jadi jam PCF8563 harus sudah benar
sebelum flash.

## 6. Yang Sengaja Tidak Dikerjakan

- **Flow meter irigasi tidak dipasang.** `flowIrrig` (pin 9) sebenarnya satu-satunya
  sensor yang masih relevan — ia bisa mencatat liter per slot dan tidak akan
  pernah bisa mengubah atau menghentikan penyiraman. Diputuskan tidak dipakai
  agar mode ini benar-benar bebas sensor. Bila nanti diinginkan, penambahannya
  terbatas pada `flowIrrig.begin(ISR)` dan satu field liter di
  `greenhouse/timer/status`, tanpa menyentuh logika relay.
- Aplikasi web tidak diubah.
- Firmware `Sleeve/` tidak diubah.
- Project `Timing/` tidak diubah dan tidak dihapus.
- Tidak ada saklar enable/disable penjadwal dari web.
- Tidak ada jadwal pengadukan otomatis; pengadukan dilakukan manual lewat relay 1
  dan 8.
- Kode FSM, sensor, dan ESP-NOW tidak dihapus. `RELAY_ONLY_MODE 0` mengembalikan
  sistem penuh apa adanya.

## 7. Verifikasi

Repo ini tidak memakai framework unit test (`Master/test/` berisi header data dan
flag saja), sehingga verifikasi dilakukan lewat kompilasi dan uji hardware.

1. `pio run` di `Master/` dengan `RELAY_ONLY_MODE 1` **dan** `0`. Keduanya harus
   sukses — memastikan jalur sistem penuh tidak ikut rusak.
2. `pio run` dengan `RELAY_ONLY_MODE 1` dan salah satu flag test menyala harus
   gagal dengan pesan `#error` yang dimaksud.
3. Uji jadwal: ubah sementara satu slot di `Master/data/TimerIrrigationData.h` ke
   beberapa menit dari waktu sekarang. Serial log harus menunjukkan transisi
   `IDLE → IRRIGATING → IDLE`, dan relay ch4 + ch8 ikut hidup lalu mati pada
   menit yang tepat.
4. Uji operator menang: saat slot aktif, kirim `{"relay":4,"action":"off"}`.
   Relay harus mati dan tidak menyala sendiri sampai slot berikutnya.
5. Uji fail-safe RTC: putus RTC saat slot aktif. Relay irigasi harus mati dan
   `state` menjadi `RTC_ERROR`.
6. Uji offline: matikan WiFi. Penjadwal harus tetap menjalankan slot berikutnya.
7. Cek broker HiveMQ: `greenhouse/timer/status` dan `greenhouse/actuators/status`
   terisi; `greenhouse/sensors`, `greenhouse/fsm/state`, `greenhouse/soil/health`,
   `greenhouse/alert/tank_low` sudah kosong (retained terhapus).
8. Uji seluruh 8 relay dari web satu per satu, memastikan ch5
   (`RELAY_WATER_INLET`, yang memakai workaround `INPUT_PULLUP`) benar-benar mati
   saat diperintahkan `off`.
