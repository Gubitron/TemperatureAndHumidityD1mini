// Libraries used:
// WiFiManager
// ArduinoJson
// DallasTemperature
// DHT Sensor Library

// Config storage
#include <FS.h> 
#ifdef ESP32
  #include <SPIFFS.h>
#endif
#include <ArduinoJson.h>

// Wifi Libraries
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

// Temp Sensor Libraries
#include <OneWire.h>
#include <DallasTemperature.h>

// WifiManager stuff
#include <WiFiManager.h>
#include <DNSServer.h>
#include <ESP8266WebServer.h>

// Humidity and Temp sensor Libraries
#include "DHT.h"

#define Node_Index 4  // Define node index for calibration and identifier

#define ONE_WIRE_BUS 5 // D1 is 5 on D1 mini

#define DHT_PIN 4 // DIO connected to DHT sensor

#define DHT_TYPE DHT11   // DHT 11
//#define DHT_TYPE DHT22   // DHT 22  (AM2302), AM2321
//#define DHT_TYPE DHT21   // DHT 21 (AM2301)

// DHT Sensor
// Connect pin 1 (on the left) of the sensor to +5V
// NOTE: If using a board with 3.3V logic like an Arduino Due connect pin 1
// to 3.3V instead of 5V!
// Connect pin 2 of the sensor to whatever your DHTPIN is
// Connect pin 4 (on the right) of the sensor to GROUND
// Connect a 10K resistor from pin 2 (data) to pin 1 (power) of the sensor

static char temporary1[8+8+4+4+8+8+8]; // "%#7.5f,%#7.5f,%.4d,%#7.5f,%#7.5f,%#7.5f"
static char temporary2[20];      // 20 char string

IPAddress broadcastIP=IPAddress(255, 255, 255, 255); // Address to broadcast to all available devices
unsigned int localPort = 50000; // local port to write on

// Buffers for UDP communication
char receiveBuffer[UDP_TX_PACKET_MAX_SIZE+1]; // MAX UDP packet size plus NULL character
char sendBuffer[UDP_TX_PACKET_MAX_SIZE+1];    // MAX UDP packet size plus NULL character

WiFiUDP UDP;

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);
DHT dht(DHT_PIN, DHT_TYPE);

// WiFiManager stuff
WiFiServer server(80);
char NodeId[20] = "NodeDefault";
char CalibrationString[80] = "1,0,1,0";

static float voltageCalibrationSlope = 1;
static float voltageCalibrationIntercept = 0;
static float temperatureCalibrationSlope = 1;
static float temperatureCalibrationIntercept = 0;

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup() 
{
  Serial.begin(115200);

  //clean FS, for testing
  //SPIFFS.format();

  Serial.println("Mounting file system...");
  if (SPIFFS.begin())
  {
    if (SPIFFS.exists("/config.json"))
    {
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile)
      {
        size_t size = configFile.size();
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);

#if defined(ARDUINOJSON_VERSION_MAJOR) && ARDUINOJSON_VERSION_MAJOR >= 6
        DynamicJsonDocument json(1024);
        auto deserializeError = deserializeJson(json, buf.get());
        serializeJson(json, Serial);
        if ( ! deserializeError ) 
        {
#else
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success())
        {
#endif
          Serial.println("\nParsed config");
          strcpy(NodeId, json["nodeId"]);
          strcpy(CalibrationString, json["calibrationString"]);
        }
        else
        {
          Serial.println("Failed to load json config.");
        }
        configFile.close();
      }
    }
  }
  else
  {
    Serial.println("Failed to mount file system!");
  }
  
  WiFiManagerParameter nodeId("nodeId", "Node ID", NodeId, 20);
  WiFiManagerParameter calibrationString("calibrationString", "Calibration String", CalibrationString, 80);
  
  WiFiManager wifiManager;
  wifiManager.setSaveConfigCallback(saveConfigCallback);
  
  wifiManager.addParameter(&nodeId);
  wifiManager.addParameter(&calibrationString);

  //reset settings - for testing
  //wifiManager.resetSettings();
  
  if (!wifiManager.autoConnect("SensorConfig","test1234")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again
    ESP.restart();
    delay(5000);
  }

  strcpy(NodeId, nodeId.getValue());
  strcpy(CalibrationString, calibrationString.getValue());
  Serial.println("Settings in the file are:");
  Serial.println("\tnodeId : " + String(NodeId));
  Serial.println("\tcalibrationString : " + String(CalibrationString));

  if (shouldSaveConfig)
  {
    //save the custom parameters to FS
    Serial.println("Saving config");
#if defined(ARDUINOJSON_VERSION_MAJOR) && ARDUINOJSON_VERSION_MAJOR >= 6
    DynamicJsonDocument json(1024);
#else
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
#endif
    json["nodeId"] = NodeId;
    json["calibrationString"] = CalibrationString;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) 
    {
      Serial.println("Failed to open config file for writing");
    }

#if defined(ARDUINOJSON_VERSION_MAJOR) && ARDUINOJSON_VERSION_MAJOR >= 6
    serializeJson(json, Serial);
    serializeJson(json, configFile);
#else
    json.printTo(Serial);
    json.printTo(configFile);
#endif
    configFile.close();
  }

  // Let developer know the assigned IP address for device
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());
  Serial.printf("UDP Server on port %d\n", localPort);
  UDP.begin(localPort);

  // Initialise 1-wire sensors
  sensors.begin();
  dht.begin();
}

void loop() 
{
  // DHT read (need to wait at least 2000ms between measurements)
  // Measurements take about 250ms to complete, readings might be 2000ms old
  float h = dht.readHumidity(); // Range: 0-100%, Resolution: 0.10%
  float t = dht.readTemperature(false); // F(default) - Celsius, T - Fahrenheit. Range: -40-125C, Resolution: 0.1C
  float hInd = dht.computeHeatIndex(t, h, false); // F(default) - Celsius , T - Fahrenheit.
  
  // Check if any reads failed and exit early (to try again).
  if (isnan(h) || isnan(t) || isnan(hInd))
  {
    if (isnan(h)) h = 0;
    if (isnan(t)) t = 0;
    if (isnan(hInd)) hInd = 0;

    Serial.println(F("Failed to read from DHT sensor!"));
  }
  
  
  // Read 1-wire sensors and print the reading of sensor 0
  sensors.requestTemperatures();              // get temperature for all sensors
  float tempC = sensors.getTempCByIndex(0);   // get temperature of the first sensor in returned temperature array

  if(tempC != DEVICE_DISCONNECTED_C)
  {
   //Serial.println(tempC);
   //Read AI value
   int rawVolt;

    rawVolt = analogRead(A0);
    //Serial.println(rawVolt);

   float voltage = (rawVolt * voltageCalibrationSlope)+voltageCalibrationIntercept;

    // Put temperature value in sendBuffer of UDP packet
    // Structure it as: tempC,Voltage,rawVoltage
    char format[] = "%#7.5f,%#7.5f,%.4d,%#7.5f,%#7.5f,%#7.5f,";

    sendBuffer[0] = '\0';                               // Clear sendBuffer before writting data
    tempC = (tempC * temperatureCalibrationSlope)+temperatureCalibrationIntercept;

    temporary1[0] = '\0';
    sprintf(temporary1,format,tempC,voltage,rawVolt,h,t,hInd);   // Format first part of message   
    temporary2[0] = '\0';                        
    strcpy(temporary2,NodeId);                                

    // Combine messages
    strcpy(sendBuffer,temporary1);
    strcat(sendBuffer,temporary2);

    // DEBUG
/*
    Serial.print("Temp1: ");
    Serial.print(tempC);
    Serial.print("; Voltage: ");
    Serial.print(voltage);
    Serial.print("; Raw Voltage: ");
    Serial.println(rawVolt);
    
    Serial.print("Temp2: ");
    Serial.print(t);
    Serial.print("; Humidity: ");
    Serial.print(h);
    Serial.print("; Heat Index: ");
    Serial.println(hInd);
    Serial.print("UDP Message: ");
    Serial.println(sendBuffer);
*/
    
    // Compose Packet
    // Use IP and Port of the received packet to send a reply
    UDP.beginPacket(broadcastIP,localPort);
    UDP.write(sendBuffer);
    UDP.endPacket();
  }
  else
  {
    Serial.println("Error: Could not read temperature data");
  }

  delay(150);          // Need to allow time for sending packet out
  ESP.deepSleep(15e6); // 60 seconds deep sleep
  //delay(2000);
}
