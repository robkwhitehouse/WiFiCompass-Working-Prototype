/*
 - Electronic Boat Compass
 - RK Whitehouse Jan 2024
 - This version uses FreeRTOS for task scheduling and adds a http server for calibration
 
*/

/* Functionality
* The CMPS14 sensor module provides a tilt compensated
 * compass bearing. This is output as an NMEA "HDM" message over WiFi
 * The ESP32 provides a WiFi Access Point.
 * This has a Telnet server On port 23- the HDM messages are transmitted 5 times per second
 * There is an http server running on port 80 - The default (root) node serves a single page web-app 
 * point your browser at 192.168.4.1/public/sensorCalibration.html to run the calibration web app
 * You need to connect to the WiFi acces point first. The default SSID is "NavSource"
 * that makes callbacks to a REST API to perform calibration and configuration 
 *
 */

/* Software design
 *  
 *  There are two sets of methods 
 *  
 *  1. Foreground tasks - i.e. user interface tasks
 *  2. Background tasks - run at specific intervals
 *
 *  The background tasks are run by RTOS Tasks without any
 *  direct user interaction. The RTOS scheduler is pre-emptive so any shared variables probably need to be protected by semaphores
 *  
 *  The foreground tasks are called from the main loop()
 *  
 *
 * Communication between background and foreground tasks is via a set of global static
 * objects and variables
 * 
 */

/*
 * Hardware design
 * 
 * Runs on an ESP-32 devkit with a CMPS14 compass sensor module attached to the standard
 * I2C GPIO bus pins (22 & 21) 
 * 
 */


/* Imported libraries */
#include <SPI.h>
#include <Wire.h>
#include "Cmps14.h"
#include <Preferences.h> //Check -is this compatible with SPIFFS?
#include <cppQueue.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <WiFiAP.h>
#include <DNSServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SH110X.h>
#include <SPIFFS.h>
#include <HTTPS_Server_Generic.h>


/* Local libs */
#include "NMEA.hpp"
#include "calibration.h"
#include "webCalibration.h"


#define VERSION "Prototype 0.E.2"
#define TELNET_PORT 23       //Compass heading is output on this port
#define CONFIG_PORT 1024
#define WWW_PORT 80
#define MAX_TELNET_CLIENTS 4

#define DISPLAY_I2C_ADDRESS 0x3c //initialize with the I2C addr 0x3C Typically eBay OLED's
#define SCREEN_WIDTH 128 // OLED display width, in pixels
#define SCREEN_HEIGHT 64 // OLED display height, in pixels
#define OLED_RESET -1   //   QT-PY / XIAO

#define CMPS14_SAMPLERATE_DELAY_MS 100 //Compass chip is sampled 10 times per second

Adafruit_SH1106G display = Adafruit_SH1106G(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

//Pre-Declare background task methods
void output();
void updateHeading();
void turnOff();
void handleHttp();
void displayHeadings();


/* Declare Global Singleton Objects */

// Non-volatile settings - will be restored on power-up
Preferences settings;

//Handles for the various RTOS tasks. Will be populated later
TaskHandle_t outputTask, updateHeadingTask,  updateOLEDTask, httpServerTask;


unsigned short  boatHeading = 0; //Heading seen on boat compass, calculated from sensorHeading + boatCompassOffset
unsigned short sensorHeading = 0; //Heading read from CMPS14
byte calibration = 0; //CMPS14 calibration level

//Set up some storage for the NMEA output messages
HSCmessage hsc;
HDMmessage hdm;

//Create WiFi network object pointers
const char *ssid = "NavSource";  //WiFi network name
WiFiServer *telnetServer = NULL;
WiFiServer configServer(CONFIG_PORT);
WiFiServer *webServer =NULL;
WiFiClient **telnetClients = {NULL}; //Array of WiFi Clients
WiFiClient configClient;  //Configuration via Telnet - redundant

extern int16_t compassCard[]; //declared in calibration.h has compass card offsets for each degree


void setup() {
  disableCalibration();  //Stop the CMPS14 from automatic recalibrating
  Serial.begin(115200);
  delay(1000);
  Wire.begin();
  // Setup filesystem
  if (!SPIFFS.begin(true))
    Serial.println("Mounting SPIFFS failed");

  Serial.print("Audio Compass. Version ");
  Serial.println(VERSION);
  calibrationBegin();
  settings.begin("compass",false); //Open (or create) settings namespace "compass" in read-write mode
  if ( settings.isKey("compassCard") ) {//We have an existing compassCard in NVRAM
    Serial.println("Loading settings from flash memory");
    settings.getBytes("compassCard",&compassCard,sizeof(compassCard));
  } else Serial.println("No settings found in flash");
  
  

  //Init OLED display
  display.begin(DISPLAY_I2C_ADDRESS, true); // Address 0x3C default
  //Display splash screen on OLED
  displayOLEDSplash();

  //Startup the Wifi access point
  WiFi.softAP(ssid);
  telnetClients = new WiFiClient*[4];
  for(int i = 0; i < 4; i++)
  {
    telnetClients[i] = NULL;
  }
  //This server outputs the NMEA messages
  telnetServer = new WiFiServer(TELNET_PORT);
  telnetServer->begin();

  //This server is used for config, calibration  & debug
  
  configServer.begin();

  httpSetup(); //Setup the webserver -used for calibration
 
  IPAddress myAddr = WiFi.softAPIP();
  Serial.print("IP Address =");
  Serial.println(myAddr);

  delay(1000);

  //Start the RTOS background tasks
  xTaskCreatePinnedToCore(output, "Output", 4000, NULL, 1, &outputTask, 0);
  xTaskCreatePinnedToCore(updateHeading, "updateHDG", 4000, NULL, 1, &updateHeadingTask, 0);
  xTaskCreatePinnedToCore(handleHttp, "HandleHTTP", 8000, NULL, 1, &httpServerTask, 0);
  xTaskCreatePinnedToCore(displayHeadings, "updateOLED", 4000, NULL, 1, &updateOLEDTask, 0);

}

unsigned loopCounter;
long loopStart, totalLoopTime=0;

void loop() {
  
  loopStart = micros();

 
  WiFiClient tempClient = telnetServer->available();   // listen for incoming clients

  if (tempClient) {                             // if you get a client,
    Serial.println("New NMEA client.");           // print a message out the serial port
    for (int i=0; i<MAX_TELNET_CLIENTS; i++ ) {   //Add it to the Client array
       if ( telnetClients[i] == NULL ) {
          WiFiClient* client = new WiFiClient(tempClient);
          telnetClients[i] = client;
          break;
       }
    }
  }
  //Configuration can also be done via Telnet (different port)
  //But this method is now deprecated - please use a web browser
  configClient = configServer.available();
  if (configClient) {
    if (configClient.connected()) {
      Serial.println("Config Client detected.");
      calibrationMenu();
    }
  }

  totalLoopTime += micros() - loopStart;
  loopCounter++;

}

void displayOLEDSplash()
{
  display.clearDisplay();
  display.setTextSize(2);
  display.setCursor(20,0);
  display.setTextColor(SH110X_WHITE);
  display.println("E.A.S.T.");
  display.setCursor(25,25);
  display.setTextSize(1);
  display.println("Audio Compass");
  display.setCursor(25,40);
  display.println(VERSION);
  display.drawLine(0, 59, 127, 59, SH110X_WHITE);
  display.drawCircle(63, 59, 4, SH110X_WHITE);
  display.display();
}

//Definition of background RTOS tasks

//Output the heading as an NMEA message over WiFi
void output(void * pvParameters) {
  char buff[128];
  TickType_t xLastWakeTime;
  const TickType_t xPeriod = 200; //Run every 200ms

  // Initialise the xLastWakeTime variable with the current time.
  xLastWakeTime = xTaskGetTickCount ();
  
  for (;;) {
     if (Serial) {
       sprintf(buff, "Sensor: %03d deg. Boat: %03d deg.\n", sensorHeading, boatHeading);
       Serial.print(buff);  
     }
  
     //Update the NMEA message with the current boat heading 
     hdm.update(boatHeading);
     //This is the main business - transmit the heading an an NMEA message over Telnet (WiFi) to anybody that is interested   
     for ( int i=0; i<MAX_TELNET_CLIENTS; i++ ) {
       if ( telnetClients[i] != NULL ) 
         telnetClients[i]->println(hdm.msgString);
     }
     vTaskDelayUntil( &xLastWakeTime, xPeriod );
  }
}


//Update the heading from the CMPS14
void updateHeading(void * pvParameters) {         
  TickType_t xLastWakeTime;
  const TickType_t xPeriod = 100; //Run every 100ms

  // Initialise the xLastWakeTime variable with the current time.
  xLastWakeTime = xTaskGetTickCount ();
  
  for (;;) {
    //get the raw CMPS14 output
    sensorHeading = getBearing();

    //Apply compass card offset
    boatHeading = MOD360(sensorHeading - compassCard[sensorHeading]);

    calibration = getCalibration();

    vTaskDelayUntil( &xLastWakeTime, xPeriod );
  }
}  


//check if there is any work for the HHTP server
void handleHttp(void * pvParameters) {
  TickType_t xLastWakeTime;
  const TickType_t xPeriod = 100; //Run every 100ms

  // Initialise the xLastWakeTime variable with the current time.
  xLastWakeTime = xTaskGetTickCount ();
  
  for (;;) {
    httpServer.loop(); 
    vTaskDelayUntil( &xLastWakeTime, xPeriod );
  } 
}



void displayHeadings(void * pvParameters)
{
  char buff[64];
  long diff;
  TickType_t xLastWakeTime;
  const TickType_t xPeriod = 200; //Run every 500ms

  // Initialise the xLastWakeTime variable with the current time.
  xLastWakeTime = xTaskGetTickCount ();
  
  for (;;) {
  
  
    buff[0] = '\0';
  
    //Set up run mode OLED display  - display fixed items
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SH110X_WHITE);
    display.setCursor(3,0);
    display.println("Sensor Hdg  Boat Hdg");
    display.drawLine(66, 0, 66, 34, SH110X_WHITE);
    display.drawLine(0, 34, 127, 34, SH110X_WHITE);
    display.setCursor(3,38);
    display.println("Calibration Status;");

    display.setTextSize(2);

    //Display sensor heading
    display.setCursor(12,12);
    sprintf(buff,"%03d", sensorHeading);
    display.print(buff);

    //Display boat heading
    display.setCursor(83,12);
    sprintf(buff,"%03d", boatHeading);
    display.print(buff);
  
    //Sensor calibration status
    display.setCursor(3,50); 
    display.setTextSize(1);
     sprintf(buff,"   S:%1d G:%1d A:%1d M:%1d",
        (calibration & 0b11000000) >> 6, (calibration & 0b00110000) >> 4, (calibration & 0b00001100) >> 2, calibration & 0b00000011);
    display.print(buff);
  
    display.display();
    vTaskDelayUntil( &xLastWakeTime, xPeriod );
  }
}
