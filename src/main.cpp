// ============================================
// SODASTREAM TRACKER - COMPLETE VERSION
// Wemos D1 mini + SH1106 + MQTT + OTA
// ============================================

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <U8g2lib.h>
#include <EEPROM.h>
#include <time.h>
#include <math.h>
#include <credentials.h>

// ---------------- PIN ----------------
#define BTN_PIN 0   // D3 (GPIO0)
#define WAKE_TIME 500
#define HOLD_TIME 3000
#define RESTART_TIME 10000
#define CONFIRM_TIMEOUT 2000
#define DISPLAY_TIMEOUT 120000

// --------------- WLAN ---------------
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;
const char* mqtt_server = MQTT_SERVER;

// --------------- DISPLAY ------------
U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R2);

// ---------- FORWARD DECLARATIONS ----------
void handleReset();
void handleFactoryReset();
void saveState();

// --------------- MQTT ---------------
WiFiClient espClient;
PubSubClient client(espClient);

// --------------- EEPROM -------------
#define EEPROM_MAGIC 0xDEADBEEF

// --------------- STATE --------------
int counter = 0;
int total = 0;
int max_co2_config = 80;
int max_co2_avg = 80;
int cylinderCount = 0;
int totalAllCylinders = 0;

int cylinderStartDay = 0;

int todayCount = 0;
int lastDayNumber = -1;

bool warningActive = false;

unsigned long pressStart = 0;
bool buttonPressed = false;

unsigned long lastLineSwitch = 0;
bool showStatsLine = false;

bool showBigStats = false;
unsigned long statsStartTime = 0;

bool confirmResetActive = false;
unsigned long confirmResetStart = 0;

bool displayOff = false;
unsigned long lastActivity = 0;
bool pressStartedWithDisplayOff = false;

struct UsageStats {
  int avgPerDay;
  int remainingUses;
  int remainingDays;
};

// ============================================
// TIME
// ============================================

int getDayNumber() {
  time_t now = time(nullptr);
  return now / 86400;
}

int getDaysSinceStart() {
  int currentDay = getDayNumber();
  if(currentDay > 0 && cylinderStartDay == 0) {
    cylinderStartDay = currentDay;
    saveState();
  }
  return max(1, currentDay - cylinderStartDay);
}

UsageStats calculateUsageStats() {
  int days = max(1, getDaysSinceStart());
  float avgPerDayFloat = (float)total / (float)days;
  int avgPerDay = max(1, (int)roundf(avgPerDayFloat));

  // Gewuenschte Logik: Rest = Start - (Ø/Tag * Tage)
  int usedByAverage = avgPerDay * days;
  int remainingUses = max(0, max_co2_avg - usedByAverage);
  int remainingDays = (int)roundf((float)remainingUses / (float)avgPerDay);

  UsageStats stats;
  stats.avgPerDay = avgPerDay;
  stats.remainingUses = remainingUses;
  stats.remainingDays = remainingDays;
  return stats;
}

// ============================================
// EEPROM
// ============================================

void saveState() {
  uint32_t magic = EEPROM_MAGIC;

  EEPROM.put(0, magic);
  EEPROM.put(4, counter);
  EEPROM.put(8, total);
  EEPROM.put(12, max_co2_config);
  EEPROM.put(16, max_co2_avg);
  EEPROM.put(20, cylinderCount);
  EEPROM.put(24, totalAllCylinders);
  EEPROM.put(28, cylinderStartDay);
  EEPROM.put(32, todayCount);
  EEPROM.put(36, lastDayNumber);
  EEPROM.commit();
}

void loadState() {

  uint32_t magic;
  EEPROM.get(0, magic);

  if (magic != EEPROM_MAGIC) {
    counter = 0;
    total = 0;
    max_co2_config = 80;
    max_co2_avg = 80;
    cylinderCount = 0;
    totalAllCylinders = 0;
    cylinderStartDay = getDayNumber();
    saveState();
    return;
  }

  EEPROM.get(4, counter);
  EEPROM.get(8, total);
  EEPROM.get(12, max_co2_config);
  EEPROM.get(16, max_co2_avg);
  EEPROM.get(20, cylinderCount);
  EEPROM.get(24, totalAllCylinders);
  EEPROM.get(28, cylinderStartDay);
  EEPROM.get(32, todayCount);
  EEPROM.get(36, lastDayNumber);
}

// ============================================
// MQTT
// ============================================

void connectMQTT(){
  while(!client.connected()){
    client.connect("sodastream_tracker");
    client.subscribe("home/sodastream/set/reset");
    client.subscribe("home/sodastream/set/max_co2");
  }
}

void sendState(int remainingUses, int remainingDays, int avgPerDay){
  StaticJsonDocument<256> doc;
  doc["value"] = counter;
  doc["remaining_uses"] = remainingUses;
  doc["remaining_days"] = remainingDays;
  doc["max_co2"] = max_co2_avg;
  doc["avg_per_day"] = avgPerDay;
  doc["today"] = todayCount;

  char buffer[256];
  serializeJson(doc, buffer);
  client.publish("home/sodastream/state", buffer, true);
}

void callback(char* topic, byte* payload, unsigned int length){
  String msg;
  for(unsigned int i=0;i<length;i++) msg+=(char)payload[i];
  lastActivity = millis();

  if(String(topic)=="home/sodastream/set/reset"){
    if(msg == "ALL") handleFactoryReset();
    else handleReset();
  }

  if(String(topic)=="home/sodastream/set/max_co2"){
    max_co2_config = msg.toInt();
    max_co2_avg = max_co2_config;
    saveState();
  }
}

// ============================================
// DISPLAY
// ============================================

void bubbleAnimation() {
  for(int frame=0; frame<15; frame++) {
    u8g2.clearBuffer();
    for(int i=0;i<6;i++){
      int x=random(20,108);
      int y=60-(frame*4)-random(0,10);
      if(y>0) u8g2.drawCircle(x,y,2);
    }
    u8g2.sendBuffer();
    delay(60);
  }
}

void drawDisplay(int remainingUses, int remainingDays, int avgPerDay){
  u8g2.clearBuffer();
  unsigned long held = buttonPressed ? millis() - pressStart : 0;

  // --- Bestätigungs-Fenster: "Push to Reset" + Countdown-Bar ---
  if(confirmResetActive){
    float progress = 1.0f - (float)(millis() - confirmResetStart) / (float)CONFIRM_TIMEOUT;
    if(progress < 0) progress = 0;
    int barHeight = (int)(64.0f * progress);
    u8g2.drawBox(124, 64 - barHeight, 4, barHeight);

    u8g2.setFont(u8g2_font_6x12_tf);
    int w = u8g2.getStrWidth("Push to");
    u8g2.drawStr((128-w)/2, 24, "Push to");
    u8g2.setFont(u8g2_font_logisoso24_tf);
    w = u8g2.getStrWidth("Reset");
    u8g2.drawStr((128-w)/2, 56, "Reset");
    u8g2.sendBuffer();
    return;
  }

  // --- Taste >= 10s gehalten: "Push to Reset" ---
  if(buttonPressed && held >= (unsigned long)RESTART_TIME){
    u8g2.setFont(u8g2_font_6x12_tf);
    int w = u8g2.getStrWidth("Push to");
    u8g2.drawStr((128-w)/2, 24, "Push to");
    u8g2.setFont(u8g2_font_logisoso24_tf);
    w = u8g2.getStrWidth("Reset");
    u8g2.drawStr((128-w)/2, 56, "Reset");
    u8g2.sendBuffer();
    return;
  }

  // --- Taste 3s–10s gehalten: "CO₂ ausgetauscht?" ---
  if(buttonPressed && held >= (unsigned long)HOLD_TIME){
    // "CO" groß, "2" tiefgestellt
    u8g2.setFont(u8g2_font_logisoso24_tf);
    int coWidth = u8g2.getStrWidth("CO");
    u8g2.setFont(u8g2_font_6x12_tf);
    int twoWidth = u8g2.getStrWidth("2");
    int startX = (128 - coWidth - twoWidth) / 2;

    u8g2.setFont(u8g2_font_logisoso24_tf);
    u8g2.drawStr(startX, 28, "CO");
    u8g2.setFont(u8g2_font_6x12_tf);
    u8g2.drawStr(startX + coWidth, 32, "2");

    int w = u8g2.getStrWidth("ausgetauscht?");
    u8g2.drawStr((128-w)/2, 46, "ausgetauscht?");
    u8g2.sendBuffer();
    return;
  }

  // --- Nach Tastendruck: Tageszähler ---
  if(showBigStats){
    u8g2.setFont(u8g2_font_logisoso32_tf);
    String todayStr = String(todayCount);
    int w = u8g2.getStrWidth(todayStr.c_str());
    u8g2.setCursor((128-w)/2, 40);
    u8g2.print(todayStr);
    u8g2.setFont(u8g2_font_6x12_tf);
    w = u8g2.getStrWidth("heute");
    u8g2.setCursor((128-w)/2, 56);
    u8g2.print("heute");
    u8g2.sendBuffer();
    return;
  }

  // --- Normalansicht ---
  u8g2.setFont(u8g2_font_logisoso28_tf);
  String val = String(counter);
  int w = u8g2.getStrWidth(val.c_str());
  u8g2.setCursor((128-w)/2, 30);
  u8g2.print(val);

  float ratio = (float)total / max_co2_avg;
  if(ratio > 1) ratio = 1;
  int barWidth = 88 * (1 - ratio);
  u8g2.drawHLine(20, 38, 88);
  u8g2.drawBox(20, 42, barWidth, 8);

  u8g2.setFont(u8g2_font_6x12_tf);
  String bottom;
  if(showStatsLine)
    bottom = "Ø " + String(avgPerDay) + " / Tag · " + String(todayCount) + " heute";
  else
    bottom = String(remainingUses) + " Fl. · " + String(remainingDays) + " Tage";

  w = u8g2.getStrWidth(bottom.c_str());
  int x = (128-w)/2;

  if(warningActive){
    u8g2.setDrawColor(0);
    u8g2.drawBox(0, 52, 128, 12);
    u8g2.setDrawColor(1);
  }

  u8g2.setCursor(x, 62);
  u8g2.print(bottom);
  u8g2.setDrawColor(1);
  u8g2.sendBuffer();
}

// ============================================
// LOGIC
// ============================================

void handleFactoryReset(){
  counter = 0;
  total = 0;
  max_co2_config = 80;
  max_co2_avg = 80;
  cylinderCount = 0;
  totalAllCylinders = 0;
  cylinderStartDay = getDayNumber();
  todayCount = 0;
  lastDayNumber = getDayNumber();
  warningActive = false;
  saveState();
}

void handleReset(){
  totalAllCylinders += total;
  cylinderCount++;

  if(cylinderCount > 0)
    max_co2_avg = totalAllCylinders / cylinderCount;

  counter = 0;
  total = 0;
  todayCount = 0;
  warningActive = false;
  cylinderStartDay = getDayNumber();

  saveState();
}

void handleIncrement(){
  bubbleAnimation();

  int currentDay = getDayNumber();
  if(currentDay != lastDayNumber){
    todayCount = 0;
    lastDayNumber = currentDay;
  }

  counter++;
  total++;
  todayCount++;

  UsageStats stats = calculateUsageStats();

  if(stats.remainingUses < 10)
    warningActive = true;

  showBigStats = true;
  statsStartTime = millis();

  sendState(stats.remainingUses, stats.remainingDays, stats.avgPerDay);
  saveState();
}

// ============================================
// BUTTON
// ============================================

void handleButton(){
  bool state = digitalRead(BTN_PIN);

  // Neue Betätigung
  if(state == LOW && !buttonPressed){
    pressStartedWithDisplayOff = displayOff;
    if(displayOff){
      displayOff = false;
      u8g2.setPowerSave(0);
    }
    if(confirmResetActive){
      confirmResetActive = false;
      handleFactoryReset();
      ESP.restart();
      return;
    }
    buttonPressed = true;
    pressStart = millis();
    lastActivity = millis();
  }

  // Loslassen
  if(state == HIGH && buttonPressed){
    unsigned long held = millis() - pressStart;
    buttonPressed = false;
    lastActivity = millis();

    if(pressStartedWithDisplayOff && held < (unsigned long)WAKE_TIME){
      return; // Display wurde nur geweckt, keine weitere Aktion
    }

    if(held >= (unsigned long)RESTART_TIME){
      confirmResetActive = true;
      confirmResetStart = millis();
    } else if(held >= (unsigned long)HOLD_TIME){
      handleReset();
    } else {
      handleIncrement();
    }
  }

  // Bestätigungs-Timeout
  if(confirmResetActive && millis() - confirmResetStart >= (unsigned long)CONFIRM_TIMEOUT){
    confirmResetActive = false;
  }
}

// ============================================
// SETUP / LOOP
// ============================================

void setup(){
  pinMode(BTN_PIN,INPUT_PULLUP);

  EEPROM.begin(128);
  loadState();

  u8g2.begin();
  u8g2.enableUTF8Print();

  WiFi.setSleepMode(WIFI_MODEM_SLEEP);
  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED) delay(100);

  configTime(0,0,"pool.ntp.org");

  client.setServer(mqtt_server,1883);
  client.setCallback(callback);

  ArduinoOTA.setHostname("sodastream");
  ArduinoOTA.begin();
}

void loop(){

  ArduinoOTA.handle();

  if(!client.connected()) connectMQTT();
  client.loop();

  handleButton();

  if(showBigStats && millis()-statsStartTime>3000)
    showBigStats=false;

  if(millis()-lastLineSwitch>10000){
    showStatsLine=!showStatsLine;
    lastLineSwitch=millis();
  }

  if(!displayOff && millis() - lastActivity > (unsigned long)DISPLAY_TIMEOUT){
    u8g2.setPowerSave(1);
    displayOff = true;
  }

  if(!displayOff){
    UsageStats stats = calculateUsageStats();
    drawDisplay(stats.remainingUses, stats.remainingDays, stats.avgPerDay);
  }
}
