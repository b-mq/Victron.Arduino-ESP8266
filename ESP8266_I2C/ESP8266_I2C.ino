/*
    Victron.Arduino-ESP8266
    A:Pim Rutgers
    E:pim@physee.eu

    Code to grab data from the VE.Direct-Protocol on Arduino / ESP8266.
    Tested on NodeMCU v1.0

    The fields of the serial commands are configured in "config.h"
*/

#include "config.h"
#include <SoftwareSerial.h>
#include <LiquidCrystal_I2C.h>

// -- RX & TX Pins --
const int rxPin = D7;  // RX D7 == GPIO 13
const int txPin = D8;  // TX D8 == GPIO 15 Not used

// -- Liquid Crystal LCD via I2C --
// set the LCD number of columns and rows
const int lcdColumns = 16;                            // number of columns/characters per line in lcd
const int lcdRows = 2;                                // number of rows of lcd
const int maxChars = 17;                              // 16 chars for LCD + 1 null terminator '\0'
const int screens = 4;                                // 4 different lcd screens
const int timeBetweenScreens = 4000;                  // time in ms between switch of screens
const int lcdTimeout = timeBetweenScreens * screens;  // time until background will be switched off in ms
const int modeSwitchTimeout = 1750;                   // timeout for display mode after switch
int modeSwitchStartTime = 0;                          // time when mode switched
bool showModeSwitch = false;                          // show mode Switch until timeout
int screenCounter = 0;                                // the current screen to print on LCD
bool isLcdOn = false;                                 // BG light state of LCD

// buffer for LCD print for 1st and 2nd row
char lcd_row_1[maxChars];  // buffer for 1st row of lcd to format output
char lcd_row_2[maxChars];  // buffer for 2nd row of lcd to format output

// -- MODE --
bool isManualMode = false;  // If switch of screens is manual via button press

// -- Button --
const int buttonPin = D4;          // GPIO 2
const int debounceTime = 50;       // 50ms debounce time
const int longPressTime = 2000;    // Time until 'long press' will be detected
int buttonState = HIGH;            // The current reading from the input pin
bool isButtonPressed = false;      // Whether button is pressed or not
bool isButtonLongPressed = false;  // Whether button is long pressed;

// set LCD address, number of columns and rows
// if you don't know your display address, run an I2C scanner sketch
LiquidCrystal_I2C lcd(0x27, lcdColumns, lcdRows);

// -- Victron Energy MPPT --
SoftwareSerial victronSerial(rxPin, txPin);             // RX, TX Using Software Serial so we can use the hardware serial to check the ouput
                                                        // via the USB serial provided by the NodeMCU.
char receivedChars[buffsize];                           // an array to store the received data
char tempChars[buffsize];                               // an array to manipulate the received data
char recv_label[num_keywords][label_bytes] = { 0 };     // {0} tells the compiler to initalize it with 0.
char recv_value[num_keywords][value_bytes] = { 0 };     // That does not mean it is filled with 0's
char victronValues[num_keywords][value_bytes] = { 0 };  // The array that holds the verified data
static byte blockindex = 0;
bool new_data = false;
bool blockend = false;


void setup() {
  // Open serial communications and wait for port to open:
  Serial.begin(19200);
  victronSerial.begin(19200);
  // Liquid Crystal - initialize LCD
  lcd.init();
  SetLcdBg(true);
  lcd.clear();
  // Button
  // Configure the ESP8266 pin as a pull-up input: HIGH when the button is open, LOW when pressed.
  pinMode(buttonPin, INPUT_PULLUP);
}

void loop() {
  // Receive information on Serial from MPPT
  RecvWithEndMarker();
  HandleNewData();

  // Just print the values every second,
  // Add your own code here to use the data.
  // Make sure to not used delay(X)s of bigger than 50ms,
  // so make use of the same principle used in PrintEverySecond()
  // or use some sort of Alarm/Timer Library
  PrintEverySecond();

  // BUTTON
  HandleButton();

  // LCD
  PrintScreens();
  CheckLcdTimeout();
}

// Serial Handling
// ---
// This block handles the serial reception of the data in a
// non blocking way. It checks the Serial line for characters and
// parses them in fields. If a block of data is send, which always ends
// with "Checksum" field, the whole block is checked and if deemed correct
// copied to the 'value' array.

void RecvWithEndMarker() {
  static byte ndx = 0;
  char endMarker = '\n';
  char rc;

  while (victronSerial.available() > 0 && new_data == false) {
    rc = victronSerial.read();
    if (rc != endMarker) {
      receivedChars[ndx] = rc;
      ndx++;
      if (ndx >= buffsize) {
        ndx = buffsize - 1;
      }
    } else {
      receivedChars[ndx] = '\0';  // terminate the string
      ndx = 0;
      new_data = true;
    }
    yield();
  }
}

void HandleNewData() {
  // We have gotten a field of data
  if (new_data == true) {
    //Copy it to the temp array because parseData will alter it.
    strcpy(tempChars, receivedChars);
    ParseData();
    new_data = false;
  }
}

void ParseData() {
  char *strtokIndx;                      // this is used by strtok() as an index
  strtokIndx = strtok(tempChars, "\t");  // get the first part - the label
  // The last field of a block is always the Checksum
  if (strcmp(strtokIndx, "Checksum") == 0) {
    blockend = true;
  }
  strcpy(recv_label[blockindex], strtokIndx);  // copy it to label

  // Now get the value
  strtokIndx = strtok(NULL, "\r");  // This continues where the previous call left off until '/r'.
  if (strtokIndx != NULL) {         // We need to check here if we don't receive NULL.
    strcpy(recv_value[blockindex], strtokIndx);
  }
  blockindex++;

  if (blockend) {
    // We got a whole block into the received data.
    // Check if the data received is not corrupted.
    // Sum off all received bytes should be 0;
    byte checksum = 0;
    for (int x = 0; x < blockindex; x++) {
      // Loop over the labels and value gotten and add them.
      // Using a byte so the the % 256 is integrated.
      char *v = recv_value[x];
      char *l = recv_label[x];
      while (*v) {
        checksum += *v;
        v++;
      }
      while (*l) {
        checksum += *l;
        l++;
      }
      // Because we strip the new line(10), the carriage return(13) and
      // the horizontal tab(9) we add them here again.
      checksum += 32;
    }
    // Checksum should be 0, so if !0 we have correct data.
    if (!checksum) {
      // Since we are getting blocks that are part of a
      // keyword chain, but are not certain where it starts
      // we look for the corresponding label. This loop has a trick
      // that will start searching for the next label at the start of the last
      // hit, which should optimize it.
      int start = 0;
      for (int i = 0; i < blockindex; i++) {
        for (int j = start; (j - start) < num_keywords; j++) {
          if (strcmp(recv_label[i], keywords[j % num_keywords]) == 0) {
            // found the label, copy it to the value array
            strcpy(victronValues[j], recv_value[i]);
            start = (j + 1) % num_keywords;  // start searching the next one at this hit +1
            break;
          }
        }
      }
    }
    // Reset the block index, and make sure we clear blockend.
    blockindex = 0;
    b - mq

        blockend = false;
  }
}

void PrintEverySecond() {
  static unsigned long prev_millis;
  if (millis() - prev_millis > 1000) {
    PrintValues();
    prev_millis = millis();
  }
}

void PrintValues() {
  for (int i = 0; i < num_keywords; i++) {
    Serial.print(keywords[i]);
    Serial.print(",");
    Serial.println(victronValues[i]);
  }
}

float GetFloatValue(int index, float mult = 1.0) {
  float val = atof(victronValues[index]) * mult;
  return val;
}

int GetIntValue(int index, int mult = 1) {
  int val = atoi(victronValues[index]) * mult;
  return val;
}

const char *GetStateOfOperation() {
  int val = GetIntValue(CS);
  switch (val) {
    case 0:
      return "Off";  // "Off"
    case 1:
      return "LoPWR";  // "Low power"
    case 2:
      return "Fault";  // "Fault"
    case 3:
      return "Bulk";  // "Bulk"
    case 4:
      return "Absor";  // "Absorption"
    case 5:
      return "Float";  // "Float"
    case 6:
      return "Storg";  // "Storage"
    case 7:
      return "Equal";  // "Equalize (manual)"
    case 9:
      return "Invrt";  // "Inverting"
    case 11:
      return "PwSup";  // "Power supply"
    case 245:
      return "Start";  // "Starting-up"
    case 246: b - mq absorption "
                case 247 : return "EqRec";  // "Auto equalize / Recondition"
    case 248:
      return "BtSaf";  // "BatterySafe"
    default:
      return "UNKNOWN";  // Unknown
  }
}

bool HandleButton() {
  static unsigned long bounceTime;
  static unsigned long longPressPrevTime;
  static int boundState;
  static int lastButtonState;

  // get button state (with noise)
  buttonState = digitalRead(buttonPin);

  // check button pressed/released state with noise filtering via
  // monitoring consistant button state over given period
  if (buttonState != boundState) {
    bounceTime = millis();
    longPressPrevTime = millis();
    boundState = buttonState;
  }

  // de-bounce button
  if ((millis() - bounceTime) > debounceTime) {
    // -- PRESSED --
    if (HIGH == lastButtonState && LOW == buttonState) {
      isButtonPressed = true;
      Serial.println("The button is pressed");
      if (isLcdOn && isManualMode) {
        MoveCounterToNextScreen();
      }
    }  // -- RELEASED --
    else if (LOW == lastButtonState && HIGH == buttonState) {
      isButtonPressed = false;
      b - mq

            Serial.println("The button is released");
    }
    // -- LONG PRESS --
    if (!isButtonLongPressed
        && LOW == buttonState
        && (millis() - longPressPrevTime) > longPressTime) {
      isButtonLongPressed = true;
      Serial.println("The button is LONG pressed");
      SwitchMode();
    }

    lastButtonState = buttonState;
  }
  return isButtonPressed;
}

// Print 4 different 'screens' on LCD
// switching screens after given amount or by button press in manual mode
void PrintScreens() {
  static unsigned long prev_millis;
  static int lastScreenCounter;

  // timeout for mode switch
  if (showModeSwitch && millis() - modeSwitchStartTime > modeSwitchTimeout) {
    showModeSwitch = false;
    SwitchScreen(screenCounter);  // need when switched to MANUAL mode so it won't stuck
  }

  if (showModeSwitch) {
    prev_millis = millis();  // mode switch screen should not be overwritten
    return;
  }

  // switched screens via button in manual mode or via timeout in automatic mode
  else if (isManualMode && screenCounter != lastScreenCounter) {
    // screen increased by button press
    SwitchScreen(screenCounter);
  } else if (!isManualMode && (millis() - prev_millis) > timeBetweenScreens) {
    MoveCounterToNextScreen();
    prev_millis = millis();
    SwitchScreen(screenCounter);
  }
  lastScreenCounter = screenCounter;
}

void SwitchScreen(int screen) {
  switch (screen) {
    case 0:
      Serial.printf("Switched to screen: %d - Voltage, Ampere & State\n", screen);
      ConfigureScreen1();
      break;
    case 1:
      Serial.printf("Switched to screen: %d - VPV, IPV & PPV\n", screen);
      ConfigureScreen2();
      break;
    case 2:
      Serial.printf("Switched to screen: %d - PTODAY & PMAX\n", screen);
      ConfigureScreen3();
      break;
    case 3:
      Serial.printf("Switched to screen: %d - TOTAL POWER\n", screen);
      ConfigureScreen4();
      break;
    default:
      Serial.printf("Switched to screen: %d - Voltage, Ampere & State\n", screen);
      ConfigureScreen1();
  }

  // write to lcd
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print(lcd_row_1);
  lcd.setCursor(0, 1);
  lcd.print(lcd_row_2);
}

void SetLcdBg(bool on) {
  if (on) {
    lcd.backlight();
  } else {
    lcd.noBacklight();
  }
  isLcdOn = on;
}

void CheckLcdTimeout() {
  static unsigned long prev_millis;

  if (isButtonPressed) {
    prev_millis = millis();
    if (!isLcdOn) {
      SetLcdBg(true);
    }
  }

  if (isLcdOn && (millis() - prev_millis) > lcdTimeout) {
    prev_millis = millis();
    Serial.println("LCD Timeout");
    SetLcdBg(false);
  }
}

void MoveCounterToNextScreen() {
  screenCounter = (screenCounter + 1) % screens;
}

void SwitchMode() {
  isManualMode = !isManualMode;
  showModeSwitch = true;
  modeSwitchStartTime = millis();

  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Switched Mode:");
  lcd.setCursor(0, 1);
  lcd.print(isManualMode ? "M A N U A L" : "AUTOMATIC");
  Serial.printf("Switched mode: %s\n", isManualMode ? "Manual" : "Automatic");
}

// -- Voltage, Ampere & State --
void ConfigureScreen1() {
  // VBAT 12.8V STATE
  // IVAT 16.5A FLOAT

  // get values & convert
  const float volt = GetFloatValue(V, 0.001);    // mV to V
  const float ampere = GetFloatValue(I, 0.001);  // mA to A
  const char *state = GetStateOfOperation();


  // format output string for lcd
  snprintf(lcd_row_1, maxChars, "VBAT %4.1fV STATE", volt);
  snprintf(lcd_row_2, maxChars, "IBAT %4.1fA %s", ampere, state);
}

// -- VPV, IPV & PPV --
void ConfigureScreen2() {
  // VPV  29V IPV 17A
  // PPV  72W

  // get values & convert
  float voltPV = GetFloatValue(VPV, 0.001);            // mV to V
  float powerPV = GetFloatValue(PPV);                  // W
  float amperePV = voltPV > 0 ? powerPV / voltPV : 0;  // W / I

  // format output string for lcd
  snprintf(lcd_row_1, maxChars, "VPV %3.0fV IPV %2.0fA", voltPV, amperePV);
  snprintf(lcd_row_2, maxChars, "PPV %3.0fW ", powerPV);
}

// -- PTODAY & PMAX --
void ConfigureScreen3() {
  // PTODAY 100WH20
  // PMAX   80W

  int powerToday = GetIntValue(H20, 10);  // in 0,01kWh -> Wh
  int powerMax = GetIntValue(H21);        // W

  // format output string for lcd
  snprintf(lcd_row_1, maxChars, "PTODAY %3dW", powerToday);
  snprintf(lcd_row_2, maxChars, "PMAX   %3dW ", powerMax);
}

// -- TOTAL POWER --
void ConfigureScreen4() {
  // TOTAL POWER
  //    10000.00KWh

  float powerTotal = GetFloatValue(H19, 0.01);  // in 0,01kWh -> kWh

  // format output string for lcd
  snprintf(lcd_row_1, maxChars, "TOTAL POWER");
  snprintf(lcd_row_2, maxChars, "%13.2fKWh ", powerTotal);
}
