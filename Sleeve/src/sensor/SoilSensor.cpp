#include "SoilSensor.h"

SoilSensor::SoilSensor(
    uint8_t pin1,
    uint8_t pin2,
    uint8_t pin3,
    uint8_t pin4
)
{
    _pins[0] = pin1;
    _pins[1] = pin2;
    _pins[2] = pin3;
    _pins[3] = pin4;

    _data = {};
}

void SoilSensor::begin() {
    // analogRead() siap dipakai langsung
}

void SoilSensor::update() {
    uint16_t raw[4];

    // Baca tiap sensor: median filter dulu, lalu masuk moving average
    for (uint8_t i = 0; i < 4; i++) {
        uint16_t med = _median.sample(_pins[i]);
        _ma[i].add(med);
        raw[i] = _ma[i].get();
    }

    _data.sensor1 = raw[0];
    _data.sensor2 = raw[1];
    _data.sensor3 = raw[2];
    _data.sensor4 = raw[3];

    // Sensor 4 (s4) rusak hardware — dikecualikan dari rata-rata kelembaban.
    // Rata-rata dihitung dari s1 + s2 + s3 saja.
    uint32_t sum = (uint32_t)raw[0] + raw[2] + raw[3];
    _data.averageRawADC = (uint16_t)(sum / 3);
}

SoilData SoilSensor::getData() const {
    return _data;
}
