#include <SPI.h>
#include <WiFiUdp.h>
#include "secrets.h"
#include <NTPClient.h>
#include <MD_Parola.h>
#include <MD_MAX72xx.h>
#include <ESP8266WiFi.h>
#include <Arduino_JSON.h>
#include <ArduinoHttpClient.h>

/**
 * MQTT is used to receive alert messages. To remove support comment out #define MQTT. 
 */
//#define MQTT

#ifdef MQTT
#include <PubSubClient.h>
#endif

/**
 * Device name for WiFi hostname and MQTT prefix. 
 */
const char* deviceName = "pixelpanel";

/**
 * Configuration for Max7219. Change according to your hardware. 
 */
#define HARDWARE_TYPE MD_MAX72XX::FC16_HW     
#define MAX_DEVICES 8
#define CS_PIN      D4
#define CLK_PIN     13
#define DATA_PIN    11

MD_Parola P = MD_Parola(HARDWARE_TYPE, CS_PIN, MAX_DEVICES); // Hardware SPI
//MD_Parola P = MD_Parola(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES); // Software SPI

/**
 * Display intensity (0-15) for normal clock display and for alerts. 
 */
int INTENSITY_NORM = 5;
int INTENSITY_ALERT = 15;

/**
 * List of timezones to be displayed. 
 * 
 * IMPORTANT: Both array must be of the same legnth!
 * 
 * timezones[] -      Lookup the relevant timezone from http://worldtimeapi.org/api/timezone. 
 *                    This can be a partial string, but make sure it's unique. 
 * timezoneLabels[] - The label to be displayed. Order must be same as in timezones[].         
 */
char* timezones[] = { 
  "Asia/Jerusalem", 
  "Africa/Nairobi",
  "Asia/Kolkata",
  "Europe/London",
  "PST8PDT",
  "Asia/Hong_Kong",
  "Europe/Berlin"
};

char* timezoneLabels[] = { 
  "Home", 
  "Nairobi", 
  "BLR", 
  "London", 
  "Bellevue", 
  "China", 
  "Munich" 
};

/**
 * Internal stuff from here on. No need to edit.
 */
 
WiFiClient WiFiclient;
const char* ssid = SECRET_SSID;
const char* password = SECRET_PASS;
char displayBuffer[40] = "Powered by worldtimeapi.org";
#ifdef MQTT
PubSubClient client(WiFiclient);
#endif
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP, "pool.ntp.org", 7200);
HttpClient http = HttpClient(WiFiclient, "worldtimeapi.org", 80);
unsigned long previousMillis = 0;
int utcOffsetInSeconds[ARRAY_SIZE(timezoneLabels)];
int curTimezone = 0;

void setup() {
  Serial.begin(9600);
  delay(10);
  Serial.println();
  Serial.print("Connecting to ");
  Serial.println(ssid);

#ifdef MQTT
  client.setServer(MQTT_HOSTNAME, 1883);
  client.setCallback(callback);
#endif

  WiFi.mode(WIFI_STA);
  WiFi.hostname(deviceName);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("WiFi connected");
  Serial.println("IP address: ");
  Serial.println(WiFi.localIP());

  timeClient.begin();
  timeClient.update();

  P.begin();
  P.setIntensity(3);
  P.displayText(displayBuffer, PA_CENTER, 50, 2500, PA_SCROLL_LEFT, PA_SCROLL_LEFT);
  
  initOffsets();

  delay(2000);
}

void initOffsets() {
  for (int i = 0; i < ARRAY_SIZE(timezoneLabels); i++) {
    utcOffsetInSeconds[i] = -1;
  }
}

#ifdef MQTT
void connectMqtt() {
  // Loop until connected
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    // Attempt to connect
    if (client.connect(deviceName)) {
      Serial.println(" connected");
      
      // Subscribe to topic
      char charMsg[255];
      String topic = "/message/command";
      topic = deviceName + topic;
      topic.toCharArray(charMsg, 255);
      client.subscribe(charMsg);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message received for topic '"); Serial.print(topic); Serial.print("'. Payload '");
  char* payloadStr = (char*)malloc(length + 1);
  memcpy(payloadStr, (const char *)payload, length);
  payloadStr[length] = 0;
  Serial.print(payloadStr); Serial.println("'");
  P.displayClear();
  P.displayText(payloadStr, PA_CENTER, 50, 3500, PA_SCROLL_LEFT, PA_SCROLL_DOWN);
  P.setIntensity(INTENSITY_ALERT);
}
#endif

void initTimezone(int index) {
  String httpCommand = "/api/timezone/";
  httpCommand.concat(timezones[index]);
  Serial.print("HTTP Command: "); Serial.println(httpCommand);
  http.get(httpCommand);
  int statusCode = http.responseStatusCode();
  Serial.print("Response code: "); Serial.println(statusCode);
  if (statusCode < 0) {
    sprintf(displayBuffer, "Error: %d", statusCode);
  } else {
    String response = http.responseBody();
    Serial.print("Response: "); Serial.println(response);
    JSONVar jsonObj = JSON.parse(response);
    if (jsonObj.hasOwnProperty("error")) {
      String errorMsg = JSON.stringify(jsonObj["error"]);
      sprintf(displayBuffer, "%s", errorMsg.c_str());
    } else if (jsonObj.hasOwnProperty("raw_offset")) {
      int raw = int(jsonObj["raw_offset"]);
      int dst = int(jsonObj["dst_offset"]);
      utcOffsetInSeconds[index] = raw + dst;
    } else {
      sprintf(displayBuffer, "Invalid response.");
    }
  }
}

void loop() {
#ifdef MQTT
  if (!client.connected()) {
    connectMqtt();
  }
  client.loop();
#endif

  // Reset utcOffsetInSeconds every 24 hours will force refresh to account for DST updates. 
  if ((unsigned long)(millis() - previousMillis) >= 24 * 60 * 60 * 1000) {
    initOffsets();  
    previousMillis = millis();
  }
   
  if(P.displayAnimate()) {
    if (utcOffsetInSeconds[curTimezone] == -1) {
      initTimezone(curTimezone);
    }
    if (utcOffsetInSeconds[curTimezone] != -1) {
      timeClient.setTimeOffset(utcOffsetInSeconds[curTimezone]);
      timeClient.update();
      sprintf(displayBuffer, "%s %02d:%02d", timezoneLabels[curTimezone], timeClient.getHours(), timeClient.getMinutes()); 
    }
    P.setIntensity(INTENSITY_NORM);
    P.displayText(displayBuffer, PA_CENTER, 50, 4500, PA_SCROLL_DOWN, PA_SCROLL_DOWN);
    curTimezone = (curTimezone + 1) % ARRAY_SIZE(utcOffsetInSeconds);  
  }  
}
