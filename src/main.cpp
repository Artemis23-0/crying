#include <M5Core2.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include "WiFi.h"
#include "FS.h"                 // SD Card ESP32
#include <EEPROM.h>             // read and write from flash memory
#include <NTPClient.h>          // Time Protocol Libraries
#include <WiFiUdp.h>            // Time Protocol Libraries
#include <Adafruit_VCNL4040.h>  // Sensor libraries
#include "Adafruit_SHT4x.h"     // Sensor libraries
#include <cstdlib>

////////////////////////////////////////////////////////////////////
// TODO 1: Enter your URL addresses
////////////////////////////////////////////////////////////////////
const String URL_GCF_UPLOAD = "https://us-central1-egr425-lab3-2024.cloudfunctions.net/StoreSensorData";
const String URL_GCF_RETRIEVE = "https://us-west2-egr425-lab3-2024.cloudfunctions.net/function-1";

////////////////////////////////////////////////////////////////////
// Variables
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
// TODO 2: Enter your WiFi Credentials
////////////////////////////////////////////////////////////////////
String wifiNetworkName = "CBU-LANCERS";
String wifiPassword = "LiveY0urPurp0se";

// Initialize library objects (sensors and Time protocols)
Adafruit_VCNL4040 vcnl4040 = Adafruit_VCNL4040();
Adafruit_SHT4x sht4 = Adafruit_SHT4x();
WiFiUDP ntpUDP;
NTPClient timeClient(ntpUDP);

// Time variables
unsigned long lastTime = 0;
unsigned long timerDelay = 5000; 

// Variables
bool gotNewDetails = false;

// Screen states
enum Screen { S_Live, S_Cloud};
static Screen screen = S_Live;
static bool stateChangedThisLoop = true;

////////////////////////////////////////////////////////////////////
// TODO 3: Device Details Structure
////////////////////////////////////////////////////////////////////
struct deviceDetails {
    int prox;
    int ambientLight;
    int whiteLight;
    double rHum;
    double temp;
    double accX;
    double accY;
    double accZ;
    long long timeCaptured;
    long long cloudUploadTime;
};

////////////////////////////////////////////////////////////////////
// Method header declarations
////////////////////////////////////////////////////////////////////
int httpGetWithHeaders(String serverURL, String *headerKeys, String *headerVals, int numHeaders);
bool gcfGetWithHeader(String serverUrl, String userId, time_t time, deviceDetails *details);
String generateM5DetailsHeader(String userId, time_t time, deviceDetails *details);
int httpPostFile(String serverURL, String *headerKeys, String *headerVals, int numHeaders, String filePath);
bool gcfPostFile(String serverUrl, String filePathOnSD, String userId, time_t time, deviceDetails *details);
String writeDataToFile(byte * fileData, size_t fileSizeInBytes);
int getNextFileNumFromEEPROM();
double convertFintoC(double f);
double convertCintoF(double c);
String generateUserIdHeader(String userId);
bool gcfGetWithUserHeader(String serverUrl, String userId, deviceDetails *latestDocDetails);
int httpGetLatestWithHeaders(String serverURL, String *headerKeys, String *headerVals, int numHeaders, deviceDetails *details);

///////////////////////////////////////////////////////////////
// Put your setup code here, to run once
///////////////////////////////////////////////////////////////
void setup()
{
    ///////////////////////////////////////////////////////////
    // Initialize the device
    ///////////////////////////////////////////////////////////
    M5.begin();
    M5.IMU.Init();

    ///////////////////////////////////////////////////////////
    // Initialize Sensors
    ///////////////////////////////////////////////////////////
    // Initialize VCNL4040
    if (!vcnl4040.begin()) {
        Serial.println("Couldn't find VCNL4040 chip");
        while (1) delay(1);
    }
    Serial.println("Found VCNL4040 chip");

    // Initialize SHT40
    if (!sht4.begin())
    {
        Serial.println("Couldn't find SHT4x");
        while (1) delay(1);
    }
    Serial.println("Found SHT4x sensor");
    sht4.setPrecision(SHT4X_HIGH_PRECISION);
    sht4.setHeater(SHT4X_NO_HEATER);

    ///////////////////////////////////////////////////////////
    // Connect to WiFi
    ///////////////////////////////////////////////////////////
    WiFi.begin(wifiNetworkName.c_str(), wifiPassword.c_str());
    Serial.printf("Connecting");
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        Serial.print(".");
    }
    Serial.print("\n\nConnected to WiFi network with IP address: ");
    Serial.println(WiFi.localIP());

    ///////////////////////////////////////////////////////////
    // Init time connection
    ///////////////////////////////////////////////////////////
    timeClient.begin();
    timeClient.setTimeOffset(3600 * -7);
}

///////////////////////////////////////////////////////////////
// Put your main code here, to run repeatedly
///////////////////////////////////////////////////////////////
void loop()
{
    // Read in button data and store
    M5.update();

    if (M5.BtnB.wasPressed()) {
        if (screen == S_Live) {
            screen = S_Cloud;
        } else {
            screen = S_Live;
        }
        stateChangedThisLoop = true;
        lastTime = millis();
    }

    ///////////////////////////////////////////////////////////
    // Read Sensor Values
    ///////////////////////////////////////////////////////////
    // Read VCNL4040 Sensors
    Serial.printf("Live/local sensor readings:\n");
    uint16_t prox = vcnl4040.getProximity();
    uint16_t ambientLight = vcnl4040.getLux();
    uint16_t whiteLight = vcnl4040.getWhiteLight();
    Serial.printf("\tProximity: %d\n", prox);
    Serial.printf("\tAmbient light: %d\n", ambientLight);
    Serial.printf("\tRaw white light: %d\n", whiteLight);

    // Read SHT40 Sensors
    sensors_event_t rHum, temp;
    sht4.getEvent(&rHum, &temp); // populate temp and humidity objects with fresh data
    Serial.printf("\tTemperature: %.2fF\n", convertCintoF(temp.temperature));
    Serial.printf("\tHumidity: %.2f %%rH\n", rHum.relative_humidity);

    // Read M5's Internal Accelerometer (MPU 6886)
    float accX;
    float accY;
    float accZ;
    M5.IMU.getAccelData(&accX, &accY, &accZ);
    accX *= 9.8;
    accY *= 9.8;
    accZ *= 9.8;
    Serial.printf("\tAccel X=%.2fm/s^2\n", accX);        
    Serial.printf("\tAccel Y=%.2fm/s^2\n", accY);
    Serial.printf("\tAccel Z=%.2fm/s^2\n", accZ);

    ///////////////////////////////////////////////////////////
    // Setup data to upload to Google Cloud Functions
    ///////////////////////////////////////////////////////////
    // Get current time as timestamp of last update
    timeClient.update();
    unsigned long epochTime = timeClient.getEpochTime();
    unsigned long long epochMillis = ((unsigned long long)epochTime)*1000;
    struct tm *ptm = gmtime ((time_t *)&epochTime);
    Serial.printf("\nCurrent Time:\n\tEpoch (ms): %llu", epochMillis);
    Serial.printf("\n\tFormatted: %d/%d/%d ", ptm->tm_mon+1, ptm->tm_mday, ptm->tm_year+1900);
    Serial.printf("%02d:%02d:%02d%s\n\n", timeClient.getHours() % 12, timeClient.getMinutes(), timeClient.getSeconds(), timeClient.getHours() < 12 ? "AM" : "PM");
    
    // Dummy User ID
    String userId = "MyUserName";
    
    // Device details
    deviceDetails details;

    deviceDetails latestDocDetails;

    ///////////////////////////////////////////////////////////
    // Post data (and possibly file)
    ///////////////////////////////////////////////////////////
    // Option 1: Just post data
    if ((millis() - lastTime) > timerDelay) {
        Serial.println("Posting new data");
        gcfGetWithHeader(URL_GCF_UPLOAD, userId, epochTime, &details);
        Serial.println("Done Posting New Data");
        Serial.println("Getting the new data");
        gcfGetWithUserHeader(URL_GCF_RETRIEVE, userId, &latestDocDetails);
        stateChangedThisLoop = true;
        lastTime = millis();
    }
        details.prox = prox;
        details.ambientLight = ambientLight;
        details.whiteLight = whiteLight;
        details.temp = temp.temperature;
        details.rHum = rHum.relative_humidity;
        details.accX = accX;
        details.accY = accY;
        details.accZ = accZ;
        details.timeCaptured = epochTime;
        details.cloudUploadTime = 0;
    // Changing to and from screens
    if (stateChangedThisLoop) {
        if (screen == S_Cloud) {
        if (gotNewDetails) {
            M5.Lcd.fillScreen(BLACK);
            M5.Lcd.setCursor(120, 10);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setTextSize(1);
            M5.Lcd.print("Cloud Data");
            M5.Lcd.setCursor(10, 50);
            M5.Lcd.print("Temp: ");
            M5.Lcd.print(latestDocDetails.temp);
            M5.Lcd.setCursor(10, 100);
            M5.Lcd.print("Humidity: ");
            M5.Lcd.print(latestDocDetails.rHum);
            M5.Lcd.setCursor(10, 150);
            M5.Lcd.print("Time: ");
            M5.Lcd.print(latestDocDetails.timeCaptured);
            M5.Lcd.setCursor(10, 200);
            M5.Lcd.print("Cloud Time: ");
            M5.Lcd.print(latestDocDetails.cloudUploadTime);
        }
        } else if(screen == S_Live){
            M5.Lcd.fillScreen(BLACK);
            M5.Lcd.setCursor(120, 10);
            M5.Lcd.setTextColor(WHITE);
            M5.Lcd.setTextSize(1);
            M5.Lcd.print("Live Data");
            M5.Lcd.setCursor(10, 50);
            M5.Lcd.print("Temp: ");
            M5.Lcd.print(details.temp);
            M5.Lcd.setCursor(10, 100);
            M5.Lcd.print("Humidity: ");
            M5.Lcd.print(details.rHum);
            M5.Lcd.setCursor(10, 150);
            M5.Lcd.print("Time: ");
            M5.Lcd.print(details.timeCaptured);
            M5.Lcd.setCursor(10, 200);
            M5.Lcd.print("Cloud Time: ");
            M5.Lcd.print(details.cloudUploadTime);
        }
        
    }
    stateChangedThisLoop = false;
    gotNewDetails = false;
}

////////////////////////////////////////////////////////////////////
// This method takes in a user ID, time and structure describing
// device details and makes a GET request with the data. 
////////////////////////////////////////////////////////////////////
bool gcfGetWithHeader(String serverUrl, String userId, time_t time, deviceDetails *details) {
    // Allocate arrays for headers
	const int numHeaders = 1;
    String headerKeys [numHeaders] = {"M5-Details"};
    String headerVals [numHeaders];

    // Add formatted JSON string to header
    headerVals[0] = generateM5DetailsHeader(userId, time, details);
    
    // Attempt to post the file
    Serial.println("Attempting post data.");
    int resCode = httpGetWithHeaders(serverUrl, headerKeys, headerVals, numHeaders);
    
    // Return true if received 200 (OK) response
    return (resCode == 200);
}

bool gcfGetWithUserHeader(String serverUrl, String userId, deviceDetails *latestDocDetails) {
    // Allocate arrays for headers
	const int numHeaders = 1; 
    String headerKeys [numHeaders] = {"USER-ID"};
    String headerVals [numHeaders];

    // Add formatted JSON string to header
    headerVals[0] = generateUserIdHeader(userId);
    
    // Attempt to post the file
    Serial.println("Attempting post data.");
    int resCode = httpGetLatestWithHeaders(serverUrl, headerKeys, headerVals, numHeaders, latestDocDetails);
    
    // Return true if received 200 (OK) response
    return (resCode == 200);
}

String generateUserIdHeader(String userId) {
    // Allocate M5-Details Header JSON object
    StaticJsonDocument<650> objHeaderUserIdDetails; //DynamicJsonDocument  objHeaderGD(600);
    
    // Add VCNL details
    JsonObject objUserId = objHeaderUserIdDetails.createNestedObject("userId");
    objUserId["userId"] = userId;

    // Convert JSON object to a String which can be sent in the header
    size_t jsonSize = measureJson(objHeaderUserIdDetails) + 1;
    char cHeaderUserIdDetails [jsonSize];
    serializeJson(objHeaderUserIdDetails, cHeaderUserIdDetails, jsonSize);
    String strHeadeUserIdDetails = cHeaderUserIdDetails;

    // Return the header as a String
    return strHeadeUserIdDetails ;
}

////////////////////////////////////////////////////////////////////
// TODO 4: Implement function
// Generates the JSON header with all the sensor details and user
// data and serializes to a String.
////////////////////////////////////////////////////////////////////
String generateM5DetailsHeader(String userId, time_t time, deviceDetails *details) {
    // Allocate M5-Details Header JSON object
    StaticJsonDocument<650> objHeaderM5Details; //DynamicJsonDocument  objHeaderGD(600);
    
    // Add VCNL details
    JsonObject objVcnlDetails = objHeaderM5Details.createNestedObject("vcnlDetails");
    objVcnlDetails["prox"] = details->prox;
    objVcnlDetails["al"] = details->ambientLight;
    objVcnlDetails["rwl"] = details->whiteLight;

    // Add SHT details
    JsonObject objShtDetails = objHeaderM5Details.createNestedObject("shtDetails");
    objShtDetails["temp"] = details->temp;
    objShtDetails["rHum"] = details->rHum;

    // Add M5 Sensor details
    JsonObject objM5Details = objHeaderM5Details.createNestedObject("m5Details");
    objM5Details["ax"] = details->accX;
    objM5Details["ay"] = details->accY;
    objM5Details["az"] = details->accZ;

    // Add Other details
    JsonObject objOtherDetails = objHeaderM5Details.createNestedObject("otherDetails");
    objOtherDetails["timeCaptured"] = time;
    objOtherDetails["userId"] = userId;

    // Convert JSON object to a String which can be sent in the header
    size_t jsonSize = measureJson(objHeaderM5Details) + 1;
    char cHeaderM5Details [jsonSize];
    serializeJson(objHeaderM5Details, cHeaderM5Details, jsonSize);
    String strHeaderM5Details = cHeaderM5Details;
    //Serial.println(strHeaderM5Details.c_str()); // Debug print

    // Return the header as a String
    return strHeaderM5Details;
}

////////////////////////////////////////////////////////////////////
// This method takes in a serverURL and array of headers and makes
// a GET request with the headers attached and then returns the response.
////////////////////////////////////////////////////////////////////
int httpGetWithHeaders(String serverURL, String *headerKeys, String *headerVals, int numHeaders) {
    // Make GET request to serverURL
    HTTPClient http;
    Serial.println("Starting HTTP");
    http.begin(serverURL.c_str());
    
	////////////////////////////////////////////////////////////////////
	// TODO 5: Add all the headers supplied via parameter
	////////////////////////////////////////////////////////////////////
    for (int i = 0; i < numHeaders; i++)
        http.addHeader(headerKeys[i].c_str(), headerVals[i].c_str());
    Serial.println("Added Headers");

    Serial.println("Posting the headers");
    int httpResCode = http.GET();
    Serial.print(http.getString());

    // Free resources and return response code
    http.end();
    return httpResCode;
}

int httpGetLatestWithHeaders(String serverURL, String *headerKeys, String *headerVals, int numHeaders, deviceDetails *details) {
    // Make GET request to serverURL
    HTTPClient http;
    Serial.println("Starting Http");
    http.begin(serverURL.c_str());
    
	////////////////////////////////////////////////////////////////////
	// TODO 5: Add all the headers supplied via parameter
	////////////////////////////////////////////////////////////////////
    for (int i = 0; i < numHeaders; i++)
        http.addHeader(headerKeys[i].c_str(), headerVals[i].c_str());
    Serial.println("Added Headers");

    // Post the headers (NO FILE)
    int httpResCode = http.GET();
    String result = http.getString();

    Serial.println("Result:");
    Serial.println(result);

    if (httpResCode == 200) {
        
    Serial.println("deserializing");
    StaticJsonDocument<850> objHeaderM5Details;

    DeserializationError error = deserializeJson(objHeaderM5Details, result);
    if (error) {
        Serial.print("deserializeJson() failed: ");
        Serial.println(error.c_str());
    }

    Serial.println("deserialized");
    String cloudTime = objHeaderM5Details["otherDetails"]["cloudUploadTime"];
    Serial.print("cloud time:");
    Serial.println(cloudTime);
    String timeCaptured = objHeaderM5Details["otherDetails"]["timeCaptured"];
    Serial.print("time caputred:");
    Serial.println(timeCaptured);
    String temp = objHeaderM5Details["shtDetails"]["temp"];
    Serial.print("temperature:");
    Serial.println(temp);
    String humidity = objHeaderM5Details["shtDetails"]["rHum"];
    Serial.print("humidity:");
    Serial.println(humidity);

    Serial.println("converting details:");
    char* endPtr;
    details->cloudUploadTime = std::strtoll(cloudTime.c_str(), &endPtr, 10);
    Serial.print("cloud time:");
    Serial.println(details->cloudUploadTime);
    Serial.println("Converted cloud time");
    char* endPtr2;
    details->timeCaptured = std::strtoll(timeCaptured.c_str(), &endPtr2, 10);
    Serial.println("Converted time caputred");
    details->temp = temp.toDouble();
    Serial.println("Converted temp");
    details->rHum = humidity.toDouble();
    Serial.println("Converted humidty");
    }
    Serial.println("Done converting");
    // Free resources and return response code
    http.end();
    Serial.println("Ended HTTP");
    gotNewDetails = true;
    return httpResCode;
}

////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////
// TODO 6: Implement method
// Given an array of bytes, writes them out to the SD file system.
////////////////////////////////////////////////////////////////////
String writeDataToFile(byte * fileData, size_t fileSizeInBytes) {
    // Print status
    Serial.println("Attempting to write file to SD Card...");

    // Obtain file system from SD card
    fs::FS &sdFileSys = SD;

    // Generate path where new picture will be saved on SD card and open file
    int fileNumber = getNextFileNumFromEEPROM();
    String path = "/file_" + String(fileNumber) + ".txt";
    File file = sdFileSys.open(path.c_str(), FILE_WRITE);

    // If file was opened successfully
    if (file)
    {
        // Write image bytes to the file
        Serial.printf("\tSTATUS: %s FILE successfully OPENED\n", path.c_str());
        file.write(fileData, fileSizeInBytes); // payload (file), payload length
        Serial.printf("\tSTATUS: %s File successfully WRITTEN (%d bytes)\n\n", path.c_str(), fileSizeInBytes);

        // Update picture number
        EEPROM.write(0, fileNumber);
        EEPROM.commit();
    }
    else {
        Serial.printf("\t***ERROR: %s file FAILED OPEN in writing mode\n***", path.c_str());
        return "";
    }

    // Close file
    file.close();

    // Return file name
    return path;
}

////////////////////////////////////////////////////////////////////
// TODO 7: Implement Method
// Get file number by reading last file number in EEPROM (non-volatile
// memory area).
////////////////////////////////////////////////////////////////////
int getNextFileNumFromEEPROM() {
    #define EEPROM_SIZE 1             // Number of bytes you want to access
    EEPROM.begin(EEPROM_SIZE);
    int fileNumber = 0;               // Init to 0 in case read fails
    fileNumber = EEPROM.read(0) + 1;  // Variable to represent file number
    return fileNumber;
}

////////////////////////////////////////////////////////////////////
// TODO 8: Implement Method
// This method takes in an SD file path, user ID, time and structure
// describing device details and POSTs it. 
////////////////////////////////////////////////////////////////////
bool gcfPostFile(String serverUrl, String filePathOnSD, String userId, time_t time, deviceDetails *details) {
    // Allocate arrays for headers
    const int numHeaders = 3;
    String headerKeys [numHeaders] = { "Content-Type", "Content-Disposition", "M5-Details"};
    String headerVals [numHeaders];

    // Content-Type Header
    headerVals[0] = "text/plain";
    
    // Content-Disposition Header
    String filename = filePathOnSD.substring(filePathOnSD.lastIndexOf("/") + 1);
    String headerCD = "attachment; filename=" + filename;
    headerVals[1] = headerCD;

    // Add formatted JSON string to header
    headerVals[2] = generateM5DetailsHeader(userId, time, details);
    
    // Attempt to post the file
    int numAttempts = 1;
    Serial.printf("Attempting upload of %s...\n", filename.c_str());
    int resCode = httpPostFile(serverUrl, headerKeys, headerVals, numHeaders, filePathOnSD);
    
    // If first attempt failed, retry...
    while (resCode != 200) {
        // ...up to 9 more times (10 tries in total)
        if (++numAttempts >= 10)
            break;

        // Re-attempt
        Serial.printf("*Re-attempting upload (try #%d of 10 max tries) of %s...\n", numAttempts, filename.c_str());
        resCode = httpPostFile(serverUrl, headerKeys, headerVals, numHeaders, filePathOnSD);
    }

    // Return true if received 200 (OK) response
    return (resCode == 200);
}

////////////////////////////////////////////////////////////////////
// TODO 9: Implement Method
// This method takes in a serverURL and file path and makes a 
// POST request with the file (to upload) and then returns the response.
////////////////////////////////////////////////////////////////////
int httpPostFile(String serverURL, String *headerKeys, String *headerVals, int numHeaders, String filePath) {
    // Make GET request to serverURL
    HTTPClient http;
    http.begin(serverURL.c_str());
    
    // Add all the headers supplied via parameter
    for (int i = 0; i < numHeaders; i++)
        http.addHeader(headerKeys[i].c_str(), headerVals[i].c_str());
    
    // Open the file, upload and then close
    fs::FS &sdFileSys = SD;
    File file = sdFileSys.open(filePath.c_str(), FILE_READ);
    int httpResCode = http.sendRequest("POST", &file, file.size());
    file.close();

    // Print the response code and message
    Serial.printf("\tHTTP%scode: %d\n\t%s\n\n", httpResCode > 0 ? " " : " error ", httpResCode, http.getString().c_str());

    // Free resources and return response code
    http.end();
    return httpResCode;
}

/////////////////////////////////////////////////////////////////
// Convert between F and C temperatures
/////////////////////////////////////////////////////////////////
double convertFintoC(double f) { return (f - 32) * 5.0 / 9.0; }
double convertCintoF(double c) { return (c * 9.0 / 5.0) + 32; }