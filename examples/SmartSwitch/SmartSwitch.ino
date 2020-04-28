/*
SmartSwitch application
Based on ESP_AsyncFSBrowser
Temperature Control for heater with schedule
Main purpose - for winter outside car block heater or battery charger 
Wide browser compatibility, no other server-side needed.
HTTP server and WebSocket, single port  
Standalone, no JS dependencies for the browser from Internet (I hope)
Based on ESP_AsyncFSBrowser
Real Time (NTP) w/ Time Zones
Memorized settings to EEPROM
Multiple clients can be connected at same time, they see each other requests
Use latest ESP core lib (from Github)
*/

#define USE_WFM   // to use ESPAsyncWiFiManager
//#define DEL_WFM   // delete Wifi credentials stored 
                  //(use once then comment and flash again), also HTTP /erase-wifi can do the same live
                  
#define USE_AUTH  // .setAuthentication for all static

#include <ArduinoOTA.h>
#ifdef ESP32
 #include <FS.h>
 #include <SPIFFS.h>
 #include <ESPmDNS.h>
 #include <WiFi.h>
 #include <AsyncTCP.h>
#elif defined(ESP8266)
 #include <ESP8266WiFi.h>
 #include <ESPAsyncTCP.h>
 #include <ESP8266mDNS.h>
#endif
#include <ESPAsyncWebServer.h>
#ifdef USE_WFM 
 #include "ESPAsyncWiFiManager.h"
#endif 
#include <SPIFFSEditor.h>
#include <EEPROM.h>
#include <Ticker.h>
#include <DHT.h>

#define RTC_UTC_TEST 1577836800 // Some Date
#ifdef ESP32
 #define MYTZ -5*3600,3600
#elif defined(ESP8266)
 #include <TZ.h>
 #define MYTZ TZ_America_Toronto  
#endif

#define EESC    100  // fixed eeprom address for sched choice
#define EECH    104  // fixed eeprom address to keep selected active channel, only for reference here 
#define EEBEGIN EECH + 1
#define EEMARK  0x5A
#define MEMMAX  2   // 0,1,2... last max index (only 3 channels)
#define EEALL   512

#define HYST 0.5 // C +/- hysteresis 

// DHT
#define DHTTYPE DHT22  // DHT 11 // DHT 22, AM2302, AM2321 // DHT 21, AM2301
#define DHTPIN 4       //D2 

DHT dht(DHTPIN, DHTTYPE);

// SKETCH BEGIN MAIN DECLARATIONS
Ticker tim;
AsyncWebServer server(80); //single port - easy for forwarding 
AsyncWebSocket ws("/ws");

#ifdef USE_WFM 
 #ifdef USE_EADNS
  AsyncDNSServer dns;
 #else
  DNSServer dns;
 #endif
 
//Fallback timeout in seconds allowed to config or it creates an own AP, then serves 192.168.4.1 
 #define FBTO 120
 const char* fbssid = "FBSSW"; 
 const char* fbpassword = "FBpassword4";
 
#else
 const char* ssid = "MYROUTERSSD";
 const char* password = "MYROUTERPASSWD";
#endif 
const char* hostName = "smartsw";
const char* http_username = "smart"; // for SPIFFSEditor (and static html)
const char* http_password = "switch";

// RTC
static timeval tv;
static time_t now;

// HW I/O
const int btnPin = 0; //D3
const int ledPin = 2; //D4
int btnState = HIGH; 

// Globals
uint8_t count = 0;
uint8_t sched = 0; // automatic schedule
byte memch = 0;    // select memory "channel" to work with 
float t = 0;
float h = 0;
bool udht = false;
bool heat_enabled_prev = false;
int ledState;                     

struct EE_bl { 
  byte memid;  //here goes the EEMARK stamp
  uint8_t hstart;
  uint8_t mstart;
  uint8_t hstop;
  uint8_t mstop;
  float tempe;
};

EE_bl ee = {0,0,0,0,0,0.1}; //populate as initial

// SUBS
void writeEE() {
  ee.memid = EEMARK;
  //EEPROM.put(EESC, sched); // only separately when needed with commit()
  //EEPROM.put(EECH, memch); // not need to store and retrieve memch
  EEPROM.put(EEBEGIN + memch*sizeof(ee), ee); 
  EEPROM.commit(); //needed for ESP8266?
}

void readEE() {
  byte ChkEE;
  if (memch > MEMMAX) memch = 0;
  EEPROM.get(EEBEGIN + memch*sizeof(ee), ChkEE);
  if (ChkEE == EEMARK){ //otherwise stays with defaults
    EEPROM.get(EEBEGIN  + memch*sizeof(ee), ee);
    EEPROM.get(EESC, sched);
    if (sched > MEMMAX + 1) sched = 0;
  }
}

void showTime() 
{
  byte tmpch = 0;
  bool heat_enabled = false; 
  
  gettimeofday(&tv, nullptr);
  now = time(nullptr);
  const tm* tm = localtime(&now);
  ws.printfAll("Now,Clock,%02d:%02d,%d", tm->tm_hour, tm->tm_min, tm->tm_wday);
  if ((2==tm->tm_hour )&&(2==tm->tm_min)) {
    configTime(MYTZ, "pool.ntp.org");
    Serial.print(F("Sync Clock at 02:02\n"));
  }
  Serial.printf("RTC: %02d:%02d\n", tm->tm_hour, tm->tm_min);

  if (sched == 0) { // automatic
      if ((tm->tm_wday > 0)&&(tm->tm_wday < 6)) tmpch = 0; //Mon - Fri
      else if (tm->tm_wday == 6) tmpch = 1; //Sat
      else if (tm->tm_wday == 0) tmpch = 2; //Sun
  } else { // manual
    tmpch = sched - 1; //and stays
  }

  if (tmpch != memch) { // update if different
    memch = tmpch;
    readEE();
    ws.printfAll("Now,Setting,%02d:%02d,%02d:%02d,%+2.1f", ee.hstart, ee.mstart, ee.hstop, ee.mstop, ee.tempe);    
  }
  
  // process smart switch by time and temperature
  uint16_t xmi = (uint16_t)(60*tm->tm_hour) + tm->tm_min; // max 24h = 1440min, current time
  uint16_t bmi = (uint16_t)(60*ee.hstart) + ee.mstart;    // begin in minutes
  uint16_t emi = (uint16_t)(60*ee.hstop) + ee.mstop;      // end in minutes

  if (bmi == emi) heat_enabled = false;
  else { //enable smart if different

    if (((bmi < emi)&&(bmi <= xmi)&&(xmi < emi))||
        ((emi < bmi)&&((bmi <= xmi)||(xmi < emi)))) {
         heat_enabled = true;
       } else heat_enabled = false;
  }

  if (heat_enabled_prev) { // smart control (delayed one cycle)
    if (((t - HYST) < ee.tempe)&&(ledState == HIGH)) { // OFF->ON once
      ledState = LOW;
      digitalWrite(ledPin, ledState); // apply change
      ws.textAll("led,ledon");
    }
    if ((((t + HYST) > ee.tempe)&&(ledState == LOW))||(!heat_enabled)) { // ON->OFF once, also turn off at end of period.
      ledState = HIGH;
      digitalWrite(ledPin, ledState); // apply change
      ws.textAll("led,ledoff");
    }
    Serial.printf(ledState ? "LED OFF" : "LED ON");
    Serial.print(F(", Smart enabled\n"));
  }
  heat_enabled_prev = heat_enabled; //update 
}

void updateDHT(){
  h = dht.readHumidity();
  t = dht.readTemperature(); //Celsius or dht.readTemperature(true) for Fahrenheit
  if (isnan(h) || isnan(t)) {
  Serial.print(F("Failed to read from DHT sensor!"));
     h = 0; // debug w/o sensor
     t = 0;
   } 
}

void analogSample()
{      
  ws.printfAll("wpMeter,Arduino,%+2.1f,%2.1f,%d", t, h, heat_enabled_prev);
  Serial.printf("T/H.: %+2.1f°C/%2.1f%%,%d\n", t, h, heat_enabled_prev);
}

void checkPhysicalButton()
{
  if (digitalRead(btnPin) == LOW) {
    if (btnState != LOW) {     // btnState is used to avoid sequential toggles
      ledState = !ledState;
      digitalWrite(ledPin, ledState);
      if (ledState) ws.textAll("led,ledoff");
      else ws.textAll("led,ledon");
    }
    btnState = LOW;
  } else {
    btnState = HIGH;
  }
}

void mytimer() {
    ++count;          //200ms increments
    checkPhysicalButton();
    if ((count % 25) == 1) { // update temp every 5 seconds
      analogSample();
      udht = true;
    }
    if ((count % 50) == 0) { // update temp every 10 seconds
      ws.cleanupClients();
    }
    if (count >= 150) { // cycle every 30 sec
      showTime();
      count = 0;
    }
}

// server
void onWsEvent(AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
  if(type == WS_EVT_CONNECT){
    Serial.printf("ws[%s][%u] connect\n", server->url(), client->id());
    //client->printf("Hello Client %u :)", client->id());
    //client->ping();

    IPAddress ip = client->remoteIP();
    Serial.printf("[%u] Connected from %d.%d.%d.%d\n", client->id(), ip[0], ip[1], ip[2], ip[3]);
    showTime();
    analogSample();
    if (ledState) ws.textAll("led,ledoff");
    else ws.textAll("led,ledon");
    
    ws.printfAll("Now,Setting,%02d:%02d,%02d:%02d,%+2.1f", ee.hstart, ee.mstart, ee.hstop, ee.mstop, ee.tempe);
    ws.printfAll("Now,sched,%d", sched);
    
  } else if(type == WS_EVT_DISCONNECT){
    Serial.printf("ws[%s][%u] disconnect\n", server->url(), client->id());
    ws.textAll("Now,remoff"); 
    
  } else if(type == WS_EVT_ERROR){
    Serial.printf("ws[%s][%u] error(%u): %s\n", server->url(), client->id(), *((uint16_t*)arg), (char*)data);
  } else if(type == WS_EVT_PONG){
    Serial.printf("ws[%s][%u] pong[%u]: %s\n", server->url(), client->id(), len, (len)?(char*)data:"");
  } else if(type == WS_EVT_DATA){
    AwsFrameInfo * info = (AwsFrameInfo*)arg;
    String msg = "";
    if(info->final && info->index == 0 && info->len == len){
      //the whole message is in a single frame and we got all of it's data
      Serial.printf("ws[%s][%u] %s-message[%llu]: ", server->url(), client->id(), (info->opcode == WS_TEXT)?"text":"binary", info->len);

      if(info->opcode == WS_TEXT){
        for(size_t i=0; i < info->len; i++) { //debug
          msg += (char) data[i];
        }
              if(data[0] == 'L') { // LED
                if(data[1] == '1') {
                  ledState = LOW;
                  ws.textAll("led,ledon"); // for others
                }
                else if(data[1] == '0') {
                  ledState = HIGH;
                  ws.textAll("led,ledoff"); 
                }
                digitalWrite(ledPin, ledState); // apply change
              
                                
              } else if(data[0] == 'T') { // timeset
                  if (len > 11) {
                    data[3] = data[6] = data[9] = data[12] = 0; // cut strings 
                    ee.hstart = (uint8_t) atoi((const char *) &data[1]);
                    ee.mstart = (uint8_t) atoi((const char *) &data[4]);
                    ee.hstop  = (uint8_t) atoi((const char *) &data[7]);
                    ee.mstop  = (uint8_t) atoi((const char *) &data[10]);
                    Serial.printf("[%u] Timer set %02d:%02d - %02d:%02d\n", client->id(), ee.hstart, ee.mstart, ee.hstop, ee.mstop);
                    writeEE();
                    memch = 255; // to force showTime()to send Setting
                    showTime();
                  }
              } else if(data[0] == 'W') { // temperatureset
                  if (len > 3) {
                    if (ee.tempe != (float) atof((const char *) &data[1])){
                     ee.tempe = (float) atof((const char *) &data[1]);
                     Serial.printf("[%u] Temp set %+2.1f\n", client->id(), ee.tempe);
                     writeEE();
                     memch = 255; // to force showTime()to send Setting
                     showTime();
                   }
                  }
              } else if ((data[0] == 'Z')&&(len > 2)) { // sched
                  data[2] = 0;
                  if (sched != (uint8_t) atoi((const char *) &data[1])){
                    sched = (uint8_t) atoi((const char *) &data[1]);
                    EEPROM.put(EESC, sched); //separately
                    EEPROM.commit(); //needed for ESP8266?
                    ws.printfAll("Now,sched,%d", sched);    
                    showTime();
                  }
              }
        
      } else {
        char buff[3];
        for(size_t i=0; i < info->len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n",msg.c_str());

      if(info->opcode == WS_TEXT)
        client->text("I got your text message");
      else
        client->binary("I got your binary message");
        
    } else {
      //message is comprised of multiple frames or the frame is split into multiple packets
      if(info->index == 0){
        if(info->num == 0)
          Serial.printf("ws[%s][%u] %s-message start\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
        Serial.printf("ws[%s][%u] frame[%u] start[%llu]\n", server->url(), client->id(), info->num, info->len);
      }

      Serial.printf("ws[%s][%u] frame[%u] %s[%llu - %llu]: ", server->url(), client->id(), info->num, (info->message_opcode == WS_TEXT)?"text":"binary", info->index, info->index + len);

      if(info->opcode == WS_TEXT){
        for(size_t i=0; i < len; i++) {
          msg += (char) data[i];
        }
      } else {
        char buff[3];
        for(size_t i=0; i < len; i++) {
          sprintf(buff, "%02x ", (uint8_t) data[i]);
          msg += buff ;
        }
      }
      Serial.printf("%s\n",msg.c_str());

      if((info->index + len) == info->len){
        Serial.printf("ws[%s][%u] frame[%u] end[%llu]\n", server->url(), client->id(), info->num, info->len);
        if(info->final){
          Serial.printf("ws[%s][%u] %s-message end\n", server->url(), client->id(), (info->message_opcode == WS_TEXT)?"text":"binary");
          if(info->message_opcode == WS_TEXT)
            client->text("I got your text message");
          else
            client->binary("I got your binary message");
        }
      }
    }
  }
}


// setup -----------------------------------

void setup(){
  Serial.begin(115200);
  Serial.setDebugOutput(true);

//Wifi 
#ifdef USE_WFM 
  AsyncWiFiManager wifiManager(&server,&dns);
 #ifdef DEL_WFM
  wifiManager.resetSettings();
 #endif
  wifiManager.setTimeout(FBTO); // seconds to config or it creates an own AP, then browse 192.168.4.1
  if (!wifiManager.autoConnect(hostName)){  
    Serial.print(F("*FALLBACK AP*\n"));
    WiFi.mode(WIFI_AP);
    WiFi.softAP(fbssid, fbpassword);
 // MDNS.begin(fbssid);
 // MDNS.addService("http","tcp",80); // Core SVN 5179 use STA as default interface in mDNS (#7042)
  }
  
#else
// Manual simple STA mode to connect to known router
    //WiFi.mode(WIFI_AP_STA); // Core SVN 5179 use STA as default interface in mDNS (#7042)
    //WiFi.softAP(hostName);  // Core SVN 5179 use STA as default interface in mDNS (#7042)
  WiFi.mode(WIFI_STA);      // Core SVN 5179 use STA as default interface in mDNS (#7042)
  WiFi.begin(ssid, password);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.print(F("STA: Failed!\n"));
    WiFi.disconnect(false);
    delay(1000);
    WiFi.begin(ssid, password);
  }
#endif 

  Serial.print(F("*CONNECTED*\n"));
  
//DHT
  dht.begin();
  updateDHT(); //first reading takes time, hold here than the loop;

//Real Time
  time_t rtc = RTC_UTC_TEST;
  timeval tv = { rtc, 0 };     
                               //timezone tz = { 0, 0 };   //(insert) <#5194
  settimeofday(&tv, nullptr);  //settimeofday(&tv, &tz);   // <#5194
  configTime(MYTZ, "pool.ntp.org");
  
//MDNS (not needed)
  // MDNS.begin(hostName);
  // MDNS.addService("http","tcp",80); // Core SVN 5179 use STA as default interface in mDNS (#7042)
  
//I/O & DHT
  pinMode(ledPin, OUTPUT);
  pinMode(btnPin, INPUT_PULLUP);
  
//EE
  EEPROM.begin(EEALL);
  //EEPROM.get(EECH, memch); //current channel, no need
  readEE(); // populate structure if healthy 
  digitalWrite(ledPin, ledState);
  Serial.printf("Timer set %02d:%02d - %02d:%02d\n", ee.hstart, ee.mstart, ee.hstop, ee.mstop);
  Serial.printf("Temp set %+2.1f\n", ee.tempe);

//SPIFFS
  SPIFFS.begin();

  ws.onEvent(onWsEvent);
  server.addHandler(&ws);

#ifdef ESP32
  server.addHandler(new SPIFFSEditor(SPIFFS, http_username,http_password));
#elif defined(ESP8266)
  server.addHandler(new SPIFFSEditor(http_username,http_password));
#endif
  
  server.on("/free-ram", HTTP_GET, [](AsyncWebServerRequest *request){  // direct request->answer
    request->send(200, "text/plain", String(ESP.getFreeHeap()));
  });


  server.on("/get-time", HTTP_GET, [](AsyncWebServerRequest *request){  
      if(request->hasParam("btime")){
       time_t rtc = (request->getParam("btime")->value()).toInt();
       timeval tv = { rtc, 0 };            
       settimeofday(&tv, nullptr);  
      }
       request->send(200, "text/plain","Got browser time ...");
  });


  server.on("/hw-reset", HTTP_GET, [](AsyncWebServerRequest *request){
      request->onDisconnect([]() {
#ifdef ESP32
        ESP.restart();
#elif defined(ESP8266)
        ESP.reset();
#endif
      });
    request->send(200, "text/plain","Restarting ...");
  });

  server.on("/erase-wifi", HTTP_GET, [](AsyncWebServerRequest *request){
      request->onDisconnect([]() {
        WiFi.disconnect(true);  
#ifdef ESP32
        ESP.restart();
#elif defined(ESP8266)
        ESP.reset();
#endif
      });
    request->send(200, "text/plain","Erasing WiFi data ...");
  });

#ifdef USE_AUTH
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm").setAuthentication(http_username,http_password);
#else
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.htm");
#endif

  server.onNotFound([](AsyncWebServerRequest *request){  // nothing known
    Serial.print(F("NOT_FOUND: "));
    if (request->method() == HTTP_OPTIONS) request->send(200); //CORS
    else request->send(404);
  });

  DefaultHeaders::Instance().addHeader("Access-Control-Allow-Origin", "*");//CORS
  server.begin();
  
  //Timer tick
  tim.attach(0.2, mytimer); //every 0.2s

   //OTA
  ArduinoOTA.setHostname(hostName);
  ArduinoOTA.onStart([]() { 
    Serial.print(F("OTA Started ...\n"));
    ws.textAll("Now,OTA"); // for all clients
  });
  ArduinoOTA.begin();
} // setup end

// loop -----------------------------------
void loop(){
  if (udht){  // 5sec
    updateDHT();
    udht = false;
  }
  ArduinoOTA.handle();
}
