#ifndef SYSTEM_CONFIG_H
#define SYSTEM_CONFIG_H

#define DEBUG_SERIAL true

// Mode irigasi DEFAULT (dipakai HANYA saat first boot / NVS kosong):
// 0 = ESP-NOW soil sensor (humidity threshold)
// 1 = Timer schedule (pakai data slot di Master/data/TimerIrrigationData.h)
//
// PENTING: setelah first boot, mode aktif disimpan di NVS namespace "cfg_soil"
// dan dapat diubah runtime lewat MQTT topic greenhouse/soil/reset_mode
// (dari tombol "Set Mode" di web). Nilai NVS akan menang di boot-boot berikutnya.
#define IRRIGATION_MODE_SOURCE 1

// Set true untuk skip jadwal harian dan langsung mix saat startup (mode testing)
// Set false untuk production — sistem menunggu jadwal jam DAILY_MIX_HOUR
#define SKIP_DAILY_SCHEDULE false

#endif
