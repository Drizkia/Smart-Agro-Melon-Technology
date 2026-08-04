#include "TDSSensor.h"

namespace {
constexpr float FALLBACK_TEMP_C  = 25.0f;
constexpr float MIN_VALID_TEMP_C = 5.0f;   // Air fertigasi melon tidak mungkin < 5C, cegah 0.0C uninitialized
constexpr float MAX_VALID_TEMP_C = 60.0f;

// Kalibrasi TDS DFRobot secara software.
// Rumus K-Factor: (Nilai Asli TDS Manual) / (Raw TDS DFRobot = EC * 0.5)
// Kalibrasi fisik: Manual = 520 PPM -> TDS_K_VALUE = 1.078f
constexpr float TDS_K_VALUE = 1.078f;
}

TDSSensor::TDSSensor(uint8_t pin) {
    _pin = pin; 
}

void TDSSensor::begin() {}

float TDSSensor::readPPM(float waterTemp) {
    if (waterTemp < MIN_VALID_TEMP_C || waterTemp > MAX_VALID_TEMP_C) {
        waterTemp = FALLBACK_TEMP_C;
    }

    const int samples = 30;
    float rawVoltages[samples];

    for (int i = 0; i < samples; i++) {
        uint16_t adc = analogRead(_pin);
        rawVoltages[i] = adc * 3.3f / 4095.0f;
        delayMicroseconds(200);
    }

    // Median Filter: Urutkan sampel untuk membuang noise spike ekstrim
    for (int i = 0; i < samples - 1; i++) {
        for (int j = i + 1; j < samples; j++) {
            if (rawVoltages[i] > rawVoltages[j]) {
                float tmp = rawVoltages[i];
                rawVoltages[i] = rawVoltages[j];
                rawVoltages[j] = tmp;
            }
        }
    }

    // Ambil rata-rata 10 sampel di area tengah (index 10..19)
    float medianVoltage = 0.0f;
    for (int i = 10; i < 20; i++) {
        medianVoltage += rawVoltages[i];
    }
    medianVoltage /= 10.0f;

    float compensationCoefficient = 1.0f + 0.02f * (waterTemp - 25.0f);
    if (compensationCoefficient <= 0.0f) {
        compensationCoefficient = 1.0f;
    }

    float compensationVoltage = medianVoltage / compensationCoefficient;

    // Polinomial DFRobot menghasilkan Electrical Conductivity (EC dalam uS/cm)
    float ecValue = 
        (133.42f * compensationVoltage * compensationVoltage * compensationVoltage)
        -
        (255.86f * compensationVoltage * compensationVoltage)
        +
        (857.39f * compensationVoltage);

    // DFRobot standard conversion: TDS (PPM) = EC * 0.5
    float rawTdsPPM = ecValue * 0.5f;

    return rawTdsPPM * TDS_K_VALUE;
}


