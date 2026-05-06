#pragma once
#include "BGSource.h"

/**
 * BGSourceBLEDirect — source passive utilisée en mode BLE.
 * Les lectures sont poussées directement via push() depuis le callback DexcomBLE.
 * hasNewData() signale au BGDisplayManager qu'une nouvelle lecture est disponible.
 */
class BGSourceBLEDirect : public BGSource {
public:
    void push(const GlucoseReading& reading) {
        glucoseReadings.push_front(reading);
        glucoseReadings = deleteOldReadings(glucoseReadings, 0);
        firstConnectionSuccess = true;
        _hasNew = true;
    }

    std::list<GlucoseReading> updateReadings(std::list<GlucoseReading> existing) override {
        return glucoseReadings;
    }

    bool hasNewData(unsigned long long epochToCompare) override {
        if (_hasNew) { _hasNew = false; return true; }
        return false;
    }

    String getStatus() const override {
        return firstConnectionSuccess ? "BLE OK" : "BLE scan...";
    }

private:
    volatile bool _hasNew = false;
};
