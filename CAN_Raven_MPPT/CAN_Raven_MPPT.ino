

/* Current Sensor, Relay, and Array Temperature Sensor Code
 *  UBC Solar
 *  Purpose: 1. To read the current output of the panels on the MPPT, determine whether the batteries are fully charged, and disconnect the panels from the MPPT if necessary
 *           2. Measure the temperature of each of the six arrays 
 *           3. Periodically (every second) send status message containing current levels and temperatures (201, length = 6 bytes)
 *           4. Recieves control message to connect or disconnect solar panels (200, length = 1 byte)
 *           
 *  Last Update: 06/09/2017
 *  Board: Arduino Mega 2560
 *  Analog Pins: A0 to A15
 *  Digital Pins: 
 *  
 *  Output: 1. Current+/-20mA of each of the 6 current sensors
 *          2. Status of 6 MPPT relays 
 *          3. Each temperature sensor status
 *          4. Battery charge status
 *          
 *  Input:  1. Temperature sensor voltage
 *          2. Current sensor voltage
 *          3. Driver controlled kill switch (enitrely mechanical)
 *          4.
 * 
 * 
 * ID       SYSTEM            LENGTH (BYTE)    FORMAT
 * 
 * 201      CURRENT SENSORS     6              frame_data[0-5] = currentOut[0-5]
 * 200      RELAY CONTROL       6              frame_data[0-5] = relay [0-5] status (0 = OFF, 1 = ON)
 * 199      TEMP SENSORS 1      5              frame_data[0-4] = tempCelsius[0-4]
 * 198      TEMP SENSORS 2      5              frame_data[0-4] = tempCelsius[5-9]
 * 197      WARNING CURRENT     1              frame_data[0] = statu(0 = OK, 1 = SHIT)
 * 196      WARNING TEMP        1              frame_data[0] = status(0 = OK, 1 = SHIT)
 * 195      RELAY STATUS        6              frame_data[0-5] = relay[0-5]
 * 
 * 
 */

#include <ubcsolar_can_ids.h>
#include <SPI.h>
#include <math.h>
#include <mcp_can.h>
#include <mcp_can_dfs.h>

#define CAN_ID_MPPT_CURRENT 201
#define CAN_ID_MPPT_CONTROL 200           // External control of relay (independent from kil switch
#define CAN_ID_MPPT_TEMP1 199             // TSensors 0-4
#define CAN_ID_MPPT_TEMP2 198             // TSensors 5-9 
#define CAN_ID_MPPT_CURRENT_WARNING 197
#define CAN_ID_MPPT_TEMP_WARNING 196     
#define CAN_ID_MPPT_RELAY_STATUS 195  
#define MAX_CURRENT  8000.0
#define MAX_TEMP 80.0                     // Celsius
#define BUS_SPEED CAN_125KBPS
#define VCC 5.05
#define CONNECT 1
#define DISCONNECT 0

// CURRENT SENSOR CONSTANTS
const float ZERO_CURRENT_VOLTAGE[6] = {2.543,2.539,2.491,2.536,2.534,2.537};      // Voltage output from VIOUT/input to A0 at current sensed is 0A (measured)
const float CURRENT_CONVERSION_FACTOR[6] = {.0687,.07,.0685,.069,.0683,.079};     // Conversion factor from current sensed to voltage read on analog pins (see ACS712T datasheet under x30A table)
const float uC_OFFSET[6] = {-2.667,-2.661,-2.619,-2.674,-2.676,-2.663};
float currentValue[6] = {0};
float currentVoltageOut[6] = {0};
float currentOut[6] = {0};

// TEMPERATURE SENSOR CONSTANTS
const float TEMP_BASE_VOLTAGE [10] = {608,617,613,607,610,610,0,0,0,0};
float baseVoltage[10] = {0}; 
float baseCelsius[10] = {0};
float tempVoltage[10] = {0};
float temp2Voltage[10] = {0};
float tempConvert[10] =  {0};                     
float tempCelsius[10] =  {0};    
float tempF[10] = {0};

// PIN CONFIG
const int panel[6] = {A0,A1,A2,A3,A4,A5};                                         // Analog input pin that the current sensor for the panel input of MPPT 0 is attached to
const int relay[6] = {0,1,2,3,4,5};                                               // Digital output pins corresponding to their respective power relays
const int temp_sensor[10] = {A6,A7,A8,A9,A10,A11,A12,A13,A14,A15};
 
// CAN SETUP -- figure out correct sizes
byte frame_data_current[6] = {0};
byte frame_data_temperature1[5] = {0};    // Will be sending in two seperate IDs because there is too much data
byte frame_data_temperature2[5] = {0};
byte warning_current = 0;                 // Overcurrent state when LSB is 1
byte warning_temp = 0;                    // Overtemperature state when LSB is 1

int data_length_current = 6;              // In bytes
int data_length_temp = 5;           

const int SPI_CS_PIN = 10;

MCP_CAN CAN(SPI_CS_PIN);

void setup() {
  
// Initialize serial communications at 115200 bps
  Serial.begin(115200); 

// Set pin modes and turn relays on
  for (int i = 0; i < 6; i++) {
    pinMode(panel[i], INPUT);
    pinMode(relay[i], OUTPUT);
    digitalWrite(relay[i], HIGH);
  }
  
  for (int x = 0; x < 10; x++) {
    pinMode(temp_sensor[x],INPUT);
  }
  
// Initialize CAN bus serial communications
  int canSSOffset = 0;
    
CAN_INIT: 

  if (CAN_OK == CAN.begin(BUS_SPEED)) {
    Serial.println("CAN BUS Shield init okay");
  } else {
        Serial.println("CAN BUS Shield init fail");
        Serial.print("Init CAN BUS Shield again with SS pin ");
        Serial.println(SPI_CS_PIN + canSSOffset);
       
        delay(100);
        
        canSSOffset ^= 1;
        CAN = MCP_CAN(SPI_CS_PIN + canSSOffset);
        goto CAN_INIT;
  }
}

void loop() {

  byte length;
  uint32_t frame_id;
  byte frame_data[8];   // Max length
  
  if(CAN_MSGAVAIL == CAN.checkReceive()) {
    CAN.readMsgBuf(&length, frame_data);
    frame_id = CAN.getCanId();   
    msgHandler(frame_id, frame_data, length);
    
  }
  
  // reading and displaying current levels, as well as relay logic
  for (int i = 0; i < 6; i++) {
    currentValue[i] = analogRead(panel[i]);
    currentVoltageOut[i] = float((currentValue[i] / 100.0) + uC_OFFSET[i]) - ZERO_CURRENT_VOLTAGE[i];      // Convert value read in sensorValue to the voltage value output of the current sensor - 2.543
    currentOut[i] = currentVoltageOut[i] / CURRENT_CONVERSION_FACTOR[i] * 1000.0 / 2.0;                    // Convert voltage value to current value, I_p, read from the current sensor
   
    Serial.print("MPPT");
    Serial.print(i);
    Serial.print(": Voltage (V) = ");
    Serial.print(currentVoltageOut[i]);
    Serial.print("\t Current (mA) = ");
    Serial.print(currentOut[0]);
    Serial.print("\n\r");

    // relay logic
    if (currentOut[i] >= MAX_CURRENT) {
      // Disconnect
      digitalWrite(relay[i], LOW);
      
      // notify user
      Serial.print( "\nMPPT" );
      Serial.print( i );
      Serial.print( " disconnected.\n\n" );

      bitSet(warning_current,0); // LSB is set to 1
      
    } else {
      digitalWrite(relay[i], HIGH);
      Serial.print( "\nMPPT" );
      Serial.print( i );
      Serial.print( " connected.\n\n" );

      bitClear(warning_current,0); // LSB is set to 0
    }
  }

// temperature reading and display
  for (int j = 0; j < 10; j++) {
    baseVoltage[j] = (TEMP_BASE_VOLTAGE[j]*VCC)/1024.0;                      // At room temperature: Convert reading to in (V) -- around 2.94
    baseCelsius[j] = (baseVoltage[j]/0.01) - 273.15;                         // At room temperature: Convert to celsius -- around 27
    tempVoltage[j] = analogRead(temp_sensor[j]);                             // Real-time (mV)   
    temp2Voltage[j] = (tempVoltage[j]*VCC)/1024;                             // Real-time (V)
//  tempConvert[j] = (temp2Voltage[j]/0.01) - 273.15;                        // Converting voltage reading into temperature   
//  tempCelsius[j] = baseCelsius[j] + (baseCelsius[j] - tempConvert[j]);     // Adding difference -- since voltage decreases as temperature increases
    tempCelsius[j] = (temp2Voltage[j] / baseVoltage) * baseCelsius;
    tempF[j] = tempCelsius[j]*(9.0/5.0) + 32.0;                              // Convert to weird american units
 
    Serial.print("LM335Z");
    Serial.print(j);
    Serial.print(": Analog Voltage (mV):");
    Serial.print(tempVoltage[j]);
    Serial.print("\t Temperature (C): ");
    Serial.print(tempCelsius[j]);
    Serial.print("\t Temperature (F): ");
    Serial.print(tempF[j]);  
    Serial.print("\n\r");

    // relay logic
    if (tempCelsius[j] > MAX_TEMP) {
      
      // disconnect
      for (int y = 0; y < 6; y++) {
        digitalWrite(relay[y], LOW);
      }
      
      // notify user
      Serial.print( "\nMPPTs" );
      Serial.print(" disconnected.\n\n" );
      bitSet(warning_temp,0);    // LSB set to 1
        
    } else { 
      bitClear(warning_temp, 0);  // LSB set to 0
    }
  }

// handling data to be sent through CAN bus shield

  // assigning frame data array with measured values
  for (int z = 0; z < 5; z++) {
    frame_data_temperature1[z] = tempCelsius[z];      //0-4
    frame_data_temperature2[z+5] = tempCelsius[z+5];  //5-9
    }
     
  for (int c = 0; c < 6; c++) {
    frame_data_current[c] = currentOut[c];  
  }
  
  CAN.sendMsgBuf(CAN_ID_MPPT_CURRENT, 0, data_length_current, frame_data_current);  
  CAN.sendMsgBuf(CAN_ID_MPPT_TEMP1, 0, data_length_temp, frame_data_temperature1);
  CAN.sendMsgBuf(CAN_ID_MPPT_TEMP2, 0, data_length_temp, frame_data_temperature2);
  
  if (bitRead(warning_current,0) == 1) {
    byte* warning_current_pointer = &warning_current;
    CAN.sendMsgBuf(CAN_ID_MPPT_CURRENT_WARNING, 0 , 1 , warning_current_pointer); 
  }
  
  if (bitRead(warning_temp,0) == 1) {
    byte* warning_temp_pointer = &warning_temp;
  CAN.sendMsgBuf(CAN_ID_MPPT_TEMP_WARNING, 0 , 1 , warning_temp_pointer); } 
  
  delay(1000);
}

// Handling recieved messages from CAN bus shield 
void msgHandler(uint32_t frame_id, byte *frame_data, byte frame_length) {
  
    if (frame_id == CAN_ID_MPPT_CONTROL) {
      for (int i = 0; i < frame_length; i++) {
        digitalWrite(relay[i], frame_data[i] );
      }
    } else {
        Serial.print("unknown message");
      }
}



