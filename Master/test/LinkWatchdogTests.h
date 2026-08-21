#pragma once

// Uji decideLinkAction() yang dievaluasi saat kompilasi.
// Tidak ada kode yang dihasilkan; bila salah satu assert gagal, build berhenti.
// Di-include dari Main.ino supaya ikut dievaluasi setiap `pio run`.

#include "../src/utils/LinkWatchdog.h"

// Ambang yang dipakai di seluruh uji ini.
static constexpr uint32_t TEST_FAILSAFE_MS = 300000UL;   // 5 menit
static constexpr uint32_t TEST_RESTART_MS  = 900000UL;   // 15 menit

// --- Link sehat: berapa pun lamanya, tidak ada aksi ---
static_assert(
    decideLinkAction(true, 0, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_OK,
    "link hidup harus LINK_OK");
static_assert(
    decideLinkAction(true, 99999999UL, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_OK,
    "link hidup harus LINK_OK walau elapsed besar");

// --- Link putus tapi belum sampai ambang failsafe ---
static_assert(
    decideLinkAction(false, 0, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_DEGRADED,
    "baru putus harus LINK_DEGRADED, jangan langsung matikan relay");
static_assert(
    decideLinkAction(false, TEST_FAILSAFE_MS - 1, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_DEGRADED,
    "satu ms sebelum ambang masih LINK_DEGRADED");

// --- Ambang failsafe inklusif ---
static_assert(
    decideLinkAction(false, TEST_FAILSAFE_MS, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_FAILSAFE,
    "tepat di ambang failsafe harus LINK_FAILSAFE");
static_assert(
    decideLinkAction(false, TEST_FAILSAFE_MS + 1, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_FAILSAFE,
    "lewat ambang failsafe harus LINK_FAILSAFE");
static_assert(
    decideLinkAction(false, TEST_RESTART_MS - 1, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_FAILSAFE,
    "satu ms sebelum ambang restart masih LINK_FAILSAFE");

// --- Ambang restart inklusif dan menang atas failsafe ---
static_assert(
    decideLinkAction(false, TEST_RESTART_MS, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_RESTART,
    "tepat di ambang restart harus LINK_RESTART");
static_assert(
    decideLinkAction(false, TEST_RESTART_MS * 10, TEST_FAILSAFE_MS, TEST_RESTART_MS) == LINK_RESTART,
    "jauh lewat ambang restart tetap LINK_RESTART");

// --- 0 = fitur dimatikan ---
static_assert(
    decideLinkAction(false, 99999999UL, 0, TEST_RESTART_MS) == LINK_RESTART,
    "failsafe=0 mematikan failsafe, restart tetap jalan");
static_assert(
    decideLinkAction(false, 99999999UL, TEST_FAILSAFE_MS, 0) == LINK_FAILSAFE,
    "restart=0 mematikan restart, failsafe tetap jalan");
static_assert(
    decideLinkAction(false, 99999999UL, 0, 0) == LINK_DEGRADED,
    "keduanya 0 = tidak ada aksi otomatis sama sekali");

// --- Rollover millis(): elapsed dihitung dengan aritmetika unsigned ---
static_assert(
    elapsedSince(0xFFFFFF00UL, 0x00000064UL) == 356UL,
    "elapsed harus benar saat millis() rollover");
static_assert(
    elapsedSince(1000UL, 4000UL) == 3000UL,
    "elapsed normal tanpa rollover");
static_assert(
    elapsedSince(4000UL, 4000UL) == 0UL,
    "elapsed nol saat waktu sama");

// --- Throttle retry ---
static_assert(
    isRetryDue(RETRY_NEVER_ATTEMPTED, 0UL, 5000UL),
    "belum pernah mencoba harus langsung boleh retry");
static_assert(
    !isRetryDue(1000UL, 5999UL, 5000UL),
    "belum sampai interval belum boleh retry");
static_assert(
    isRetryDue(1000UL, 6000UL, 5000UL),
    "tepat di interval sudah boleh retry");
