#include "Tape.h"
#include "Ascii.h"
#include "Manager.h"
#include "Sharp.h"


const int TAPE_BUFFER_SIZE = 64;        // The size of the serial and tape buffers
byte serialBuffer[TAPE_BUFFER_SIZE];    // Serial receive buffer
byte tapeBuffer[TAPE_BUFFER_SIZE];      // The tape buffer
int serialBufferCount = 0;              // The number of bytes to read into the serial buffer (no larger than TAPE_BUFFER_SIZE)
int serialBufferIndex = 0;              // The index of the next position to read into the buffer
int tapeBufferCount = 0;                // The number of items in the tape buffer
int tapeBufferIndex = 0;                // The next byte to process in the tape buffer

const int ErrorTimeout = -1;            // Read timeout error
const int ErrorSync = -2;               // Sync error

/**
 * @brief Loads data from the serial port to the pocket computer
 */
void Tape::Load()
{
    // Read the load tape packet information
    int length = Manager::WaitReadWord();            // 2 bytes, total length of program
    if (length == -1) {
        Manager::SendFailure(ErrorCode::Timeout);
        return;
    }

    int remaining = length;                 // Number of bytes remaining to be read from PC

    int headerCount = Manager::WaitReadByte();       // Number of bytes that make up the header
    if (length == -1) {
        Manager::SendFailure(ErrorCode::Timeout);
        return;
    }

    // Parsing the header was a success
    Manager::SendSuccess();

    // Reset the buffers
    tapeBufferCount = 0;
    tapeBufferIndex = 0;
    serialBufferCount = min(TAPE_BUFFER_SIZE, remaining);
    serialBufferIndex = 0;

    // Has the tape prefix been sent
    bool sentPrefix = false;

    while (true) 
    {
        // If the write buffer contains unsent bytes
        if (tapeBufferIndex < tapeBufferCount) {
            // Send the prefix if it isn't already sent
            if (!sentPrefix) {
                digitalWrite(LED_BUILTIN, HIGH);
                // Send the prefix  
                for (int i = 0; i < 250; i++) SendTapeBit(1);    
                sentPrefix = true;
            }
            // Send a single byte to the pocket computer
            SendTapeByte(tapeBuffer[tapeBufferIndex++], headerCount > 0);
            // Decrement the header count
            if (headerCount > 0) headerCount--;
        }
        else
        {
            // If write buffer empty and no remaining bytes, leave
            if (remaining == 0) break;
        }

        // If a byte is available, add to read buffer
        if (Serial.available() > 0 && serialBufferIndex < serialBufferCount) {
            serialBuffer[serialBufferIndex++] = Serial.read();
        }    

        // If the read buffer is full and the write buffer is full
        // Copy the read buffer into the write buffer and receive another packet
        if (serialBufferIndex == serialBufferCount && tapeBufferIndex == tapeBufferCount) {
            remaining -= serialBufferCount;
            memcpy(tapeBuffer, serialBuffer, serialBufferCount);
            tapeBufferCount = serialBufferCount;
            tapeBufferIndex = 0;
            serialBufferCount = min(TAPE_BUFFER_SIZE, remaining);
            serialBufferIndex = 0;
            // Acknowledge the read buffer
            Manager::SendSuccess();
        }
    }

    // End with 2 stop bits
    for (int sb = 0; sb < 2; sb++) SendTapeBit(1);
    digitalWrite(LED_BUILTIN, LOW);      
}


/**
 * @brief Sends a single byte over the tape interface
 * @param value     Byte to send
 * @param header    True if header byte else data byte
 */
void SendTapeByte(byte value, bool header)
{
    SendTapeBit(0);
    for (int j = 4; j < 8; j++) SendTapeBit(bitRead(value, j));
    SendTapeBit(1);
    SendTapeBit(0);
    for (int j = 0; j < 4; j++) SendTapeBit(bitRead(value, j));
    // Send 5 or 2 stop bits
    for (int sb = 0; sb < (header ? 5 : 2); sb++) SendTapeBit(1);
}


/**
 * @brief Sends a single bit over the tap interface
 * @param bit   The bit to send
 */
void SendTapeBit(bool bit)
{
    const int pulse8 = 125;  // μs, short pulse CSAVE/CLOAD (8 x pulse8 = HIGH)
    const int pulse4 = 250;  // μs, long  pulse CSAVE/CLOAD (4 x pulse4 = LOW)

    // For every bit, attempt to read one byte from the buffer
    if (Serial.available() > 0 && serialBufferIndex < serialBufferCount) {
        serialBuffer[serialBufferIndex++] = Serial.read();
    }

    // Pulse the bits
    if (bit) { // Bit = 1
        for (int z = 0; z < 8; z++) TapePulseOut(pulse8); // 8 short pulses = HIGH
    }
    if (!bit) { // Bit = 0
        for (int z = 0; z < 4; z++) TapePulseOut(pulse4); // 4 long pulses = LOW
    }    
}


/**
 * @brief Sends a square wave pulse to the pocket computer
 * @param duration Number of microseconds to be on and off
 */
void TapePulseOut(int duration) 
{
    digitalWrite(SHARP_XIN, HIGH);
    delayMicroseconds(duration);
    digitalWrite(SHARP_XIN, LOW);
    delayMicroseconds(duration);
}

/**
 * @brief Saves data from the pocket computer to the serial port
 */
void Tape::Save()
{
    Serial.println("Waiting for CSAVE...");

    // Wait for xout to go high
    unsigned long startTimeout = millis();
    while (!digitalRead(SHARP_XOUT)) {      
        if ((millis() - startTimeout) > 10000) {
            Serial.println("Timeout");
            return;
        }
    }  

    // Read the device select
    int device = Sharp::ReadDeviceSelect(); 
    Serial.print("Device select: 0x");
    Serial.println(device, HEX);

    Serial.println("Starting sync...");
    unsigned long startTime = TapeReadSync();
    if (startTime == 0) {
        Serial.println("Timeout");
        return;
    }

    // Serial.println("Reading tape data...");
    bool headerMarker = false;                      // Have we see the end of header byte
    bool header = true;                             // Are we in the header portion
    int value = 0;

    while (true) {
        // Start bit
        if (ReadStartBit(startTime)) {
            value = ReadTapeByte();
            if (headerMarker) header = false;           // Read one byte past header marker
            if (value == 0x5F) headerMarker = true;     // Read the header marker
            if (value >= 0) {
                Serial.print(value, HEX);
                Serial.print(" ");
            } else {
                if (value == ErrorTimeout) Serial.println("Timeout");
                else if (value == ErrorSync) Serial.println("error");
                Serial.println(value);
                break;
            }
        }
        // Resync
        unsigned long syncTime = micros();
        startTime = TapeReadSync();
        if ((micros() - syncTime) > 10000) {
            Serial.println();
            Serial.println("Done.");
            return;
        }
    }
}

bool TapeReadSync(unsigned long &startTime, int &totalSync)
{
    unsigned long syncStart = micros();
    bool result = TapeReadSync(startTime);
    totalSync = micros() - syncStart;
    return result;
}

bool TapeReadSync(unsigned long &startTime)
{
    while (true) {
        // Wait for HIGH of XOUT and return zero if timed out
        unsigned long startTimeout = millis();
        while (!digitalRead(SHARP_XOUT)) {      
            if ((millis() - startTimeout) > 1000) return false;       
        }  
        // Potential start time of the data
        startTime = micros();
        // Wait for low transition
        while (digitalRead(SHARP_XOUT));       

        // Wait for HIGH of XOUT and return zero if timed out
        startTimeout = millis();
        while (!digitalRead(SHARP_XOUT)) {    
            if ((millis() - startTimeout) > 1000) return false;       
        }  

        // Calculate the duration of the pulse
        unsigned long duration = micros() - startTime;
        // If duration is greater than 270 then sync over and return start time
        if (duration >= 270) return true;
    }
}


unsigned long TapeReadSync()
{
    while (true) {
        // Wait for HIGH of XOUT and return zero if timed out
        unsigned long startTimeout = millis();
        while (!digitalRead(SHARP_XOUT)) {      
            if ((millis() - startTimeout) > 1000) return 0;       
        }  
        // Potential start time of the data
        unsigned long startTime = micros();
        // Wait for low transition
        while (digitalRead(SHARP_XOUT));       

        // Wait for HIGH of XOUT and return zero if timed out
        startTimeout = millis();
        while (!digitalRead(SHARP_XOUT)) {    
            if ((millis() - startTimeout) > 1000) return 0;       
        }  

        // Calculate the duration of the pulse
        unsigned long duration = micros() - startTime;
        // If duration is greater than 270 then sync over and return start time
        if (duration >= 270) return startTime;
    }
}

int ReadTapeByte()
{
    int result = 0;
    for (int j = 4; j < 8; j++) bitWrite(result, j, ReadTapeBit());
    if (!ReadStopBit()) return -1;      // Stop bit
    if (!ReadStartBit()) return -2;       // Start bit
    for (int j = 0; j < 4; j++) bitWrite(result, j, ReadTapeBit());
    if (!ReadStopBit()) return -3;      // Stop bit      
    return result;  
}


bool ReadTapeBit(unsigned long startTime)
{
    const int samplePeriod = 2000;
    int previousState = 0;
    int pulseCount = 0;
    while (micros() - startTime < samplePeriod)
    {
        int currentState = digitalRead(SHARP_XOUT);
        if (previousState != currentState) {
            pulseCount++;
            previousState = currentState;
        }        
    }

    pulseCount /= 2; // Divide by 2 to get the number of cycles (full square waves)
    return pulseCount >= 6 ? 1 : 0; // If 6 or more cycles are detected, it's a 1 bit, otherwise it's a 0 bit
}

bool ReadTapeBit() { ReadTapeBit(micros()); }
bool ReadStartBit(unsigned long startTime) { return !ReadTapeBit(startTime); }
bool ReadStartBit() { return !ReadTapeBit(); }
bool ReadStopBit() { return ReadTapeBit(); }