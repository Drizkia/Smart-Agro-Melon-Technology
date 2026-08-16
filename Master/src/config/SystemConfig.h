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
