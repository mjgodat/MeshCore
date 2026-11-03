#include "ThinknodeM5Board.h"

PCA9557 expander (0x18, &Wire1);

void ThinknodeM5Board::begin() {
    // Start expander and configure pins
    Wire1.begin(48, 47);
    expander.pinMode(EXP_PIN_POWER, OUTPUT); // eink
    expander.pinMode(EXP_PIN_BACKLIGHT, OUTPUT); // peripherals
    expander.pinMode(EXP_PIN_LED, OUTPUT); // peripherals
    expander.digitalWrite(EXP_PIN_POWER, HIGH);
    expander.digitalWrite(EXP_PIN_BACKLIGHT, LOW);
    expander.digitalWrite(EXP_PIN_LED, LOW);

    // Hardware revision strap (expander IO7): HIGH = v2.0 (AG3352 GPS),
    // LOW = v1.0 (L76K). Sample a few times; the strap is a weak pull.
    expander.pinMode(EXP_PIN_HW_VERSION, INPUT);
    delay(2);
    uint8_t high = 0;
    for (uint8_t i = 0; i < 10; i++) high += expander.digitalRead(EXP_PIN_HW_VERSION) == HIGH;
    _hw_v2 = high >= 3;

#ifdef PIN_GPS_SWITCH
    pinMode(gpsSwitchPin(), INPUT);
#endif

    ESP32Board::begin();
  }

 uint16_t ThinknodeM5Board::getBattMilliVolts() {
    analogReadResolution(12);
    analogSetPinAttenuation(PIN_VBAT_READ, ADC_11db);

    uint32_t mv = 0;
      for (int i = 0; i < 8; ++i) {
        mv += analogReadMilliVolts(PIN_VBAT_READ);
        delayMicroseconds(200);
      }
    mv /= 8;

    analogReadResolution(10);  
    return calibrateBatteryMillivolts(static_cast<uint16_t>(mv * ADC_MULTIPLIER ));
}

  const char* ThinknodeM5Board::getManufacturerName() const {
    return "Elecrow ThinkNode M5";
  }
