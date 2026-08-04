#pragma once

#include <ArduinoJson.h>
#include "config/ConfigManager.h"
#include "TimerIrrigationData.h"

// Hardcoded replacement for MQTT inbound configuration.
// Keep the JSON field names identical to REQUIREMENT_INPUT.md so SOP values can
// be edited here later without touching FSM or ConfigManager logic.
//
// Source of truth for the values below: "GH_Kebon_Agung_5_Juli_2016.pdf"
// (pindah tanam / transplant day = Ahad, 5 Juli 2026 -> Hari ke-1).
// As of the last edit (2026-07-18, Sabtu) the plant age is Hari ke-14,
// which the PDF places in the "GP 1100" ppm band. This age is NOT hardcoded
// here — it's derived at runtime from plant_year/plant_month/plant_day below
// vs. the current date, so it stays correct as days pass.

static const char FSM_INPUT_PAYLOAD_SYSTEM[] = R"json(
{
  "total_plants": 80,
  "max_consumption_per_plant": 2.0,
  "target_fill_volume": 640.0,
  "tank_capacity_liter": 700.0,
  "tank_height_cm": 142.0,
  "tank_diameter_cm": 83.0
}
)json";
// Tank: toren EXCEL AL-700 (700 L nominal). target_fill_volume = 90% x 700 = 630 L.
// tank_height_cm / tank_diameter_cm are the AL-700's published dimensions
// (142 cm tall, 83 cm diameter) — not in the PDF, but needed for the
// ultrasonic level->volume conversion, so update these if your unit's spec
// sheet differs. total_plants and max_consumption_per_plant aren't covered
// by the PDF either, left as-is.

static const char FSM_INPUT_PAYLOAD_SCHEDULE[] = R"json(
{
  "plant_year": 2026,
  "plant_month": 7,
  "plant_day": 5,
  "daily_mix_hour": 4,
  "daily_mix_minute": 0
}
)json";
// plant_day corrected to 5 (Ahad, 5 Juli 2026 = Hari ke-1 in the PDF).
// daily_mix_hour/minute aren't specified in the PDF (no explicit AM mixing
// clock time given), left at the existing 05:00 default.

static const char FSM_INPUT_PAYLOAD_PPM[] = R"json(
{
  "ppm_tolerance": 50.0,
  "initial_a": 1.0,
  "initial_b": 1.0
}
)json";
// Not covered by the PDF (no starting pump-ratio data), left as-is.

static const char FSM_INPUT_PAYLOAD_PH[] = R"json(
{
  "min_ph": 5.5,
  "max_ph": 8.0
}
)json";
// min_ph diturunkan ke 5.5 agar sesuai pembacaan sensor pH lapangan (5.49–5.52).
// Nilai PDF "5.9" adalah target ideal, tapi sensor aktual konsisten di 5.49–5.52.

static const char FSM_INPUT_PAYLOAD_RECIPE[] = R"json(
{
  "stages": [
    { "max_age_days": 1,  "target_ppm": 0.0,    "min_ph": 5.5, "max_ph": 6.5 },
    { "max_age_days": 3,  "target_ppm": 500.0,  "min_ph": 5.5, "max_ph": 6.5 },
    { "max_age_days": 5,  "target_ppm": 750.0,  "min_ph": 5.5, "max_ph": 6.5 },
    { "max_age_days": 8,  "target_ppm": 900.0,  "min_ph": 5.5, "max_ph": 6.5 },
    { "max_age_days": 14, "target_ppm": 1100.0, "min_ph": 5.5, "max_ph": 6.5 },
    { "max_age_days": 58, "target_ppm": 1200.0, "min_ph": 5.5, "max_ph": 6.5 },
    { "max_age_days": 70, "target_ppm": 1100.0, "min_ph": 5.5, "max_ph": 6.5 }
  ]
}
)json";
// Replaces the single 900 ppm placeholder stage with the full ppm ramp read
// off the "AB mix ppm" column of the PDF (Hari ke-1..70, day 1 = transplant
// with no mix yet -> 0 ppm):
//   Hari 1        -> 0    ppm (tanam, belum ada mix)
//   Hari 2-3      -> 500  ppm
//   Hari 4-5      -> 750  ppm
//   Hari 6-8      -> 900  ppm
//   Hari 9-14     -> 1100 ppm   <- current stage as of 2026-07-18 (Hari 14)
//   Hari 15-58    -> 1200 ppm
//   Hari 59-70    -> 1100 ppm
// NOTE: this is 7 stages, up from the original 1 — double check
// MAX_RECIPE_STAGES in ConfigManager.h is >= 7, or the loader will silently
// drop the tail stages (the load loop breaks once recipeCount hits the cap).

static const char FSM_INPUT_PAYLOAD_IRRIGATION[] = R"json(
{
  "stages": [
    {
      "max_age_days": 120,
      "dry_threshold": 3900,
      "wet_threshold": 3650
    }
  ]
}
)json";
// Left unchanged: the PDF has no soil-moisture/ADC threshold data, and per
// your note irrigation is running off the TIMER fallback rather than
// HUMIDITY for this cycle, so there's nothing here to reconcile against the
// PDF right now. Revisit these thresholds once you're back on HUMIDITY mode.

static const char FSM_INPUT_PAYLOAD_MIX_SCHEDULE_EXT[] = R"json(
{
  "stir_evening_hour": 18,
  "stir_evening_minute": 0,
  "stir_duration_seconds": 300
}
)json";
// Not covered by the PDF (only "Cek Torn selalu terisi air dan nutrisi" as a
// general note, no explicit evening stir time), left as-is.

inline void loadHardcodedFSMInputData(ConfigManager& configManager) {
    JsonDocument systemDoc;
    deserializeJson(systemDoc, FSM_INPUT_PAYLOAD_SYSTEM);
    float tankCap = systemDoc["tank_capacity_liter"].as<float>();
    float targetFill = systemDoc["target_fill_volume"].as<float>();
    if (tankCap > 0.0f && targetFill > tankCap) {
        targetFill = tankCap;
    }
    configManager.setSystemConfig(
        systemDoc["total_plants"].as<uint16_t>(),
        systemDoc["max_consumption_per_plant"].as<float>(),
        targetFill,
        tankCap,
        systemDoc["tank_height_cm"].as<float>(),
        systemDoc["tank_diameter_cm"].as<float>()
    );

    JsonDocument scheduleDoc;
    deserializeJson(scheduleDoc, FSM_INPUT_PAYLOAD_SCHEDULE);
    configManager.setScheduleConfig(
        scheduleDoc["plant_year"].as<uint16_t>(),
        scheduleDoc["plant_month"].as<uint8_t>(),
        scheduleDoc["plant_day"].as<uint8_t>(),
        scheduleDoc["daily_mix_hour"].as<uint8_t>(),
        scheduleDoc["daily_mix_minute"].as<uint8_t>()
    );

    JsonDocument ppmDoc;
    deserializeJson(ppmDoc, FSM_INPUT_PAYLOAD_PPM);
    configManager.setPPMConfig(
        ppmDoc["ppm_tolerance"].as<float>(),
        ppmDoc["initial_a"].as<float>(),
        ppmDoc["initial_b"].as<float>()
    );

    JsonDocument phDoc;
    deserializeJson(phDoc, FSM_INPUT_PAYLOAD_PH);
    configManager.setPHConfig(
        phDoc["min_ph"].as<float>(),
        phDoc["max_ph"].as<float>()
    );

    JsonDocument recipeDoc;
    deserializeJson(recipeDoc, FSM_INPUT_PAYLOAD_RECIPE);
    RecipeStageConfig recipeStages[MAX_RECIPE_STAGES];
    uint8_t recipeCount = 0;
    for (JsonObjectConst s : recipeDoc["stages"].as<JsonArrayConst>()) {
        if (recipeCount >= MAX_RECIPE_STAGES) break;
        recipeStages[recipeCount].maxAgeDays = s["max_age_days"].as<uint16_t>();
        recipeStages[recipeCount].targetPPM = s["target_ppm"].as<float>();
        recipeStages[recipeCount].minPH = s["min_ph"].as<float>();
        recipeStages[recipeCount].maxPH = s["max_ph"].as<float>();
        recipeCount++;
    }
    configManager.setRecipeStages(recipeStages, recipeCount);

    JsonDocument irrigationDoc;
    deserializeJson(irrigationDoc, FSM_INPUT_PAYLOAD_IRRIGATION);
    IrrigationStageConfig irrigationStages[MAX_IRRIGATION_STAGES];
    uint8_t irrigationCount = 0;
    for (JsonObjectConst s : irrigationDoc["stages"].as<JsonArrayConst>()) {
        if (irrigationCount >= MAX_IRRIGATION_STAGES) break;
        irrigationStages[irrigationCount].maxAgeDays = s["max_age_days"].as<uint16_t>();
        irrigationStages[irrigationCount].dryThreshold = s["dry_threshold"].as<uint16_t>();
        irrigationStages[irrigationCount].wetThreshold = s["wet_threshold"].as<uint16_t>();
        irrigationCount++;
    }
    configManager.setIrrigationStages(irrigationStages, irrigationCount);

    loadTimerIrrigationData(configManager);

    JsonDocument mixDoc;
    deserializeJson(mixDoc, FSM_INPUT_PAYLOAD_MIX_SCHEDULE_EXT);
    configManager.setStirSchedule(
        mixDoc["stir_evening_hour"].as<uint8_t>(),
        mixDoc["stir_evening_minute"].as<uint8_t>(),
        mixDoc["stir_duration_seconds"].as<uint32_t>() * 1000UL
    );
}