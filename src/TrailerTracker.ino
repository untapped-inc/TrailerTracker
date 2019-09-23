/*
 * Project TrailerTracker
 * Description: This Particle Boron code was written to collect GPS, Flowmeter, and Eletrical Current data from Untapped's trailers in Haiti
 * Author: JJ OBrien/Blue Frog Code
 * Date: September 2019
 */

void setup() {
    Serial.begin(115200);
    Serial1.begin(9600);
}

void loop() {
    
    while (Serial1.available()){
        Serial.print((char)Serial1.read());
    }    

    //2 HZ
    delay(500);
}