/*
 * ESP32 based BLE beacon for energy meters
 *
 * See https://github.com/oh2mp/esp32_energymeter
 *
 */
 
#include "BLEDevice.h"
#include "BLEUtils.h"
#include "BLEBeacon.h"

extern "C" {
#include "esp_partition.h"
#include "esp_err.h"
#include "nvs_flash.h"
#include "nvs.h"
}

#define LED 2                   // Onboard led pin on devkit
#define PULSE 13                // Pin where meter S0+ is connected
#define RESETBUTTON 15          // Pin where reset button is connected
#define PULSE_FACTOR 1000       // Number of blinks per kwh of your meter. Normally 1000.
#define MAX_WATT 3680           // Theoretical max watt value you will consume (230V@16A=3680W) for filtering.
#define SEND_FREQ 5000          // milliseconds how often to advertise
#define SAVE_FREQ 60000         // milliseconds how often save pulse counter to flash (see README)

nvs_handle nvsh;
double ppwh = ((double)PULSE_FACTOR) / 1000; // Pulses per watt hour
volatile uint32_t pulse_total = 0;
volatile uint32_t pulse_reset = 0;
volatile uint32_t last_pulse = 0;
uint32_t old_pulse_total = 0;
uint32_t savetime_pulse_total = 0;
float kwh;
float old_kwh;
float curr_kwh;
uint32_t last_sent = 0;
uint32_t last_saved = 0;
// Sometimes we get pulse interrupt on wrong edge. Calculate theoretical minimum pulse time to filter them out.
const uint32_t min_pulsetime = int(MAX_WATT/3600.0/PULSE_FACTOR*10000)*100;
bool buttonpressed = false;
uint32_t buttontime = 0;

BLEAdvertising *advertising;
std::string mfdata = "";

/* ----------------------------------------------------------------------------------
 * Set up data packet for advertising
 */ 
void set_beacon() {
    BLEBeacon beacon = BLEBeacon();
    BLEAdvertisementData advdata = BLEAdvertisementData();
    BLEAdvertisementData scanresponse = BLEAdvertisementData();
    
    advdata.setFlags(0x06); // BR_EDR_NOT_SUPPORTED 0x04 & LE General discoverable 0x02

    uint32_t wh = int(kwh*1000);
    
    mfdata = "";
    mfdata += (char)0xE5; mfdata += (char)0x02;  // Espressif Incorporated Vendor ID = 0x02E5
    mfdata += (char)0xDC; mfdata += (char)0xAC;  // Identifier for this sketch is 0xACDC

    mfdata += (char)wh & 0xFF;
    mfdata += (char)(wh >> 8) & 0xFF;
    mfdata += (char)(wh >> 16) & 0xFF;
    mfdata += (char)(wh >> 24) & 0xFF;

    wh = int(curr_kwh*1000);
    mfdata += (char)wh & 0xFF;
    mfdata += (char)(wh >> 8) & 0xFF;
    mfdata += (char)(wh >> 16) & 0xFF;
    mfdata += (char)(wh >> 24) & 0xFF;
    
    mfdata += (char)0xBE; mfdata += (char)0xEF;  // Beef is always good nutriment
  
    advdata.setManufacturerData(mfdata);
    advertising->setAdvertisementData(advdata);
    advertising->setScanResponseData(scanresponse);
}
/* ---------------------------------------------------------------------------------- */
// Initialize ESP non-volatile storage (NVS)
bool nvs_init() {
    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        Serial.println("NVS init failed");
        if (err != ESP_ERR_NVS_NO_FREE_PAGES) return false;

        Serial.println("Reformat NVS");
        const esp_partition_t *nvs_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, NULL);
        if (nvs_partition == NULL)
            return false;
        err = esp_partition_erase_range(nvs_partition, 0, nvs_partition->size);
        err = nvs_flash_init();
        if (err)
        return false;
        Serial.println("NVS-partition re-formatted");
        err = nvs_set_u32(nvsh, "pulsetotal", (uint32_t)pulse_total);
        err = nvs_set_u32(nvsh, "pulsereset", (uint32_t)pulse_reset);
        err = nvs_commit(nvsh);        
    }

    err = nvs_open("nvs", NVS_READWRITE, &nvsh);
    if (err != ESP_OK) return false;

    return true;
}        
  
/* ---------------------------------------------------------------------------------- */
void ICACHE_RAM_ATTR onPulse() {
    uint32_t now = micros();
    if (now - last_pulse < min_pulsetime) return; // Sometimes we get interrupt on wrong edge
    last_pulse = now;
    pulse_total++;
}
/* ---------------------------------------------------------------------------------- */
void blinker() {
    for (int i = 0; i < 5; i++) {
         digitalWrite(LED, HIGH);
         delay(100);
         digitalWrite(LED, LOW);
         delay(100);
    }
}
/* ---------------------------------------------------------------------------------- */
void setup() {
    pinMode(LED, OUTPUT);
    digitalWrite(LED, LOW);   // LED off
    
    pinMode(PULSE, INPUT_PULLUP);
    attachInterrupt(PULSE, onPulse, RISING);

    pinMode(RESETBUTTON, INPUT_PULLUP);

    Serial.begin(115200);
    Serial.println("ESP32 energymeter");
    
    nvs_init();
    uint32_t foo;    // pulse_total and reset are volatile, so we must do it this way
    esp_err_t err = nvs_get_u32(nvsh, "pulsetotal", &foo);
    if (err == ESP_OK) {pulse_total = foo;}
    err = nvs_get_u32(nvsh, "pulsereset", &foo);
    if (err == ESP_OK) {pulse_reset = foo;}
    
    BLEDevice::init("ESP32+energymeter");
    advertising = BLEDevice::getAdvertising();
    
    last_sent = millis();

    // fake values for debugging
    // pulse_total = 900;
    // pulse_reset = 0;
}
 
/* ---------------------------------------------------------------------------------- */
void loop() {
    if (digitalRead(RESETBUTTON) == LOW) {
        if (buttonpressed == false) {
            buttonpressed = true;
            buttontime = millis();
        }
        if (millis() - buttontime > 5000 && buttonpressed == true) {
            blinker();
            pulse_reset = pulse_total;
            Serial.printf("Reset. Pulse counter = %d\n",pulse_reset);
            esp_err_t err = nvs_set_u32(nvsh, "pulsetotal", (uint32_t)pulse_total);
            err = nvs_set_u32(nvsh, "pulsereset", (uint32_t)pulse_reset);
            err = nvs_commit(nvsh);        
            ESP.restart();
        }
    } else {
        buttonpressed = false;
    }
    
    if (millis() - last_sent > SEND_FREQ) {
        if (pulse_total != old_pulse_total) {
            kwh = pulse_total / (float)PULSE_FACTOR;
            old_pulse_total = pulse_total;
            if (kwh != old_kwh) {
                old_kwh = kwh;
            }
            curr_kwh = (pulse_total - pulse_reset) / (float)PULSE_FACTOR;
        }
        last_sent = millis();

        Serial.printf("Pulsetotal:%d Lastreset:%d kWh tot:%.1f kWh: %.1f\n", pulse_total, pulse_reset, kwh, curr_kwh);
        
        set_beacon();
        digitalWrite(LED, HIGH);   // LED on during the advertising
        advertising->start();
        delay(100);
        advertising->stop();
        digitalWrite(LED, LOW);   // LED off
    }
        
    if (millis() - last_saved > SAVE_FREQ) {
        if (savetime_pulse_total != pulse_total) {
            savetime_pulse_total = pulse_total;
            esp_err_t err = nvs_set_u32(nvsh, "pulsetotal", (uint32_t)pulse_total);
            err = nvs_set_u32(nvsh, "pulsereset", (uint32_t)pulse_reset);
            err = nvs_commit(nvsh);
            last_saved = millis();
            Serial.printf("Saved pulse count to NVS: %d\n",pulse_total);
        }
    }

    // Reboot once in hour to be sure and prevent micros() to overflow at about 70 minutes
    if (millis() > 3.6E+6) {
        esp_err_t err = nvs_set_u32(nvsh, "pulsetotal", (uint32_t)pulse_total);
        err = nvs_set_u32(nvsh, "pulsereset", (uint32_t)pulse_reset);
        err = nvs_commit(nvsh);        
        ESP.restart();
    }
}

/* ---------------------------------------------------------------------------------- */
 
