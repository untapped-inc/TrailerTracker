// #pragma SPARK_NO_PREPROCESSOR // see https://community.particle.io/t/c-struct-as-parameter-to-methods/27065 as to why this is necessary
#include "Particle.h"
#include "HttpClient.h"
/*
 * Project TrailerTracker
 * Description: This Particle Boron code was written to collect GPS, Flowmeter, and Eletrical Current data from Untapped's trailers in Haiti
 * Author: JJ OBrien/Blue Frog Code
 * Date: September 2019
 */

//define a struct to hold GPS data
struct Coordinates {
    //latitude formatted as a floating-point decimal
    float Latitude;
    float Longitude;
};

//function definitions
Coordinates getGPSCoordinates();
float averageArray(float *latArray);
float formatCoordinate(float coordinate);
void flowmeterPulseDetected();
double getVolume();
void sendData(float longitude, float latitude, double volume);

/** BEGIN CONSTANTS **/

//GPS won't always be 100% precise - take a number of samples and average the result to show the approx location
const int MIN_GPS_SAMPLES = 5;
//buffer used to store incoming characters from GPS module
const int BUFFER_SIZE = 300;
//the prefix that identifies the line of the GPS transmission that we pull the lat and long from
const char* GPS_ID = "GPGGA";
const int FLOWMETER_PIN = A0;
//this value comes from the datasheet of whatever flowmeter you are using - change to whatever your flowmeter specifies
const double LITERS_PER_PULSE = 0.0022;
//this is used to send data to the API endpoint
const char* HOSTNAME = "api.thingspeak.com";
const char* PATH = "/update?api_key=X0AVGKDUHRKS2GK9&field1=%.5f&field2=%.5f&field3=%.4f&field4=%.2f";
//set addresses for the pulse variables in EEPROM memory
const int PULSEA_ADDRESS = 1;
const int PULSEB_ADDRESS = 20;

//the number of loops between transmissions
const int CYCLES_PER_TRANSMISSION = 10;

/** END CONSTANTS **/

//global to track the total pulses passed through the pipes
long pulseA = 0l;
long pulseB = 0l;
bool isPulseA = true;

//track loops between transmissions
int cycleNumber = 0;

//variables to manage http connections
HttpClient http;
http_request_t request;
http_response_t response;
//HTTP request headers
http_header_t headers[] = {
    { "Accept" , "application/json" },
    { NULL, NULL } // application.ino says that terminating the headers is necessary
};


void setup() {
    Serial.begin(115200);
    //begin Serial 1 at 9600 baudrate to read GP-20U7 module from RX pin
    Serial1.begin(9600);

    //setup the flowmeter analog pin with an interrupt
    pinMode(FLOWMETER_PIN, INPUT);                                     
    attachInterrupt(FLOWMETER_PIN, flowmeterPulseDetected, RISING); 

    delay(1000);   

    //check to see if anything is stored in non-volatile memory to recover data that existed prior to a crash/shutdown
    long pulseAMemory;
    long pulseBMemory;
    EEPROM.get(PULSEA_ADDRESS, pulseAMemory);
    EEPROM.get(PULSEB_ADDRESS, pulseBMemory);
    Serial.print("Pulse A Memory: ");
    Serial.println(pulseAMemory);
    Serial.print("Pulse B Memory");
    Serial.println(pulseBMemory);

    if (pulseAMemory > 0 && pulseBMemory > 0){
      //if both have values, set volatile variables with the contents of the data in memory
      pulseA = pulseAMemory;
      pulseB = pulseBMemory;
    }else if(pulseAMemory > 0){
      //if only one has a value, then start with that pulse
      pulseA = pulseAMemory;
      isPulseA = true;
    }else if(pulseBMemory > 0){
      pulseB = pulseBMemory;
      isPulseA = false;
    }
}

void loop() {

  //send data once every 10 cycles - todo: find a good frequency from Jim
  if (cycleNumber >= CYCLES_PER_TRANSMISSION){
    Coordinates currentLocation = getGPSCoordinates();
    Serial.print("Location: ");
    Serial.printf("%.5f", currentLocation.Latitude);
    Serial.print(", ");
    Serial.printf("%.5f", currentLocation.Longitude);
    Serial.println();
    Serial.print("Volume: ");
    double volumeInLiters = getVolume();
    Serial.print(volumeInLiters);
    Serial.println();

    sendData(currentLocation.Longitude, currentLocation.Latitude, volumeInLiters);
  }

  //periodically save to EEPROM - this helps minimize data loss in the event of a crash
  EEPROM.put(PULSEA_ADDRESS, pulseA);
  EEPROM.put(PULSEA_ADDRESS, pulseB);

  cycleNumber++;

  delay(100);
}

//helper function to get the GPS coordinates from the GPS module
Coordinates getGPSCoordinates(){
  Coordinates averagedCoordinates;
  int sampleCount = 0;
  bool isGGA = false; //track whether we are reading the GGA line or not
  char listenBuffer[BUFFER_SIZE];
  char incoming;
  int bufferCursor = 0;
  int commaCount = 0; //count the number of commas in the line
  char longitudeBuffer[10];
  char latitudeBuffer[10];
  int longLatCursor = 0;
  //use these arrays to track the GPS longitude and latitudes until we average them together
  float longArray[MIN_GPS_SAMPLES];
  float latArray[MIN_GPS_SAMPLES];
  bool isEast = true;
  bool isNorth = true;

  //lots of data is returned by the GP-20U7 module (See datasheet for format) 
  //parse out the line that begins with $GPGGA - that is the one we want to use
  //here's an example of what it looks like: $GPGGA,162926.00,3233.02295,N,08454.10047,W,1,03,5.30,124.0,M,-30.6,M,,*6E
  while (sampleCount < MIN_GPS_SAMPLES){
    while (Serial1.available()){
        //read one character at a time
        incoming = (char)Serial1.read();
        
        //dollar sign indicates a new line
        if (incoming == '$'){
          //Serial.println(listenBuffer);
          commaCount = 0; //reset comma count
          //clear out the old data in the buffer
          memset(listenBuffer, 0, sizeof listenBuffer);
          bufferCursor = 0;        
          //reset isGGA
          isGGA = false;
        }else{
          listenBuffer[bufferCursor++] = incoming;
         
          //check to see if the prefix matches     
          if ((bufferCursor == strlen(GPS_ID)) && listenBuffer[0] == GPS_ID[0] && listenBuffer[1] == GPS_ID[1] 
          && listenBuffer[2] == GPS_ID[2] && listenBuffer[3] == GPS_ID[3] && listenBuffer[4] == GPS_ID[4]){
            isGGA = true;
          }else if (isGGA){  //this logic only applies when the GGA line is being transmitted
            //Number following 2nd comma is latitude; following fourth is longitude
            if (incoming == ','){
              commaCount++;
              //reset longitude/latitude buffer cursor
              longLatCursor = 0;
              //set long and lat
              if (commaCount == 6){
                //convert to float
                float latitude = atof(latitudeBuffer);
                if (latitude > 0){
                  //dont increment sample count yet since we also need to get the longitude
                  latArray[sampleCount] = isNorth ?  formatCoordinate(latitude) : (-1.0 * formatCoordinate(latitude));
                }
                //reset the longitude buffer
                memset(latitudeBuffer, 0, sizeof latitudeBuffer);
                float longitude = atof(longitudeBuffer);
                //check that the longitude was actually read
                if (longitude > 0){
                  //increment the sample count
                  longArray[sampleCount++] = isEast ? formatCoordinate(longitude) : (-1.0 * formatCoordinate(longitude));
                }
                memset(longitudeBuffer, 0, sizeof longitudeBuffer);
              }
            } else if(commaCount == 2){
               latitudeBuffer[longLatCursor++] = incoming;
            } else if(commaCount == 3){
                if (incoming == 'S'){
                  isNorth = false;
                }
            } else if(commaCount == 4){
               longitudeBuffer[longLatCursor++] = incoming;
            } else if (commaCount == 5) {
                if (incoming == 'W'){
                  isEast = false;
                }
            }
            
          }
        }        
    } 
  }
  averagedCoordinates.Latitude = averageArray(latArray);
  averagedCoordinates.Longitude = averageArray(longArray);

  return averagedCoordinates;
}


float averageArray(float *latArray){
  float sum = 0.0;
  for (int i = 0; i < sizeof(latArray); i++){
    sum += latArray[i];
  }

  return sum / sizeof(latArray);
}

//the GP-20U7 returns latitude and longitude as ddmm.mmmm
//we want to return this as decimal degrees
float formatCoordinate(float coordinate){
  int degrees = int(coordinate/100);
  float minutes = coordinate - float(degrees * 100);
  return float(degrees) + minutes/60.0;
}


//this function should be triggered a pulse on the flowmeter's analog pin
void flowmeterPulseDetected(){
  if(isPulseA){
    pulseA++;
  }else{
    pulseB++;
  }
}

//this helper function gets the total volume through the system and toggles between pulse A and B
double getVolume(){
  double volume;
  if (isPulseA){
    volume = (pulseA * LITERS_PER_PULSE);
  }else{
    volume = (pulseB * LITERS_PER_PULSE);
  }
  //toggle which variable we store the pulse in since theflowmeter needs to be able to continue to send data even if we are in the middle of transmission
  isPulseA = !isPulseA;
  return volume;
}

void sendData(float longitude, float latitude, double volume){
  //setup API call
  request.hostname = HOSTNAME;
  char formattedRequest[300];
  sprintf(formattedRequest, PATH, longitude, latitude, volume, 1);
  request.path = formattedRequest;

  //send the data
  http.get(request, response, headers);
  //check for a successful transmission
  if (response.status == 200){
    //reset the volume that was just passed to the API endpoint if successful
    if(isPulseA){
      //we've already switched the pulse at this point, so the volume that we need to clear is the previous one
      pulseB = 0l;
    }else{
      pulseA = 0l;
    }
  }else{
    Serial.println("Request Failed. Retrying...");
    Serial.print("formattedrequest:   ");
    Serial.println(formattedRequest);
    //if the request fails, retry...
    sendData(longitude, latitude, volume);
  }
}