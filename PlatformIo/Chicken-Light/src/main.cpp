//------------------------------
// ESP32 NodeMCU 32-S (ESP-WROOM-32)
// Chicken House Light Control
//
// 04.05.2022   B. Michl    initial version
// 
//
//
//  TODO
//
//  * wifi hotspot mode
//  * web server
//      pass values via html form (adjust RTC time+date, rise and fall times, threshold for light sensor)     OK
//      buttons enable/disable light control
//      switch on/off/dim light manually
//      display time and temperature        OK
//  * DS3231 RTC
//      init                  OK
//      set date + time
//      read date + time
//  * PWM dimming led strip
//  * light sensor
//  * switch SW1 switch light on permanently
//  * sunrise / sunset table, twillight times
//  * calculate calendar week from date
//  * DS18B20 temperature sensor
//      init                OK
//      read temperature
//------------------------------

//includes
//------------------------------
#include <Arduino.h>


#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include "WiFiCredentials.h"

#include "SPIFFS.h"

#include "time.h"

#include <Wire.h>
#include <SPI.h>

#include "RTClib.h"

#include <OneWire.h>
#include <DallasTemperature.h>

#include "SunriseSunset.h"
//------------------------------

//constants
//------------------------------

//LED
#define LED_GREEN     26
#define LED_INTERN    2

//PWM
#define PWM_OUT 16

//switch light on
#define SWITCH1 27

//brightness sensor
#define BRIGHTNESS_DIGITAL_IN 13
#define BRIGHTNESS_ANALOG_IN 36

//temperature sensor
#define DS18B20_DATA 21

//serial
#define SERIAL_BAUD_RATE 115200

//I2C
#define I2C_SCL     22
#define I2C_SDA     21

//RTC-EEPROM
#define DS3231_EEPROM_ADDRESS 0x57


#define ESP_getChipId()   ((uint32_t)ESP.getEfuseMac())

//webserver
const char* PARAM_INPUT_1 = "InputDateTime";
const char* PARAM_INPUT_2 = "InputThresholdDark";
const char* PARAM_INPUT_3 = "InputThresholdBright";
//------------------------------


//global variables
//------------------------------

//web server
AsyncWebServer server(80);

//WiFi
bool WifiConnected_b = false;

//RTC DS3132
RTC_DS3231 rtc;     //examples: https://wolles-elektronikkiste.de/ds3231-echtzeituhr

//DS18B20 temperature sensor
OneWire oneWire(DS18B20_DATA);
DallasTemperature DS18B20(&oneWire);


tm DateTime_st;
tm Sunrise_st;
tm Sunset_st;

uint8_t DutyCyclePercent_u8 = 0;

uint8_t ThresholdDarkPercent_u8 = 0;
uint8_t ThresholdBrightPercent_u8 = 100;
//------------------------------

//function prototypes
//------------------------------
void main_task(void * pvParameters);

String processor(const String& var);

void GetDateTime_v(void);
void SetDateTime_v(String DateTimeString);
void GetSunriseTime_v(void);
void GetSunsetTime_v(void);

float GetTemperature_f32(void);

uint8_t CalcCalendarWeek_u8(uint16_t YYYY_u16, uint16_t MM_u16, uint16_t DD_u16);
//------------------------------


//------------------------------
//setup, configuration and test routines
//------------------------------
void setup() 
{
  //serial connection
  //------------------------------
  Serial.begin(SERIAL_BAUD_RATE);
  //------------------------------


  //LED
  //------------------------------
  pinMode(LED_GREEN, OUTPUT);
  pinMode(LED_INTERN, OUTPUT);
  //------------------------------



  Serial.println("---- Starting ESP32 Chicken House Light Control... ----");


  //create main task
  xTaskCreate(main_task, "Main task", 4096*4, NULL, 1, NULL);

  //TESTS GO HERE
  /*

  */

  //wifi
  //---
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  if (WiFi.waitForConnectResult() != WL_CONNECTED) 
  {
    Serial.printf("WiFi Connection Failed!\n");
    return;
  }
  else
  {
    WifiConnected_b = true;
  }

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("\n");
  //---


  //RTC
  //---
  if (! rtc.begin()) 
  {
    Serial.println("couldn't find RTC!\n");
  }
  
  if (rtc.lostPower()) 
  {
    Serial.println("RTC lost power, using default time");

    rtc.adjust(DateTime(2022, 1, 1, 0, 0, 0));  //set RTC to YYYY, M, D, H, M, S
  }
  //---

  //SPIFFS
  //---
  // Initialize SPIFFS
  if(!SPIFFS.begin(true))
  {
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }
  //---


  //web server
  //---
  // Route for root / web page --> resides in filesystem
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/index.html", String(), false, processor);
              }
            );

  // Route to load style.css file
  server.on("/style.css", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/style.css", "text/css");
              }
            );

  // Route to symbol images
  //----
  server.on("/symbol_huhn.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_huhn.png", "image/png");
              }
            );

  server.on("/symbol_huhn_gespiegelt.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_huhn_gespiegelt.png", "image/png");
              }
            );

  server.on("/symbol_temperatur.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_temperatur.png", "image/png");
              }
            );

  server.on("/symbol_uhr.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_uhr.png", "image/png");
              }
            );

  server.on("/symbol_speichern.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_speichern.png", "image/png");
              }
            );

  server.on("/symbol_tag.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_tag.png", "image/png");
              }
            );

  server.on("/symbol_nacht.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_nacht.png", "image/png");
              }
            );

  server.on("/symbol_licht_birne_aus.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_licht_birne_aus.png", "image/png");
              }
            );

  server.on("/symbol_licht_an.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_licht_an.png", "image/png");
              }
            );

  server.on("/symbol_licht_aus.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_licht_aus.png", "image/png");
              }
            );

  server.on("/symbol_play.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_play.png", "image/png");
              }
            );

  server.on("/symbol_stop.png", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                request->send(SPIFFS, "/symbol_stop.png", "image/png");
              }
            );
  //----


  // Send a GET request to 
  server.on("/get", HTTP_GET, [] (AsyncWebServerRequest *request) 
              {
                String inputMessage;
                String inputParam;
                // GET InputDateTime value
                if (request->hasParam(PARAM_INPUT_1)) 
                {
                  inputMessage = request->getParam(PARAM_INPUT_1)->value();
                  inputParam = PARAM_INPUT_1;

                  Serial.print("Set DateTime: ");
                  Serial.print("\n");
                  Serial.println(inputMessage);
                  Serial.print("\n");
                  SetDateTime_v(inputMessage);
                }
                // GET InputThresholdDark value
                else if (request->hasParam(PARAM_INPUT_2)) 
                {
                  inputMessage = request->getParam(PARAM_INPUT_2)->value();
                  inputParam = PARAM_INPUT_2;
                  ThresholdDarkPercent_u8 = inputMessage.toInt();

                  Serial.print("Set ThresholdDarkPercent_u8: ");
                  Serial.print("\n");
                  Serial.println(ThresholdDarkPercent_u8);
                  Serial.print("\n");
                }
                // GET InputThresholdBright value
                else if (request->hasParam(PARAM_INPUT_3)) 
                {
                  inputMessage = request->getParam(PARAM_INPUT_3)->value();
                  inputParam = PARAM_INPUT_2;
                  ThresholdBrightPercent_u8 = inputMessage.toInt();

                  Serial.print("Set ThresholdBrightPercent_u8: ");
                  Serial.print("\n");
                  Serial.println(ThresholdBrightPercent_u8);
                  Serial.print("\n");
                }
                else 
                {
                  inputMessage = "No message sent";
                  inputParam = "none";
                }
                //Serial.println(inputMessage);
                //request->send(200, "text/html", "HTTP GET request sent to your ESP on input field (" 
                //                     + inputParam + ") with value: " + inputMessage +
                //                     "<br><a href=\"/\">Return to Home Page</a>");
                request->send(200, "text/html", "<h1>Wert wurde gesendet.<br><a href=\"/\">Zurueck zur Hauptseite</a></h1>");
                });

  

  server.begin();
  //---
}
//------------------------------



//------------------------------
//loop()
//------------------------------
void loop() 
{
  //nothing to do here...
}
//------------------------------


//------------------------------
//main loop
//------------------------------
void main_task(void * pvParameters) 
{

  while (1) 
  {

    digitalWrite(LED_GREEN, HIGH);

    // Idle for xx msec
    if(WifiConnected_b == true)
    {
      vTaskDelay(pdMS_TO_TICKS(2));

      digitalWrite(LED_GREEN, LOW);

      vTaskDelay(pdMS_TO_TICKS(198));
    }
    else
    {
      vTaskDelay(pdMS_TO_TICKS(200));
    }
    

  }
}
//------------------------------



//------------------------------
// Processor function for webserver
// replaces placeholders with strings
//------------------------------
String processor(const String& var)
{
  String RetStr = "";

  

  if(var == "DATE_TIME")
  {
    GetDateTime_v();

    RetStr = String(DateTime_st.tm_mday) + "-" + String(DateTime_st.tm_mon) + "-" + String(DateTime_st.tm_year) + "  " +
             String(DateTime_st.tm_hour) + ":" + String(DateTime_st.tm_min) + ":" + String(DateTime_st.tm_sec);

    Serial.print("Date Time: ");
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");
  }

  else if(var == "TEMP")
  {
    RetStr = String(GetTemperature_f32(), 1);

    Serial.print("Temperature: ");
    Serial.print(GetTemperature_f32());
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");

    
  }

  else if(var == "LIGHT_DUTYCYCLE")
  {
    RetStr = String(DutyCyclePercent_u8);

    Serial.print("Dutycycle: ");
    Serial.print(DutyCyclePercent_u8);
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");

    
  }


  else if(var == "SUNRISE")
  {
    GetSunriseTime_v();

    RetStr = String(Sunrise_st.tm_hour) + ":" + String(Sunrise_st.tm_min) + ":00";

    Serial.print("Sunrise Time: ");
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");
  }


  else if(var == "SUNSET")
  {
    GetSunsetTime_v();

    RetStr = String(Sunset_st.tm_hour) + ":" + String(Sunset_st.tm_min) + ":00";

    Serial.print("Sunset Time: ");
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");
  }


  else if(var == "THRESHOLD_DARK")
  {
    RetStr = String(ThresholdDarkPercent_u8);

    Serial.print("ThresholdDarkPercent: ");
    Serial.print(ThresholdDarkPercent_u8);
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");
  }

  else if(var == "THRESHOLD_BRIGHT")
  {
    RetStr = String(ThresholdBrightPercent_u8);

    Serial.print("ThresholdBrightPercent: ");
    Serial.print(ThresholdBrightPercent_u8);
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");
  }



  return RetStr;
}
//------------------------------



//------------------------------
// Get temperature value from DS18B20
//------------------------------
float GetTemperature_f32(void)
{
  float Temperature_f32 = 0.0F;

  DS18B20.requestTemperatures();                  // send the command to get temperatures
  Temperature_f32 = DS18B20.getTempCByIndex(0);   // read temperature in °C

  Serial.print("DS18B20 temperature: ");
  Serial.print(Temperature_f32);
  Serial.print("°C \n");

  
  Serial.print("DS3231 temperature: ");
  Serial.print(rtc.getTemperature());
  Serial.print("°C \n");

  return Temperature_f32;
}
//------------------------------


//------------------------------
// Get date and time from DS3231
//------------------------------
void GetDateTime_v(void)
{
  DateTime now = rtc.now();   //get current time from RTC 

  DateTime_st.tm_mday = int(now.day());
  DateTime_st.tm_mon = int(now.month());
  DateTime_st.tm_year = int(now.year());

  DateTime_st.tm_hour = int(now.hour());
  DateTime_st.tm_min = int(now.minute());
  DateTime_st.tm_sec = int(now.second());
}
//------------------------------


//------------------------------
// Set date and time of DS3231 to user values
//------------------------------
void SetDateTime_v(String DateTimeString)
{

  //convert date-time-string to yyyy, mm, dd, hh, mm, ss

  rtc.adjust(DateTime(2022, 1, 1, 0, 0, 0));  //set RTC to YYYY, M, D, H, M, S

  Serial.println("RTC time set!");

  //buffer can be defined using following combinations:
  //hh - the hour with a leading zero (00 to 23)
  //mm - the minute with a leading zero (00 to 59)
  //ss - the whole second with a leading zero where applicable (00 to 59)
  //YYYY - the year as four digit number
  //YY - the year as two digit number (00-99)
  //MM - the month as number with a leading zero (01-12)
  //MMM - the abbreviated English month name ('Jan' to 'Dec')
  //DD - the day as number with a leading zero (01 to 31)
  //DDD - the abbreviated English day name ('Mon' to 'Sun')

  DateTime now = rtc.now();

  char buf2[] = "YYMMDD-hh:mm:ss";
  Serial.println(now.toString(buf2));
}
//------------------------------


//------------------------------
// lookup sunrise time
//------------------------------
void GetSunriseTime_v(void)
{
  Sunrise_st.tm_hour = 5;
  Sunrise_st.tm_min = 55;
}
//------------------------------


//------------------------------
// lookup sunset time
//------------------------------
void GetSunsetTime_v(void)
{
  Sunset_st.tm_hour = 20;
  Sunset_st.tm_min = 53;
}
//------------------------------


//------------------------------
// calculate calendar week number
//------------------------------
uint8_t CalcCalendarWeek_u8(uint16_t y_u16, uint16_t m_u16, uint16_t d_u16) 
{
  //found here: https://forum.arduino.cc/t/ds1302-clock-calendar-and-week-number/964893/35

  // reject out-of-range dates
  if ((y_u16 < 1901)||(y_u16 > 2099)) return 0;
  if ((m_u16 < 1)||(m_u16 > 12)) return 0;
  if ((d_u16 < 1)||(d_u16 > 31)) return 0;
  // (It is useful to know that Jan. 1, 1901 was a Tuesday)
  // compute adjustment for dates within the year
  //     If Jan. 1 falls on: Mo Tu We Th Fr Sa Su
  // then the adjustment is:  6  7  8  9  3  4  5
  int adj = (((y_u16-1901) + ((y_u16-1901)/4) + 4) % 7) + 3;
  // compute day of the year (in range 1-366)
  int doy = d_u16;
  if (m_u16 > 1) doy += 31;
  if (m_u16 > 2) {
    if ((y_u16 % 4)==0) doy += 29;
    else doy += 28;
  }
  if (m_u16 > 3) doy += 31;
  if (m_u16 > 4) doy += 30;
  if (m_u16 > 5) doy += 31;
  if (m_u16 > 6) doy += 30;
  if (m_u16 > 7) doy += 31;
  if (m_u16 > 8) doy += 31;
  if (m_u16 > 9) doy += 30;
  if (m_u16 > 10) doy += 31;
  if (m_u16 > 11) doy += 30;
  // compute week number
  uint8_t wknum = (adj + doy) / 7;
  // check for boundary conditions
  if (wknum < 1) {
    // last week of the previous year
    // check to see whether that year had 52 or 53 weeks
    // re-compute adjustment, this time for previous year
    adj = (((y_u16-1902) + ((y_u16-1902)/4) + 4) % 7) + 3;
    // all years beginning on Thursday have 53 weeks
    if (adj==9) return 53;
    // leap years beginning on Wednesday have 53 weeks
    if ((adj==8) && ((y_u16 % 4)==1)) return 53;
    // other years have 52 weeks
    return 52;
  }
  if (wknum > 52) {
    // check to see whether week 53 exists in this year
    // all years beginning on Thursday have 53 weeks
    if (adj==9) return 53;
    // leap years beginning on Wednesday have 53 weeks
    if ((adj==8) && ((y_u16 % 4)==0)) return 53;
    // other years have 52 weeks
    return 1;
  }
  return wknum;
}
//------------------------------




//------------------------------
void notFound(AsyncWebServerRequest *request) 
{
    request->send(404, "text/plain", "Not found");
}
//------------------------------