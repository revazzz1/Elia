// DFPlayer Mini with Arduino by ArduinoYard
#include "SoftwareSerial.h"
#include "DFRobotDFPlayerMini.h"

void buttonHandler();

const uint8_t buttonPins[] = {3,4,5}; //[prev, play, next]
const int debounceTime = 50;
const int numTracks = 4;
const int maxVolume = 30;

int buttons[] = {0, 0, 0}; 
int prevButtons[] = {0, 0, 0};
int prevButtonTimes[] = {0, 0, 0};

int currentTrack = 1;
int currentVolume = 20;

SoftwareSerial mySerial(10, 11); // RX, TX
DFRobotDFPlayerMini myDFPlayer;

void setup() {
    Serial.begin(9600);
    mySerial.begin(9600);

    for(int i = 0; i < 3; i++){
        pinMode(buttonPins[i], INPUT_PULLUP);
    }
    
    
    if (!myDFPlayer.begin(mySerial)) {
        Serial.println("DFPlayer Mini not detected!");
        while (true);
    }
    
    Serial.println("DFPlayer Mini ready!");
    myDFPlayer.volume(currentVolume);  // Set volume (0 to 30)
    Serial.println("Playing File 1");
    myDFPlayer.play(currentTrack);      // Play first MP3 file
}

void loop() {
    buttonHandler();
    Serial.println("buttons: [" + String(buttons[0]) + ", " + String(buttons[1]) + ", " + String(buttons[2]) + "], volume: " + String(currentVolume));
    if(buttons[2] > 0){ //increase volume
        if(currentVolume + 1 <= maxVolume){
            Serial.println("Increasing volume");
            currentVolume++;
            myDFPlayer.volume(currentVolume);
        }
    }
    else if (buttons[0] > 0) {
        if(currentVolume - 1 >= 0){
            Serial.println("Lowering volume");
            currentVolume--;
            myDFPlayer.volume(currentVolume);
        }
    }
    delay(100);
}

void buttonHandler(){ // -1=just released, 0=not pressed, 1=held down, 2=just pressed
    for(int i = 0; i < 3; i++){
        int b = !digitalRead(buttonPins[i]);
        if(b == HIGH){ //button held
            if(prevButtons[i] <= 0){ //not previously held
                if((millis() - prevButtonTimes[i] >= debounceTime)){ //chech for bounce
                    buttons[i] = 2;
                    prevButtonTimes[i] = millis();
                }
                else{
                    buttons[i]=0;
                }
            }
            else{
                buttons[i]=1;
            }
        }
        else {
            if(prevButtons[i] > 0){
                buttons[i] = -1;
            }
            else{
                buttons[i]=0;
            }
        }
    }

    for(int i = 0; i < 3; i++){
        if(buttons[i] > 0){
            prevButtons[i]=1;
        }
        else{
            prevButtons[i]=0;
        }
    }

}