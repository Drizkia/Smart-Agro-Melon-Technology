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
