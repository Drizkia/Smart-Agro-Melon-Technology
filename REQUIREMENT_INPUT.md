# Requirement Input Web ke Firmware ESP32

Dokumen ini adalah kontrak input dari web/backend ke firmware. Semua input dikirim via MQTT dalam format JSON.

Firmware akan tetap berada di state `IDLE` sampai konfigurasi inti lengkap. Config dianggap lengkap jika bagian berikut sudah valid:

- System/tangki
- Jadwal tanam dan jadwal mixing harian
- PPM dan dosis awal nutrisi A/B
- pH default
- Recipe nutrisi
- Threshold irigasi

Timer irrigation dan stir schedule bersifat tambahan.

## 1. System / Tangki

Topic:

```text
greenhouse/config/system
```

Schema:

```json
{
  "total_plants": 80,
  "max_consumption_per_plant": 2,
  "target_fill_volume": 800,
  "tank_capacity_liter": 1000,
  "tank_height_cm": 120,
  "tank_diameter_cm": 103
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `total_plants` | tanaman | Ya | `_totalPlants` | Jumlah tanaman, dipakai untuk timer irrigation volume total. |
| `max_consumption_per_plant` | L/tanaman/hari | Ya | `_maxConsumptionPerPlant` | Kebutuhan air harian maksimal per tanaman. Dipakai timer irrigation fallback untuk menghitung volume harian total. |
| `target_fill_volume` | liter | Ya | `_targetFillVolume` | Target volume air dalam tangki. Solenoid inlet stop saat `tankVolume >= target_fill_volume`. Biasanya 80–90% kapasitas, bukan 100%. |
| `tank_capacity_liter` | liter | Ya | `_tankCapacityLiter` | Kapasitas nominal tangki. Dipakai sebagai safety clamp dan deteksi overflow. |
| `tank_height_cm` | cm | Ya | `_tankHeightCM` | Tinggi efektif tangki dari dasar sampai titik penuh. Dipakai konversi ultrasonic ke liter. |
| `tank_diameter_cm` | cm | Ya | `_tankDiameterCM` | Diameter tangki silinder vertikal. Dipakai konversi ultrasonic ke liter. |

Rumus volume air:

```text
water_height_cm = tank_height_cm - ultrasonic_distance_cm
volume_liter = π × (tank_diameter_cm / 2)^2 × water_height_cm / 1000
```

Catatan:

- `target_fill_volume` tidak boleh lebih besar dari `tank_capacity_liter`.
- Jika target 800L dan pembacaan aktual 850L, solenoid inlet ditutup dan sistem tetap lanjut setelah level stabil.

## 2. Jadwal Tanam dan Mixing Harian

Topic:

```text
greenhouse/config/schedule
```

Schema:

```json
{
  "plant_year": 2026,
  "plant_month": 7,
  "plant_day": 15,
  "daily_mix_hour": 5,
  "daily_mix_minute": 0
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `plant_year` | tahun | Ya | `_plantYear` | Tahun tanggal tanam. |
| `plant_month` | 1–12 | Ya | `_plantMonth` | Bulan tanggal tanam. |
| `plant_day` | 1–31 | Ya | `_plantDay` | Hari tanggal tanam. |
| `daily_mix_hour` | 0–23 | Ya | `_dailyMixHour` | Jam mulai proses mixing harian. |
| `daily_mix_minute` | 0–59 | Ya | `_dailyMixMinute` | Menit mulai proses mixing harian. |

Firmware menghitung umur tanaman (`plantAgeDays`) dari tanggal tanam. Umur ini dipakai memilih recipe stage.

## 3. PPM dan Dosis Awal Nutrisi

Topic:

```text
greenhouse/config/ppm
```

Schema:

```json
{
  "ppm_tolerance": 50,
  "initial_a": 0.8,
  "initial_b": 0.8
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `ppm_tolerance` | ppm | Ya | `_ppmTolerance` | Toleransi batas bawah target PPM. PPM over tetap diterima. |
| `initial_a` | liter | Ya | `_initialNutrientA` | Volume awal nutrisi A yang dimasukkan setelah fill water. |
| `initial_b` | liter | Ya | `_initialNutrientB` | Volume awal nutrisi B yang dimasukkan setelah fill water. |

Perilaku validasi PPM:

- Jika `ppm >= target_ppm - ppm_tolerance`, sistem dianggap cukup dan lanjut `READY`.
- Jika PPM over target, sistem tetap lanjut dan mengirim warning `OVER_PPM`.
- Jika `ppm < target_ppm - ppm_tolerance`, sistem melakukan koreksi A/B per loop.
- Koreksi memakai `CORRECTION_DOSE = 0.05L` untuk A dan B.

## 4. pH Default

Topic:

```text
greenhouse/config/ph
```

Schema:

```json
{
  "min_ph": 5.5,
  "max_ph": 6.5
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `min_ph` | pH | Ya | `_defaultMinPH` | Batas bawah pH default. |
| `max_ph` | pH | Ya | `_defaultMaxPH` | Batas atas pH default. |

Nilai ini dipakai sebagai fallback jika recipe stage tidak mengirim `min_ph` / `max_ph`.

## 5. Recipe Nutrisi Berdasarkan Umur Tanaman

Topic:

```text
greenhouse/config/recipe
```

Schema:

```json
{
  "stages": [
    {
      "max_age_days": 10,
      "target_ppm": 700,
      "min_ph": 5.5,
      "max_ph": 6.5
    },
    {
      "max_age_days": 20,
      "target_ppm": 900,
      "min_ph": 5.6,
      "max_ph": 6.4
    }
  ]
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `stages[].max_age_days` | hari | Ya | `RecipeStageConfig.maxAgeDays` | Batas akhir umur tanaman untuk stage ini. |
| `stages[].target_ppm` | ppm | Ya | `RecipeStageConfig.targetPPM` | Target PPM untuk stage. |
| `stages[].min_ph` | pH | Disarankan | `RecipeStageConfig.minPH` | Batas bawah pH untuk stage. |
| `stages[].max_ph` | pH | Disarankan | `RecipeStageConfig.maxPH` | Batas atas pH untuk stage. |

Cara kerja stage:

- Stage harus dikirim urut naik berdasarkan `max_age_days`.
- Jika stage pertama `max_age_days = 10`, maka recipe itu berlaku untuk day 1–10.
- Stage kedua `max_age_days = 20` berlaku untuk day 11–20.
- Firmware memilih stage pertama yang memenuhi:

```text
plantAgeDays <= max_age_days
```

Batas firmware:

- Maksimal `MAX_RECIPE_STAGES = 7`.
- Jika umur tanaman melewati `max_age_days` stage terakhir, firmware menghapus config dan kembali ke `IDLE`.

Source code utama:

- `RecipeManager::getRecipe()`
- `ConfigManager::_recipeStages`

## 6. Threshold Irigasi Berdasarkan Umur Tanaman

Topic:

```text
greenhouse/config/irrigation
```

Schema:

```json
{
  "stages": [
    {
      "max_age_days": 10,
      "dry_threshold": 3900,
      "wet_threshold": 3650
    },
    {
      "max_age_days": 20,
      "dry_threshold": 3800,
      "wet_threshold": 3500
    }
  ]
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `stages[].max_age_days` | hari | Ya | `IrrigationStageConfig.maxAgeDays` | Batas akhir umur tanaman untuk threshold ini. |
| `stages[].dry_threshold` | ADC | Ya | `IrrigationStageConfig.dryThreshold` | Jika `soilADC >= dry_threshold`, sistem mulai irigasi saat state `READY`. |
| `stages[].wet_threshold` | ADC | Ya | `IrrigationStageConfig.wetThreshold` | Irigasi berhenti saat `soilADC <= wet_threshold`. |

Cara kerja stage sama seperti recipe nutrisi:

- `max_age_days = 10` berlaku untuk day 1–10.
- `max_age_days = 20` berlaku untuk day 11–20.
- Stage harus urut naik.

Batas firmware:

- Maksimal `MAX_IRRIGATION_STAGES = 7`.

## 7. Timer Irrigation Fallback

Topic:

```text
greenhouse/config/timer_irrigation
```

Schema:

```json
{
  "slots": [
    { "hour": 7, "minute": 0 },
    { "hour": 16, "minute": 0 }
  ]
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `slots[].hour` | 0–23 | Opsional | `IrrigationSlot.hour` | Jam slot timer irrigation. |
| `slots[].minute` | 0–59 | Opsional | `IrrigationSlot.minute` | Menit slot timer irrigation. |

Perhitungan target per slot:

```text
daily_total_ml = max_consumption_per_plant × 1000 × total_plants
target_per_slot_ml = daily_total_ml / jumlah_slot
```

Batas firmware:

- Maksimal `MAX_IRRIG_SLOTS = 10`.
- Mode timer dipakai jika `SoilHealthMonitor` switch ke `TIMER`.

## 8. Stir Schedule

Topic:

```text
greenhouse/config/mix_schedule_ext
```

Schema:

```json
{
  "stir_evening_hour": 18,
  "stir_evening_minute": 0,
  "stir_duration_seconds": 300
}
```

Keterangan:

| Field | Unit | Wajib | Variable firmware | Fungsi |
|---|---:|---:|---|---|
| `stir_evening_hour` | 0–23 | Opsional | `_stirEveningHour` | Jam stir sore. |
| `stir_evening_minute` | 0–59 | Opsional | `_stirEveningMinute` | Menit stir sore. |
| `stir_duration_seconds` | detik | Opsional | `_stirDurationMs` | Durasi stir. Firmware menyimpan dalam ms. |

Field lama `mix_interval_days` dan `per_plant_need_liter` diabaikan.

## 8b. Status Hidup/Mati Perangkat (WAJIB dipakai web)

Topic publish firmware:

```text
greenhouse/status
```

**Inilah satu-satunya sumber kebenaran "ESP hidup atau mati". Web TIDAK boleh
lagi menebaknya dari kesegaran topik lain.**

Payload (retained, QoS 1):

```json
{ "device_id": "greenhouse-master-01", "status": "online" }
{ "device_id": "greenhouse-master-01", "status": "offline" }
```

Cara kerja:

- `online` dipublish firmware retained tepat setelah berhasil connect ke broker.
- `offline` dipasang sebagai **Last Will** saat connect. Broker sendiri yang
  menerbitkannya bila ESP hilang tanpa sempat pamit — WiFi putus, brownout,
  listrik mati, atau board hang.
- `offline` juga dipublish firmware secara sadar sebelum ia me-restart diri
  (perintah reset dari web, atau failsafe link).
- Karena retained, web yang baru subscribe langsung tahu keadaan terakhir.

Kenapa ini ditambahkan:

Sebelumnya firmware tidak pernah mengirim sinyal liveness apa pun. Web terpaksa
menebak dari kesegaran `greenhouse/sensors`. Di `RELAY_ONLY_MODE` topik sensor
memang tidak pernah dipublish — malah dihapus oleh `clearStaleRetainedTopics()` —
sehingga tebakan itu **selalu** berbunyi "ESP mati" walau perangkatnya sehat.

Catatan penting untuk web:

- `greenhouse/actuators/status` adalah **retained**. Isinya tetap tampil walau
  ESP sudah mati berhari-hari. Jangan pakai ada/tidaknya payload itu sebagai
  tanda perangkat hidup — pakai `greenhouse/status`.
- Saat `status` = `offline`, tampilkan status relay sebagai **keadaan terakhir
  yang diketahui**, bukan keadaan sekarang. Firmware tidak bisa memastikan relay
  fisiknya masih sesuai.

## 8c. Failsafe Link Putus

Bila firmware tidak bisa menjangkau broker:

| Lama putus | Tindakan firmware |
|---|---|
| < 5 menit | Sambung ulang WiFi/MQTT berkala. Relay dibiarkan apa adanya. |
| >= 5 menit | **Semua relay dimatikan** (`allOff`). |
| >= 15 menit | Board restart. Boot memanggil `allOff()` lagi. |

Ambangnya diatur di `Master/src/communication/MQTTConfig.h`
(`LINK_FAILSAFE_MS`, `LINK_RESTART_MS`); nilai `0` mematikan masing-masing
perilaku.

Konsekuensi untuk web: relay bisa berubah menjadi OFF **tanpa ada perintah dari
web**. Kalau `greenhouse/status` sempat `offline` lebih dari 5 menit, anggap
seluruh relay sudah direset ke OFF.

Tombol reset di web (`greenhouse/control/reset`) hanya sampai bila ESP sedang
terhubung. Perangkat yang offline tidak bisa diselamatkan dari web — pemulihannya
sekarang jadi tugas failsafe di atas.

## 9. Telemetry Status Relay / Aktuator

Topic publish firmware:

```text
greenhouse/actuators/status
```

Status saat ini:

- Firmware publish status relay setiap `MQTT_PUBLISH_INTERVAL` atau 1 detik.
- Firmware juga publish status relay saat state FSM berubah.
- Publish menggunakan retained message, jadi web/backend yang baru subscribe akan langsung menerima status terakhir.
- Nilai `true` berarti relay sedang aktif/ON menurut firmware.
- Nilai `false` berarti relay sedang mati/OFF menurut firmware.
- Relay module memakai active-low di firmware, tetapi web/backend cukup pakai boolean dari payload ini; tidak perlu membalik logika pin.

Schema payload:

```json
{
  "device_id": "greenhouse-master-01",
  "timestamp": 123456,
  "uptime_ms": 123456,
  "rtc_time": "2026-08-21 14:33:05",
  "relays": {
    "relay_1": true,
    "relay_2": false,
    "relay_3": false,
    "relay_4": false,
    "relay_5": false,
    "relay_6": false,
    "relay_7": false,
    "relay_8": true
  },
  "active_relays": [1, 8]
}
```

Mapping relay:

| Relay | Aktuator |
|---:|---|
| `relay_1` | Mixer stir / pengaduk mixing chamber |
| `relay_2` | Solenoid nutrisi A |
| `relay_3` | Solenoid nutrisi B |
| `relay_4` | Solenoid irigasi |
| `relay_5` | Water inlet / solenoid air masuk |
| `relay_6` | Pompa nutrisi A |
| `relay_7` | Pompa nutrisi B |
| `relay_8` | Pompa mixing / pompa irigasi dari mixing tank |

Catatan untuk web/backend:

- Untuk tampilan sederhana, tampilkan `relays.relay_x` sebagai ON/OFF.
- Untuk highlight aktuator yang sedang jalan, gunakan `active_relays`.
- `timestamp` dan `uptime_ms` sama-sama `millis()`, yaitu **uptime sejak boot**,
  bukan waktu absolut. Jangan bandingkan dengan jam server untuk menghitung basi
  — nilainya kembali ke ~0 setiap kali ESP restart.
- `rtc_time` adalah jam dinding dari RTC (waktu lokal WIB, bukan UTC), atau
  `null` bila RTC gagal dibaca. Ini yang bisa dipakai menampilkan "data per jam
  berapa".
- Status ini merepresentasikan state aktual output firmware saat loop MQTT berjalan, baik akibat FSM otomatis maupun command manual yang diterima saat `READY`.

## 10. Command Fertigasi / Aktuator

Topic:

```text
greenhouse/actuators/cmd
```

Status saat ini:

- Command hanya diterima jika FSM berada di state `READY`.
- Jika state belum `READY`, firmware mengabaikan command dan publish ACK error `actuators_cmd_not_ready`.

Tujuan:

- Mencegah irigasi/aktuator berjalan ketika air belum terisi, nutrisi belum masuk, mixing belum selesai, atau PPM/pH belum tervalidasi.

## 11. Reset Soil Mode

Topic:

```text
greenhouse/soil/reset_mode
```

Fungsi:

- Mengembalikan mode soil monitor ke `HUMIDITY`.
- Tidak menghapus config NVS.

## 12. Clear Config / Hapus Memori ESP

Status saat ini:

- Firmware sudah punya fungsi internal `ConfigManager::clearConfig()`.
- Fungsi ini menghapus config NVS untuk PPM, pH, recipe, irrigation, system, schedule, timer irrigation, dan stir schedule.
- Belum ada topic MQTT khusus dari web untuk memanggil clear config.

Rencana patch berikutnya:

```text
greenhouse/config/reset
```

Payload awal yang disarankan:

```json
{
  "scope": "all"
}
```

Setelah diterima, firmware akan menjalankan `configManager.clearConfig()` dan FSM kembali ke `IDLE`.

## 13. Minimal Urutan Input dari Web

Agar firmware keluar dari `IDLE`, web/backend harus mengirim minimal:

1. `greenhouse/config/system`
2. `greenhouse/config/schedule`
3. `greenhouse/config/ppm`
4. `greenhouse/config/ph`
5. `greenhouse/config/recipe`
6. `greenhouse/config/irrigation`

Setelah semua valid, FSM akan pindah dari `IDLE` ke `WAIT_DAILY_MIX`.

## 14. Ringkasan Unit Penting

| Data | Unit |
|---|---:|
| `target_fill_volume` | liter |
| `tank_capacity_liter` | liter |
| `tank_height_cm` | cm |
| `tank_diameter_cm` | cm |
| `initial_a` | liter |
| `initial_b` | liter |
| `CORRECTION_DOSE` | liter, fixed 0.05L |
| `max_consumption_per_plant` | L/tanaman/hari |
| `target_ppm` | ppm |
| `ppm_tolerance` | ppm |
| `dry_threshold` / `wet_threshold` | ADC |
| `stir_duration_seconds` | detik |
