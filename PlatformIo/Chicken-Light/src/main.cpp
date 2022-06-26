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
//  * wifi hotspot mode   OK
//  * static IP address   OK
//  * web server
//      pass values via html form (adjust RTC time+date, rise and fall times, threshold for light sensor)     OK
//      buttons enable/disable light control and visualize running light control TESTING
//      switch on/off/dim light manually                                          OK
//      display time and temperature                                              OK
//  * DS3231 RTC
//      init                                  OK
//      set date + time                       OK
//      read date + time                      OK
//  * fetch time via NTP                      OK
//  * PWM dimming led strip
//  * light sensor                            XXX
//  * switch SW1 switch light on permanently  OK
//  * sunrise / sunset table                  OK
//  * twillight times                         OK
//  * DS18B20 temperature sensor
//      init                                  OK
//      read temperature                      OK
//  * dim up / down using tasks               OK
//  * light control task with state machine   OK
//------------------------------

//includes
//------------------------------
#include <Arduino.h>


#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>


#include "WiFiCredentials.h" //--> switch client / access point mode can also bei found here!

#include "SPIFFS.h"

#include "time.h"


#include <Wire.h>
#include <SPI.h>

#include "RTClib.h"

#define USE_NTP   //use NTP client for time keeping

#ifdef USE_NTP
  #include <NTPClient.h>
  #include <WiFiUdp.h>
#endif

#include <OneWire.h>
#include <DallasTemperature.h>

#include "SunriseSunset.h"
//------------------------------

//constants
//------------------------------

//version
const uint8_t VER_MAJOR_U8 = 1;
const uint8_t VER_MINOR_U8 = 1;

//DEBUG
//#define DEBUG_SUNRISE
//#define DEBUG_SUNSET

//LED
#define LED_GREEN     26
#define LED_INTERN    2

//PWM
#define PWM_OUT 16
const uint16_t PwmFreqHz_u16 = 5000;
const uint8_t PwmChannel_u8 = 0;
const uint8_t PwmResolutionBit_u8 = 13;

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
#define I2C_SCL     23
#define I2C_SDA     22

//RTC-EEPROM
#define DS3231_EEPROM_ADDRESS 0x57


#define ESP_getChipId()   ((uint32_t)ESP.getEfuseMac())

//WiFi
#define STATIC_IP   //use static IP instead of DHCP
String hostname = "chickenlight";

//webserver
const char* PARAM_INPUT_1 = "InputDateTime";
const char* PARAM_INPUT_2 = "InputThresholdDark";
const char* PARAM_INPUT_3 = "InputThresholdBright";

//light control states
#define STATE_IDLE 0
#define STATE_DIM_UP 1
#define STATE_WAITING_HOLD_TIME_SUNRISE 2
#define STATE_WAITING_HOLD_TIME_SUNSET 3
#define STATE_DIM_DOWN 4
#define STATE_STOP 5




//------------------------------


//global variables
//------------------------------

//Wifi
#ifdef USE_ACCESS_POINT
  IPAddress local_IP(192,168,111,1);
  IPAddress gateway(192,168,111,1);
  IPAddress subnet(255,255,255,0);
#endif

//web server
AsyncWebServer server(80);

//WiFi
bool WifiConnected_b = false;
#ifdef STATIC_IP
  IPAddress local_IP(192, 168, 178, 199);   //static IP
  IPAddress gateway(192, 168, 178, 1);      // gateway IP
  IPAddress subnet(255, 255, 255, 0);
  IPAddress primaryDNS(192, 168, 178, 1);
#endif

//RTC DS3132
RTC_DS3231 rtc;     //examples: https://wolles-elektronikkiste.de/ds3231-echtzeituhr

//NTP
#ifdef USE_NTP
  WiFiUDP ntpUDP;
  NTPClient timeClient(ntpUDP);
#endif

//DS18B20 temperature sensor
OneWire oneWire(DS18B20_DATA);
DallasTemperature DS18B20(&oneWire);


tm DateTime_st;
tm Sunrise_st;
tm Sunset_st;


uint16_t UpdateNtpCounter_u16 = 0;
String NtpFormattedDate;

uint8_t CalendarWeekNumber_u8 = 0;

uint8_t DutyCyclePercent_u8 = 0;

uint8_t ThresholdDarkPercent_u8 = 0;
uint8_t ThresholdBrightPercent_u8 = 100;

uint8_t StartDutyCyclePercent_u8 = 0;
uint8_t StopDutyCycle_u8 = 0;
uint16_t RampUpTimeSec_u16 = 0;
uint16_t RampDownTimeSec_u16 = 0;

bool LightOn_b = false;

bool DimTaskRunning_b = false;

bool LightControlRunning_b = false;
uint8_t LightControlState_u8 = STATE_IDLE;

uint8_t DimTimeMinFromTable_u8 = 0;
uint8_t HoldTimeMinFromTable_u8 = 0;

TaskHandle_t LightControl_taskHandle;
TaskHandle_t DimUp_taskHandle;
TaskHandle_t DimDown_taskHandle;
//------------------------------

//function prototypes
//------------------------------
void main_task(void * pvParameters);
void DimUp_task(void * pvParameters);
void DimDown_task(void * pvParameters);
void LightControl_task(void * pvParameters) ;

String processor(const String& var);

DateTime GetDateTime_v(void);
void SetDateTime_v(String DateTimeString);
void GetSunriseTime_v(void);
void GetSunsetTime_v(void);

float GetTemperature_f32(void);

uint8_t CalcCalendarWeek_u8(uint16_t YYYY_u16, uint16_t MM_u16, uint16_t DD_u16);

void SetPwmDutycycle(void);
void DimUp_v(void);
void DimDown_v(void);
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

  //Switch
  //------------------------------
  pinMode(SWITCH1, INPUT_PULLUP);
  //------------------------------

  //PWM
  //------------------------------
  ledcSetup(PwmChannel_u8, PwmFreqHz_u16, PwmResolutionBit_u8);   //configure PWM
  ledcAttachPin(PWM_OUT, PwmChannel_u8);  //attach GPIO pin
  DutyCyclePercent_u8 = 0;
  SetPwmDutycycle();
  //------------------------------


  Serial.println("---- Starting ESP32 Chicken House Light Control... ----");


  //create main task
  xTaskCreate(main_task, "Main task", 4096*4, NULL, 1, NULL);

  //TESTS GO HERE
  /*

  */

  //wifi
  //---
  #ifdef USE_ACCESS_POINT
    //start in access point mode
    WiFi.softAPConfig(local_IP, gateway, subnet);
    WiFi.softAP(ssid,password);
    Serial.printf("Settiung up WiFi Access Point ");
    Serial.printf(ssid);
    Serial.printf("\n");
    WifiConnected_b = true;
  #else
    //start as client
    WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);
    WiFi.setHostname(hostname.c_str());

    WiFi.mode(WIFI_STA);

    #ifdef STATIC_IP
      WiFi.config(local_IP, gateway, subnet, primaryDNS);
    #endif

    
  
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
  #endif

  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("\n");
  //---

  //I2C
  //------------------------------
  Wire.begin(I2C_SDA, I2C_SCL);   //I2C bus master
  Wire.setClock(100000);           //clock freq 100kHz
  //------------------------------


  //temperature sensor dummy read
  //------------------------------
  //float dummy_f32 = GetTemperature_f32();
  //------------------------------


  //RTC
  //---
  if (!rtc.begin()) 
  {
    Serial.println("couldn't find RTC!\n");
  }
  
  if (rtc.lostPower()) 
  {
    Serial.println("RTC lost power, using default time");

    rtc.adjust(DateTime(2022, 1, 1, 0, 0, 0));  //set RTC to YYYY, M, D, H, M, S
  }
  //---

  //NTP
  //---
  #ifdef USE_NTP
    // Initialize a NTPClient to get time
    timeClient.begin();
    timeClient.setTimeOffset(3600);

    if(!timeClient.update()) 
        {
          timeClient.forceUpdate();
        }

        // The formattedDate comes with the following format:
        // 2018-05-28T16:00:13Z
        NtpFormattedDate = timeClient.getFormattedDate();

        Serial.println("NTP date is: ");
        Serial.println(NtpFormattedDate);

        //set date and time of RTC
      SetDateTime_v(NtpFormattedDate);
  #endif

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


  // Route for button Light On
  server.on("/LightOn", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                //DutyCyclePercent_u8 = 100;
                //SetPwmDutycycle();

                //Serial.print("Set DutyCycle: ");
                //Serial.print(DutyCyclePercent_u8);
                //Serial.print("%\n");

                
                

                if(DimTaskRunning_b == false) 
                {
                  digitalWrite(LED_INTERN, HIGH);
                  //LightOn_b = true;

                  StartDutyCyclePercent_u8 = 0;
                  StopDutyCycle_u8 = 100;
                  RampUpTimeSec_u16 = 2;
                  RampDownTimeSec_u16 = 0;

                  //create dim task
                  xTaskCreate(DimUp_task, "DimUp task", 1024, NULL, 1, &DimUp_taskHandle);
                }


                request->send(SPIFFS, "/index.html", String(), false, processor);
              }
            );

  // Route for button LightOff
  server.on("/LightOff", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                //DutyCyclePercent_u8 = 0;
                //SetPwmDutycycle();

                //Serial.print("Set DutyCycle: ");
                //Serial.print(DutyCyclePercent_u8);
                //Serial.print("%\n");

                //LightOn_b = false;

                if(DimTaskRunning_b == false) 
                {
                  digitalWrite(LED_INTERN, LOW);
                  //LightOn_b = false;

                  StartDutyCyclePercent_u8 = 100;
                  StopDutyCycle_u8 = 0;
                  RampUpTimeSec_u16 = 0;
                  RampDownTimeSec_u16 = 2;

                  //create dim task
                  xTaskCreate(DimDown_task, "DimDown task", 1024, NULL, 1, &DimDown_taskHandle);
                }

                request->send(SPIFFS, "/index.html", String(), false, processor);
              }
            );


  // Route for button LightControlOn
  server.on("/LightControlOn", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                if(LightControlRunning_b == false)
                {
                
                  LightControlRunning_b = true;

                  Serial.print("Light Control Enabled\n");

                  LightControlState_u8 = STATE_IDLE;

                  //create light control task
                  xTaskCreate(LightControl_task, "Light Control Task", 4096*4, NULL, 1, &LightControl_taskHandle);

                }

                //just for TESTING
                //---------
                if(0)
                {
                  Serial.print("dimming up...\n");
                
                  request->send(SPIFFS, "/index.html", String(), false, processor);
                  
                  StartDutyCyclePercent_u8 = 0;
                  StopDutyCycle_u8 = 100;
                  RampUpTimeSec_u16 = 10;
                  RampDownTimeSec_u16 = 0;

                  //create dim task
                  xTaskCreate(DimUp_task, "DimUp task", 1024, NULL, 1, NULL);
                }
                //---------

                 request->send(SPIFFS, "/index.html", String(), false, processor);
                
              }
            );


  // Route for button LightControlOff
  server.on("/LightControlOff", HTTP_GET, [](AsyncWebServerRequest *request)
              {
                Serial.print("Light Control Disabled\n");

                LightControlRunning_b = false;

                LightControlState_u8 = STATE_IDLE;
                
                //stopping light control task and switch off light
                if(LightControl_taskHandle != NULL) 
                {
                  Serial.print("Stopping Light Control Task...\n");
                  vTaskDelete(LightControl_taskHandle);
                }
                else
                {
                  Serial.print("Light Control TaskHandle = NULL...\n");
                }


                if(DimUp_taskHandle!= NULL) 
                {
                  Serial.print("Stopping Dim Task...\n");
                  vTaskDelete(DimUp_taskHandle);
                }

                if(DimDown_taskHandle!= NULL) 
                {
                  Serial.print("Stopping Dim Task...\n");
                  vTaskDelete(DimDown_taskHandle);
                }


                DutyCyclePercent_u8 = 0;
                SetPwmDutycycle();

                digitalWrite(LED_INTERN, LOW);
                
                

                //just for TESTING
                //---------
                if(0)
                {
                  Serial.print("dimming down...\n");
                
                  request->send(SPIFFS, "/index.html", String(), false, processor);
                  
                  StartDutyCyclePercent_u8 = 100;
                  StopDutyCycle_u8 = 0;
                  RampUpTimeSec_u16 = 0;
                  RampDownTimeSec_u16 = 10;

                  //create dim task
                  xTaskCreate(DimDown_task, "DimDown task", 1024, NULL, 1, NULL);
                }
                //---------

                request->send(SPIFFS, "/index.html", String(), false, processor);
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

    //switch light manually on/off using hardware switch SWITCH1
    //------
    if((digitalRead(SWITCH1) == 0) && (LightOn_b == false) && (DimTaskRunning_b == false)) 
    {
      //switch light on
      Serial.print("HW switch dimming up...\n");
      
      LightOn_b = true;
      digitalWrite(LED_INTERN, HIGH);

      //create dim task
      StartDutyCyclePercent_u8 = 0;
      StopDutyCycle_u8 = 100;
      RampUpTimeSec_u16 = 2;
      RampDownTimeSec_u16 = 0;
      xTaskCreate(DimUp_task, "DimUp task", 1024, NULL, 1, &DimUp_taskHandle);
    }

    else if((digitalRead(SWITCH1) == 1) && (LightOn_b == true) && (DimTaskRunning_b == false)) 
    {
      //switch light off
      Serial.print("HW switch dimming down...\n");
              
      LightOn_b = false;
      digitalWrite(LED_INTERN, LOW);

      //create dim task
      StartDutyCyclePercent_u8 = 100;
      StopDutyCycle_u8 = 0;
      RampUpTimeSec_u16 = 0;
      RampDownTimeSec_u16 = 2;
      xTaskCreate(DimDown_task, "DimDown task", 1024, NULL, 1, &DimDown_taskHandle);
    }
    //------


    #ifdef USE_NTP
      //update NTP client every 60sec (300 * 200msec)
      

      if(UpdateNtpCounter_u16 > 300)
      {
        UpdateNtpCounter_u16 = 0;

        Serial.print("updating NTP client now...\n");

        if(!timeClient.update()) 
        {
          timeClient.forceUpdate();
        }

        // The formattedDate comes with the following format:
        // 2018-05-28T16:00:13Z
        NtpFormattedDate = timeClient.getFormattedDate();

        Serial.println("NTP date is: ");
        Serial.println(NtpFormattedDate);

        //set date and time of RTC
      SetDateTime_v(NtpFormattedDate);
      }

      UpdateNtpCounter_u16++;
    #endif


    

  }
}
//------------------------------



//------------------------------
//Light Control Task
//------------------------------
void LightControl_task(void * pvParameters) 
{

  uint16_t UpTimeSec_u16 = 0;      //time spent for dim up 
  uint16_t DownTimeSec_u16 = 0;    //time spent for dim down

  uint32_t HoldTimeSunriseSeconds_u32 = 0;
  uint32_t HoldTimeSunsetSeconds_u32 = 0;

  uint32_t HoldStartTimestamp_u32 = 0;
  uint32_t ExpiredHoldTimeSeconds_u32 = 0;

  DateTime now;

  Serial.print("Light Control Task Running...");


  while(1)
  { 
    
    //state machine
    switch(LightControlState_u8)
    {
      case STATE_IDLE:

        Serial.print("STATE = IDLE\n");

        digitalWrite(LED_INTERN, LOW);

        //get date and time
        GetDateTime_v();

        //get sunrise and sunset time
        GetSunriseTime_v();
        GetSunsetTime_v();

        //Dim time and hold time
        UpTimeSec_u16 = DimTimeMinFromTable_u8 * 60;
        DownTimeSec_u16 = DimTimeMinFromTable_u8 * 60;
        HoldTimeSunriseSeconds_u32 = HoldTimeMinFromTable_u8 * 60;
        HoldTimeSunsetSeconds_u32 = HoldTimeMinFromTable_u8 * 60;

        //calculate time for "new" sunrise and sunset
        if(DimTimeMinFromTable_u8 == 60)
        {
          Sunrise_st.tm_hour =- 1;
          
        }

        if(DimTimeMinFromTable_u8 == 120)
        {
          Sunrise_st.tm_hour =- 2;
        }


        if(HoldTimeMinFromTable_u8 == 30)
        {
          if(Sunrise_st.tm_min >= 30)
          {
            Sunrise_st.tm_min =- 30;
          }
          else
          {
            Sunrise_st.tm_min = 60 - (30-Sunrise_st.tm_min);
            Sunrise_st.tm_hour =- 1;
          }
        }

        if(HoldTimeMinFromTable_u8 == 60)
        {
          Sunrise_st.tm_hour =- 1;
        }

        if(HoldTimeMinFromTable_u8 == 90)
        {
          Sunrise_st.tm_hour =- 1;

          if(Sunrise_st.tm_min >= 30)
          {
            Sunrise_st.tm_min =- 30;
          }
          else
          {
            Sunrise_st.tm_min = 60 - (30-Sunrise_st.tm_min);
            Sunrise_st.tm_hour =- 1;
          }
        }

        
        //fake sunrise / sunset for DEBUGGING
        #ifdef DEBUG_SUNRISE
          Sunrise_st.tm_hour = DateTime_st.tm_hour;
          Sunrise_st.tm_min = DateTime_st.tm_min;
          DimTimeMinFromTable_u8 = 60;
          HoldTimeMinFromTable_u8 = 60;
          UpTimeSec_u16 = 60 * 60;
          HoldTimeSunriseSeconds_u32 = 60 * 60;
        #endif

        //fake sunrise / sunset for DEBUGGING
        #ifdef DEBUG_SUNSET
          Sunset_st.tm_hour = DateTime_st.tm_hour;
          Sunset_st.tm_min = DateTime_st.tm_min;
          DimTimeMinFromTable_u8 = 60;
          HoldTimeMinFromTable_u8 = 60;
          DownTimeSec_u16 = 60 * 60;
          HoldTimeSunsetSeconds_u32 = 60 * 60;
        #endif

        //if SUNRISE time is reached, start dim up task
        //only if dim up time or hold time is greater than zero
        if((DateTime_st.tm_hour == Sunrise_st.tm_hour)
            && (DateTime_st.tm_min == Sunrise_st.tm_min)
            && ( (DimTimeMinFromTable_u8 > 0) || (HoldTimeMinFromTable_u8 > 0) ) )
        {
          //dim up light
          Serial.print("sunrise time reached...\n");

          LightControlState_u8 = STATE_DIM_UP;

          digitalWrite(LED_INTERN, HIGH);

        }



        //if SUNSET time is reached, switch on light (100%) and wait hold time
        //only if dim up time or hold time is greater than zero
        if((DateTime_st.tm_hour == Sunset_st.tm_hour)
            && (DateTime_st.tm_min == Sunset_st.tm_min)
            && ( (DimTimeMinFromTable_u8 > 0) || (HoldTimeMinFromTable_u8 > 0) ) )
        {
          //switch on light
          Serial.print("sunset time reached...\n");

          Serial.print("switch on light...\n");
          DutyCyclePercent_u8 = 100;
          SetPwmDutycycle();
          
          digitalWrite(LED_INTERN, HIGH);

          LightControlState_u8 = STATE_WAITING_HOLD_TIME_SUNSET;

        }

        break;


      case STATE_DIM_UP:

          Serial.print("STATE = DIM UP\n");

          //dim up light
          Serial.print("dimming up...\n");
          
          digitalWrite(LED_INTERN, HIGH);

          //create dim task
          StartDutyCyclePercent_u8 = 0;
          StopDutyCycle_u8 = 100;
          RampUpTimeSec_u16 = UpTimeSec_u16;
          RampDownTimeSec_u16 = 0;
          xTaskCreate(DimUp_task, "DimUp task", 1024, NULL, 1, &DimUp_taskHandle);


          LightControlState_u8 = STATE_WAITING_HOLD_TIME_SUNRISE;

          break;


        case STATE_DIM_DOWN:

          Serial.print("STATE = DIM DOWN\n");

          //dim down light
          Serial.print("dimming down...\n");
          
          digitalWrite(LED_INTERN, LOW);

          //create dim task
          StartDutyCyclePercent_u8 = 100;
          StopDutyCycle_u8 = 0;
          RampUpTimeSec_u16 = 0;
          RampDownTimeSec_u16 = DownTimeSec_u16;
          xTaskCreate(DimDown_task, "DimDown task", 1024, NULL, 1, &DimDown_taskHandle);


          LightControlState_u8 = STATE_IDLE;

          break;


        case STATE_WAITING_HOLD_TIME_SUNRISE:

          Serial.print("STATE = WAIT HOLD SUNRISE\n");

          //wait for hold time to expire
          Serial.print("entering hold time loop...\n");

          //save start timestamp
          now = GetDateTime_v();
          HoldStartTimestamp_u32 = now.unixtime();

          Serial.print("HoldStartTimestamp: ");
          Serial.print(HoldStartTimestamp_u32);
          Serial.print("\n");

          while(ExpiredHoldTimeSeconds_u32 < HoldTimeSunriseSeconds_u32 + RampUpTimeSec_u16)
          {
            now = GetDateTime_v();
            ExpiredHoldTimeSeconds_u32 = now.unixtime() - HoldStartTimestamp_u32;

            Serial.print("waiting for hold time to expire...\n");
            Serial.print(ExpiredHoldTimeSeconds_u32);
            Serial.print("sec of ");
            Serial.print(HoldTimeSunriseSeconds_u32 + RampUpTimeSec_u16);
            Serial.print("sec expired\n");

            delay(2000);

          }

          LightControlState_u8 = STATE_IDLE;

          ExpiredHoldTimeSeconds_u32 = 0;

          DutyCyclePercent_u8 = 0;
          SetPwmDutycycle();
          
          digitalWrite(LED_INTERN, LOW);

          break;



      case STATE_WAITING_HOLD_TIME_SUNSET:

        Serial.print("STATE = WAIT HOLD SUNSET\n");

          //wait for hold time to expire
          Serial.print("entering hold time loop...\n");

          //save start timestamp
          now = GetDateTime_v();
          HoldStartTimestamp_u32 = now.unixtime();

          Serial.print("HoldStartTimestamp: ");
          Serial.print(HoldStartTimestamp_u32);
          Serial.print("\n");

          while(ExpiredHoldTimeSeconds_u32 < HoldTimeSunsetSeconds_u32)
          {
            now = GetDateTime_v();
            ExpiredHoldTimeSeconds_u32 = now.unixtime() - HoldStartTimestamp_u32;

            Serial.print("waiting for hold time to expire...\n");
            Serial.print(ExpiredHoldTimeSeconds_u32);
            Serial.print("sec of ");
            Serial.print(HoldTimeSunsetSeconds_u32);
            Serial.print("sec expired\n");

            delay(2000);

          }

          LightControlState_u8 = STATE_DIM_DOWN;

          ExpiredHoldTimeSeconds_u32 = 0;

          break;


        case STATE_STOP:

          Serial.print("STATE = WAIT HOLD SUNSET\n");

          ExpiredHoldTimeSeconds_u32 = 0;

          DutyCyclePercent_u8 = 0;
          SetPwmDutycycle();

          digitalWrite(LED_INTERN, LOW);

          LightControlState_u8 = STATE_IDLE;
          
          break;




      default:
        break;
    }




    
    //sleep
    vTaskDelay(pdMS_TO_TICKS(2000));

  }

  vTaskDelete(NULL);

}




//------------------------------
//dim up task
//------------------------------
void DimUp_task(void * pvParameters) 
{

  DimTaskRunning_b = true;

  Serial.println("DimUp task started");

  DimUp_v();

  Serial.println("DimUp task finished");

  DimTaskRunning_b = false;
  
  vTaskDelete(NULL);
  
}
//------------------------------


//------------------------------
//dim down task
//------------------------------
void DimDown_task(void * pvParameters) 
{
  DimTaskRunning_b = true;

  Serial.println("DimDown task started");

  DimDown_v();

  Serial.println("DimDown task finished");

  DimTaskRunning_b = false;

  vTaskDelete(NULL);
  
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

    if(DateTime_st.tm_min > 9)
    {
      RetStr = String(DateTime_st.tm_mday) + "-" + String(DateTime_st.tm_mon) + "-" + String(DateTime_st.tm_year) + "  " +
              String(DateTime_st.tm_hour) + ":" + String(DateTime_st.tm_min);
    }
    else
    {
      RetStr = String(DateTime_st.tm_mday) + "-" + String(DateTime_st.tm_mon) + "-" + String(DateTime_st.tm_year) + "  " +
              String(DateTime_st.tm_hour) + ":0" + String(DateTime_st.tm_min);
    }

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


  else if(var == "STATE")
  {
    switch(LightControlState_u8)
    {
      case STATE_IDLE:
        RetStr = "IDLE"; 
        break;
      
      case STATE_DIM_UP:
        RetStr = "DIM UP"; 
        break;

      case STATE_DIM_DOWN:
        RetStr = "DIM DOWN"; 
        break;

      case STATE_WAITING_HOLD_TIME_SUNRISE:
        RetStr = "WAIT TIME SUNRISE"; 
        break;

      case STATE_WAITING_HOLD_TIME_SUNSET:
        RetStr = "WAIT TIME SUNSET"; 
        break;

      case STATE_STOP:
        RetStr = "STOPPING"; 
        break;

      default:
        break;
    }

    if(LightControlRunning_b == false)
    {
      RetStr = RetStr + " (OFF)";
    }
    
  }




  else if(var == "SUNRISE")
  {
    GetSunriseTime_v();

    if(Sunrise_st.tm_min > 9)
    {
      if(Sunrise_st.tm_hour > 9)
      {
        RetStr = String(Sunrise_st.tm_hour) + ":" + String(Sunrise_st.tm_min);
      }
      else
      {
        RetStr = "0" + String(Sunrise_st.tm_hour) + ":" + String(Sunrise_st.tm_min);
      }
    }
    else
    {
      if(Sunrise_st.tm_hour > 9)
      {
        RetStr = String(Sunrise_st.tm_hour) + ":0" + String(Sunrise_st.tm_min);
      }
      else
      {
        RetStr = "0" + String(Sunrise_st.tm_hour) + ":0" + String(Sunrise_st.tm_min);
      }
    }

    Serial.print("Sunrise Time: ");
    Serial.print("\nRetStr: ");
    Serial.println(RetStr);
    Serial.print("\n");
  }


  else if(var == "SUNSET")
  {
    GetSunsetTime_v();

    if(Sunset_st.tm_min > 9)
    {
      if(Sunset_st.tm_hour > 9)
      {
        RetStr = String(Sunset_st.tm_hour) + ":" + String(Sunset_st.tm_min);
      }
      else
      {
        RetStr = "0" + String(Sunset_st.tm_hour) + ":" + String(Sunset_st.tm_min);
      }
    }
    else
    {
      if(Sunset_st.tm_hour > 9)
      {
        RetStr = String(Sunset_st.tm_hour) + ":0" + String(Sunset_st.tm_min);
      }
      else
      {
        RetStr = "0" + String(Sunset_st.tm_hour) + ":0" + String(Sunset_st.tm_min);
      }
    }

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


  else if(var == "VERSION")
  {
    RetStr = String(VER_MAJOR_U8) + "." + String(VER_MINOR_U8);
  }



  return RetStr;
}
//------------------------------



//------------------------------
// Set PWM dutycycle
//------------------------------
void SetPwmDutycycle(void)
{
  //set PWM dutycycle (range: 0...2^resolution - 1)
  ledcWrite(PwmChannel_u8, ( (1<<PwmResolutionBit_u8) - 1) / 100 * DutyCyclePercent_u8);
}
//------------------------------


//------------------------------
// Dim LED up
//------------------------------
void DimUp_v(void)
{
    uint8_t DutyCycleSteps_u8 =  StopDutyCycle_u8 - StartDutyCyclePercent_u8;

    uint32_t DelayBetweenStepsMsec_u32 = RampUpTimeSec_u16 * 1000 / DutyCycleSteps_u8;

    DutyCyclePercent_u8 = StartDutyCyclePercent_u8;

    SetPwmDutycycle();

    while(DutyCycleSteps_u8)
    {
      delay(DelayBetweenStepsMsec_u32);
      
      if(DutyCyclePercent_u8 < 100)
      {
        DutyCyclePercent_u8++;
      }

      SetPwmDutycycle();

      DutyCycleSteps_u8--;
    }
}
//------------------------------


//------------------------------
// Dim LED down
//------------------------------
void DimDown_v(void)
{
  uint8_t DutyCycleSteps_u8 = StartDutyCyclePercent_u8 - StopDutyCycle_u8;

  uint32_t DelayBetweenStepsMsec_u32 = RampDownTimeSec_u16 * 1000 / DutyCycleSteps_u8;

  DutyCyclePercent_u8 = StartDutyCyclePercent_u8;

  SetPwmDutycycle();

  while(DutyCycleSteps_u8)
  {
    delay(DelayBetweenStepsMsec_u32);
    
    if(DutyCyclePercent_u8 > 0)
    {
      DutyCyclePercent_u8--;
    }

    SetPwmDutycycle();

    DutyCycleSteps_u8--;
  }
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
DateTime GetDateTime_v(void)
{
  DateTime now = rtc.now();   //get current time from RTC 

  DateTime_st.tm_mday = int(now.day());
  DateTime_st.tm_mon = int(now.month());
  DateTime_st.tm_year = int(now.year());

  DateTime_st.tm_hour = int(now.hour());
  DateTime_st.tm_min = int(now.minute());
  DateTime_st.tm_sec = int(now.second());

  //calculation of calendar week
  CalendarWeekNumber_u8 = CalcCalendarWeek_u8(DateTime_st.tm_year, DateTime_st.tm_mon, DateTime_st.tm_mday);
  Serial.print("calendar week: ");
  Serial.print(CalendarWeekNumber_u8);
  Serial.print("\n");

  return now;

}
//------------------------------


//------------------------------
// Set date and time of DS3231 to user values
//------------------------------
void SetDateTime_v(String DateTimeString)
{
  uint16_t Year_u16 = 0;
  uint8_t Month_u8 = 0;
  uint8_t Day_u8 = 0;
  uint8_t Hour_u8 = 0;
  uint8_t Minute_u8 = 0;
  uint8_t Second_u8 = 0;
  char buf5 [5] = {0};
  char buf3 [3] = {0};
  char buf2 [2] = {0};

  //convert date-time-string to yyyy, mm, dd, hh, mm, ss
  // Y Y Y Y - M M - D D     H  H  :  M  M  :  S  S
  // 0 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18

  //year
  buf5 [0] = DateTimeString [0];
  buf5 [1] = DateTimeString [1];
  buf5 [2] = DateTimeString [2];
  buf5 [3] = DateTimeString [3];
  buf5 [4] = '\0';
  Year_u16 = atoi(buf5);

  //month
  if(DateTimeString [5] != '0')
  {
    buf3 [0] = DateTimeString [5];
    buf3 [1] = DateTimeString [6];
    buf3 [2] = '\0';
    Month_u8 = atoi(buf3);
  }
  else
  {
    buf2 [0] = DateTimeString [6];
    buf2 [1] = '\0';
    Month_u8 = atoi(buf2);
  }

  //day
  if(DateTimeString [8] != '0')
  {
    buf3 [0] = DateTimeString [8];
    buf3 [1] = DateTimeString [9];
    buf3 [2] = '\0';
    Day_u8 = atoi(buf3);
  }
  else
  {
    buf2 [0] = DateTimeString [9];
    buf2 [1] = '\0';
    Day_u8 = atoi(buf2);
  }

  //hours
  if(DateTimeString [11] != '0')
  {
    buf3 [0] = DateTimeString [11];
    buf3 [1] = DateTimeString [12];
    buf3 [2] = '\0';
    Hour_u8 = atoi(buf3);
  }
  else
  {
    buf2 [0] = DateTimeString [12];
    buf2 [1] = '\0';
    Hour_u8 = atoi(buf2);
  }

  //minutes
  if(DateTimeString [14] != '0')
  {
    buf3 [0] = DateTimeString [14];
    buf3 [1] = DateTimeString [15];
    buf3 [2] = '\0';
    Minute_u8 = atoi(buf3);
  }
  else
  {
    buf2 [0] = DateTimeString [15];
    buf2 [1] = '\0';
    Minute_u8 = atoi(buf2);
  }

  //seconds
  if(DateTimeString [17] != '0')
  {
    buf3 [0] = DateTimeString [17];
    buf3 [1] = DateTimeString [18];
    buf3 [2] = '\0';
    Second_u8 = atoi(buf3);
  }
  else
  {
    buf2 [0] = DateTimeString [18];
    buf2 [1] = '\0';
    Second_u8 = atoi(buf2);
  }

  Serial.print("set RTC to: \n");
  Serial.println(Year_u16);
  Serial.println(Month_u8);
  Serial.println(Day_u8);
  Serial.println(Hour_u8);
  Serial.println(Minute_u8);
  Serial.println(Second_u8);
  Serial.print("\n");
  

  rtc.adjust(DateTime(Year_u16, Month_u8, Day_u8, Hour_u8, Minute_u8, Second_u8));  //set RTC to YYYY, M, D, H, M, S

  Serial.println("RTC says: ");

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

  char buf15[] = "YYMMDD-hh:mm:ss";
  Serial.println(now.toString(buf15));
}
//------------------------------


//------------------------------
// lookup sunrise time
//------------------------------
void GetSunriseTime_v(void)
{

  Sunrise_st.tm_hour = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [0];
  Sunrise_st.tm_min = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [1];

  //dim time and hold time
  DimTimeMinFromTable_u8 = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [4];
  HoldTimeMinFromTable_u8 = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [5];


}
//------------------------------


//------------------------------
// lookup sunset time
//------------------------------
void GetSunsetTime_v(void)
{
  Sunset_st.tm_hour = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [2];
  Sunset_st.tm_min = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [3];

  //dim time and hold time
  DimTimeMinFromTable_u8 = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [4];
  HoldTimeMinFromTable_u8 = SunriseSunset_au8 [CalendarWeekNumber_u8 - 1] [5];
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