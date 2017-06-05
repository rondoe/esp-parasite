#include "Arduino.h"
#include <OneWire.h>



#include <ESP8266WiFi.h>          //https://github.com/esp8266/Arduino
#include <ESP8266httpUpdate.h>
#include <WiFiManager.h>          //https://github.com/tzapu/WiFiManager
#include <PubSubClient.h>

#include "Timer.h"

// OneWire DS18S20, DS18B20, DS1822 Temperature Example
//
// http://www.pjrc.com/teensy/td_libs_OneWire.html
//
// The DallasTemperature library can do all this work for you!
// http://milesburton.com/Dallas_Temperature_Control_Library

OneWire ds(4, 14);   // on pin 10 (a 4.7K resistor is necessary)
WiFiClient espClient;
PubSubClient client(espClient);

#define MQTT_SERVER "192.168.1.5"

#include <ESP8266httpUpdate.h>
#include "Timer.h"
Timer t;
Timer temperatureTimer;


void checkUpdate() {
        Serial.println("Check for updates");
        t_httpUpdate_return ret = ESPhttpUpdate.update("https://upthrow.rondoe.com/update/esp?token=xhestcal842myu1ewrupfgpxsy1b352cq2psfohs2nrdxmvsxu0036pw9tdh5f4b", "", "A8 A8 4C 5D C7 5D 03 BB 1A 4C 47 13 B9 33 4C A6 4D AF 4A 63");

        switch(ret) {
        case HTTP_UPDATE_FAILED:
                Serial.printf("HTTP_UPDATE_FAILD Error (%d): %s", ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
                Serial.println();
                break;

        case HTTP_UPDATE_NO_UPDATES:
                Serial.println("HTTP_UPDATE_NO_UPDATES");
                break;

        case HTTP_UPDATE_OK:
                Serial.println("HTTP_UPDATE_OK");
                break;
        }
}



void reconnect() {
        int i=0;
        // Loop until we're reconnected
        while (!client.connected() && i<3) {
                Serial.print("Attempting MQTT connection...");
                // Create a random client ID
                String clientId = "ESP8266DallasParasite";
                clientId += String(random(0xffff), HEX);
                // Attempt to connect
                if (client.connect(clientId.c_str())) {
                        Serial.println("connected to mqtt");
                } else {
                        i++;
                        // Wait 5 seconds before retrying
                        delay(5000);
                }
        }
        // reset if not connected to mqtt
        if(!client.connected()) {
                //reset and try again, or maybe put it to deep sleep
                ESP.reset();
        }
}




int idx = 0;
bool readAllSensors() {
        byte i;
        byte present = 0;
        byte type_s;
        byte data[12];
        byte addr[8];
        float celsius, fahrenheit;

        if ( !ds.search(addr)) {
                Serial.println("No more addresses.");
                Serial.println();
                ds.reset_search();
                delay(1000);
                idx=0;
                return true;
        }

        Serial.print("ROM =");
        for( i = 0; i < 8; i++) {
                Serial.write(' ');
                Serial.print(addr[i], HEX);
        }

        if (OneWire::crc8(addr, 7) != addr[7]) {
                Serial.println("CRC is not valid!");
                return false;
        }
        Serial.println();

        // the first ROM byte indicates which chip
        switch (addr[0]) {
        case 0x10:
                Serial.println("  Chip = DS18S20"); // or old DS1820
                type_s = 1;
                break;
        case 0x28:
                Serial.println("  Chip = DS18B20");
                type_s = 0;
                break;
        case 0x22:
                Serial.println("  Chip = DS1822");
                type_s = 0;
                break;
        default:
                Serial.println("Device is not a DS18x20 family device.");
                return false;
        }

// http://www.cupidcontrols.com/2014/10/moteino-arduino-and-1wire-optimize-your-read-for-speed/
        /*byte resbyte = 0x3F;
           // Set configuration
           ds.reset();
           ds.select(addr);
           ds.write(0x4E); // Write scratchpad
           ds.write(0);    // TL
           ds.write(0);    // TH
           ds.write(resbyte); // Configuration Register

           ds.write(0x48); // Copy Scratchpad*/


        ds.reset();
        ds.select(addr);
        ds.write(0x44, 1); // start conversion, with parasite power on at the end
        delay(1000); // maybe 750ms is enough, maybe not
        // we might do a ds.depower() here, but the reset will take care of it.

        present = ds.reset();
        ds.select(addr);
        ds.write(0xBE); // Read Scratchpad

        Serial.print("  Data = ");
        Serial.print(present, HEX);
        Serial.print(" ");
        for ( i = 0; i < 9; i++) { // we need 9 bytes
                data[i] = ds.read();
                Serial.print(data[i], HEX);
                Serial.print(" ");
        }
        Serial.print(" CRC=");
        Serial.print(OneWire::crc8(data, 8), HEX);
        Serial.println();

        // Convert the data to actual temperature
        // because the result is a 16 bit signed integer, it should
        // be stored to an "int16_t" type, which is always 16 bits
        // even when compiled on a 32 bit processor.
        int16_t raw = (data[1] << 8) | data[0];
        if (type_s) {
                raw = raw << 3; // 9 bit resolution default
                if (data[7] == 0x10) {
                        // "count remain" gives full 12 bit resolution
                        raw = (raw & 0xFFF0) + 12 - data[6];
                }
        } else {
                byte cfg = (data[4] & 0x60);
                // at lower res, the low bits are undefined, so let's zero them
                if (cfg == 0x00) raw = raw & ~7; // 9 bit resolution, 93.75 ms
                else if (cfg == 0x20) raw = raw & ~3; // 10 bit res, 187.5 ms
                else if (cfg == 0x40) raw = raw & ~1; // 11 bit res, 375 ms
                //// default is 12 bit resolution, 750 ms conversion time
        }
        celsius = (float)raw / 16.0;
        fahrenheit = celsius * 1.8 + 32.0;
        Serial.print("  Temperature = ");
        Serial.print(celsius);
        Serial.print(" Celsius, ");
        Serial.print(fahrenheit);
        Serial.println(" Fahrenheit");

        // publish to mqtt
        char mac[sizeof(addr)*3+1];
        sprintf(mac, "%02X:%02X:%02X:%02X:%02X:%02X:%02X:%02X\0", addr[0], addr[1], addr[2], addr[3], addr[4], addr[5], addr[6], addr[7]);

        char topic[60];
        char result[8];       // Buffer big enough for 7-character float
        dtostrf(celsius, 6, 2, result);
        // Once connected, publish an announcement...
        sprintf(topic, "controller/43/%s/V_TEMP", mac);
        client.publish(topic, result);
        idx++;

        delay(1000);
        return false;
}

void readSensors() {
        reconnect();
        while(!readAllSensors()) ;
}


void setup(void) {
        Serial.begin(115200);
        Serial.println("Startup");

        //WiFiManager
        //Local intialization. Once its business is done, there is no need to keep it around
        WiFiManager wifiManager;

        //tries to connect to last known settings
        if (!wifiManager.autoConnect("AutoConnectAP", "password")) {
                Serial.println("failed to connect, we should reset as see if it connects");
                delay(3000);
                ESP.reset();
                delay(5000);
        }

        //if you get here you have connected to the WiFi
        Serial.println("connected...yeey :)");


        Serial.println("local ip");
        Serial.println(WiFi.localIP());

        client.setServer(MQTT_SERVER, 1883);

        checkUpdate();
        // check for software update every minute
        t.every(1000 * 60, checkUpdate);



        readSensors();
        // read sensors every 5 minutes
        temperatureTimer.every(60*5000, readSensors);

}


void loop(void) {
        // update timer
        t.update();
        temperatureTimer.update();
}
