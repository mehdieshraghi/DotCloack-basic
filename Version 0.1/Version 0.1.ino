#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <ESP8266WebServer.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Max72xxPanel.h>
#include <time.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <ESP8266HTTPUpdateServer.h>

#define VERSION "0.0.2"
#define DEVICE_NAME "SmartClock"
#define CONFIG_FILE "/clock_config.json"

// MQTT Configuration
String mqtt_server = "your-mqtt-server";  // Default MQTT server
int mqtt_port = 1883;
String mqtt_user = "your-mqtt-username";
String mqtt_password = "your-mqtt-password";
#define MQTT_TOPIC_TIME "clock/time"

// Timezone Configuration
int timezone_offset = 0; // Default GMT+0
String timezone_name = "GMT"; // Default timezone name

// LED Matrix Configuration
int pinCS = D6;
int numberOfHorizontalDisplays = 4;
int numberOfVerticalDisplays = 1;

Max72xxPanel matrix = Max72xxPanel(pinCS, numberOfHorizontalDisplays, numberOfVerticalDisplays);
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer httpUpdater;
WiFiClient espClient;
PubSubClient mqttClient(espClient);

int wait = 70; // In milliseconds
int spacer = 1;
int width = 5 + spacer; // The font width is 5 pixels

int brightness = 5; // Default brightness
bool useMQTT = false;

// 3x5 custom digits (0-9)
const byte MINI_DIGITS[10][5] = {
  { B111, B101, B101, B101, B111 },  // 0
  { B010, B110, B010, B010, B111 },  // 1
  { B111, B001, B111, B100, B111 },  // 2
  { B111, B001, B111, B001, B111 },  // 3
  { B101, B101, B111, B001, B001 },  // 4
  { B111, B100, B111, B001, B111 },  // 5
  { B111, B100, B111, B101, B111 },  // 6
  { B111, B001, B001, B001, B001 },  // 7
  { B111, B101, B111, B101, B111 },  // 8
  { B111, B101, B111, B001, B111 }   // 9
};

// Save configuration to LittleFS
void saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["use_mqtt"] = useMQTT;
  doc["mqtt_server"] = mqtt_server;
  doc["mqtt_port"] = mqtt_port;
  doc["mqtt_user"] = mqtt_user;
  doc["mqtt_password"] = mqtt_password;
  doc["brightness"] = brightness;
  doc["timezone_offset"] = timezone_offset;
  doc["timezone_name"] = timezone_name;
  
  File configFile = LittleFS.open(CONFIG_FILE, "w");
  if (!configFile) {
    Serial.println("Error opening config file for writing!");
    return;
  }
  
  serializeJson(doc, configFile);
  configFile.close();
  Serial.println("Settings saved");
}

// Load configuration from LittleFS
void loadConfig() {
  if (LittleFS.exists(CONFIG_FILE)) {
    File configFile = LittleFS.open(CONFIG_FILE, "r");
    
    if (configFile) {
      size_t size = configFile.size();
      std::unique_ptr<char[]> buf(new char[size]);
      configFile.readBytes(buf.get(), size);
      
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, buf.get());
      
      if (!error) {
        useMQTT = doc["use_mqtt"];
        mqtt_server = doc["mqtt_server"].as<String>();
        mqtt_port = doc["mqtt_port"];
        mqtt_user = doc["mqtt_user"].as<String>();
        mqtt_password = doc["mqtt_password"].as<String>();
        brightness = doc["brightness"];
        timezone_offset = doc["timezone_offset"];
        timezone_name = doc["timezone_name"].as<String>();
        
        // Apply loaded settings
        matrix.setIntensity(brightness);
        
        Serial.println("Settings loaded");
      } else {
        Serial.println("Error parsing JSON file");
      }
      configFile.close();
    }
  }
}

// Apply timezone settings
void applyTimezoneSettings() {
  // Format timezone string based on offset
  char tz[50];
  sprintf(tz, "%s%d", (timezone_offset >= 0) ? "GMT+" : "GMT", timezone_offset);
  setenv("TZ", tz, 1);
  tzset(); // Apply timezone change
  Serial.print("Timezone set to: ");
  Serial.println(tz);
}

// Generate timezone selection HTML
String generateTimezoneOptions() {
  String options = "";
  for (int i = -12; i <= 14; i++) {
    String label = "GMT";
    if (i > 0) label += "+";
    if (i != 0) label += String(i);
    
    options += "<option value='" + String(i) + "' " + 
               (i == timezone_offset ? "selected" : "") + ">" + 
               label + "</option>";
  }
  return options;
}

// Setup Web Server
void setupWebServer() {
  httpUpdater.setup(&server, "/update");
  
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head>";
    html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
    html += "<style>";
    html += "body{font-family:Arial,sans-serif;margin:0;padding:20px;text-align:center;background:#f5f5f5;}";
    html += ".container{max-width:600px;margin:0 auto;background:#fff;padding:20px;border-radius:10px;box-shadow:0 2px 10px rgba(0,0,0,0.1);}";
    html += "h1{color:#333;margin-bottom:30px;}";
    html += ".panel{margin:15px 0;padding:15px;background:#f9f9f9;border-radius:5px;text-align:left;}";
    html += ".panel h2{font-size:18px;margin-top:0;color:#444;}";
    html += ".form-group{margin-bottom:15px;}";
    html += "label{display:block;margin-bottom:5px;font-weight:bold;}";
    html += "input[type=text],input[type=number],input[type=password],select{width:100%;padding:8px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;}";
    html += "input[type=range]{width:100%;}";
    html += "input[type=checkbox]{margin-right:8px;}";
    html += ".checkbox-label{display:inline-block;font-weight:normal;}";
    html += ".btn{background:#4CAF50;color:white;border:none;padding:10px 15px;border-radius:5px;cursor:pointer;font-size:16px;}";
    html += ".btn:hover{background:#45a049;}";
    html += ".btn-update{background:#2196F3;}";
    html += ".btn-update:hover{background:#0b7dda;}";
    html += ".footer{font-size:14px;margin-top:30px;color:#666;}";
    html += ".version{font-size:12px;color:#999;margin-top:10px;}";
    html += "</style>";
    html += "<title>Smart Clock Settings</title></head><body>";
    html += "<div class='container'>";
    html += "<h1>Smart Clock Settings</h1>";
    
    html += "<form action='/save' method='post'>";
    
    // Display Settings Panel
    html += "<div class='panel'>";
    html += "<h2>Display Settings</h2>";
    html += "<div class='form-group'>";
    html += "<label for='brightness'>Brightness:</label>";
    html += "<input type='range' id='brightness' name='brightness' min='0' max='12' value='" + String(brightness) + "'>";
    html += "<div style='display:flex;justify-content:space-between;'><span>Low</span><span>" + String(brightness) + "</span><span>High</span></div>";
    html += "</div>";
    html += "</div>";
    
    // Time Settings Panel
    html += "<div class='panel'>";
    html += "<h2>Time Settings</h2>";
    html += "<div class='form-group'>";
    html += "<label for='timezone'>Timezone:</label>";
    html += "<select id='timezone' name='timezone'>";
    html += generateTimezoneOptions();
    html += "</select>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='timezone_name'>Timezone Label:</label>";
    html += "<input type='text' id='timezone_name' name='timezone_name' value='" + timezone_name + "' placeholder='e.g. GMT, EST, CET'>";
    html += "</div>";
    html += "</div>";
    
    // MQTT Settings Panel
    html += "<div class='panel'>";
    html += "<h2>MQTT Settings</h2>";
    html += "<div class='form-group'>";
    html += "<input type='checkbox' id='use_mqtt' name='use_mqtt' " + String(useMQTT ? "checked" : "") + ">";
    html += "<label for='use_mqtt' class='checkbox-label'>Enable MQTT</label>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='mqtt_server'>Server Address:</label>";
    html += "<input type='text' id='mqtt_server' name='mqtt_server' value='" + mqtt_server + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='mqtt_port'>Port:</label>";
    html += "<input type='number' id='mqtt_port' name='mqtt_port' value='" + String(mqtt_port) + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='mqtt_user'>Username:</label>";
    html += "<input type='text' id='mqtt_user' name='mqtt_user' value='" + mqtt_user + "'>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<label for='mqtt_password'>Password:</label>";
    html += "<input type='password' id='mqtt_password' name='mqtt_password' value='" + mqtt_password + "'>";
    html += "</div>";
    html += "</div>";
    
    // Submit Button
    html += "<div style='text-align:center;margin-top:20px;'>";
    html += "<input type='submit' value='Save Settings' class='btn'>";
    html += "</div>";
    html += "</form>";
    
    // Firmware Update Panel
    html += "<div class='panel' style='margin-top:30px;'>";
    html += "<h2>Firmware Update</h2>";
    html += "<p>Current version: " + String(VERSION) + "</p>";
    html += "<div style='text-align:center;'>";
    html += "<a href='/update' class='btn btn-update' style='display:inline-block;text-decoration:none;'>Update Firmware</a>";
    html += "</div>";
    html += "</div>";
    
    html += "<div class='footer'>www.oneos.ir</div>";
    html += "<div class='version'>v" + String(VERSION) + "</div>";
    
    html += "</div>";
    
    html += "</body></html>";
    
    server.send(200, "text/html", html);
  });
  
  server.on("/save", HTTP_POST, []() {
    if (server.hasArg("brightness")) {
      brightness = server.arg("brightness").toInt();
      matrix.setIntensity(brightness);
    }
    
    if (server.hasArg("timezone")) {
      timezone_offset = server.arg("timezone").toInt();
    }
    
    if (server.hasArg("timezone_name")) {
      timezone_name = server.arg("timezone_name");
    }
    
    useMQTT = server.hasArg("use_mqtt");
    
    if (server.hasArg("mqtt_server")) {
      mqtt_server = server.arg("mqtt_server");
    }
    
    if (server.hasArg("mqtt_port")) {
      mqtt_port = server.arg("mqtt_port").toInt();
    }
    
    if (server.hasArg("mqtt_user")) {
      mqtt_user = server.arg("mqtt_user");
    }
    
    if (server.hasArg("mqtt_password")) {
      mqtt_password = server.arg("mqtt_password");
    }
    
    // Apply timezone settings
    applyTimezoneSettings();
    
    // Save settings to LittleFS
    saveConfig();
    
    // Reconnect to MQTT with new settings
    if (useMQTT) {
      mqttClient.disconnect();
      connectMQTT();
    }
    
    server.sendHeader("Location", "/");
    server.send(303);
  });
  
  server.begin();
  Serial.println("Web server started");
}

// Connect to MQTT
void connectMQTT() {
  if (!useMQTT) return;
  
  mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
  
  if (mqttClient.connect("ESP8266Clock", mqtt_user.c_str(), mqtt_password.c_str())) {
    Serial.println("Connected to MQTT server");
  } else {
    Serial.print("Error connecting to MQTT, state: ");
    Serial.println(mqttClient.state());
  }
}

// Publish data to MQTT
void publishTimeToMQTT(String timeStr) {
  if (!useMQTT || !mqttClient.connected()) return;
  mqttClient.publish(MQTT_TOPIC_TIME, timeStr.c_str());
}

// Draw custom 3x5 mini digit
void drawMiniDigit(int digit, int x, int y) {
  if (digit < 0 || digit > 9) return;
  
  for (int row = 0; row < 5; row++) {
    byte rowData = MINI_DIGITS[digit][row];
    for (int col = 0; col < 3; col++) {
      if (bitRead(rowData, 2 - col)) {
        matrix.drawPixel(x + col, y + row, HIGH);
      }
    }
  }
}

// Draw time with mini digits (3x5 pixels per digit)
void drawMiniTime(int hours, int minutes) {
  int x = 1; // starting position
  
  // Draw hours
  drawMiniDigit(hours / 10, x, 1); // tens place
  x += 4; // spacing between digits
  drawMiniDigit(hours % 10, x, 1); // ones place
  x += 4;
  
  // Draw colon (2 pixels)
  matrix.drawPixel(x, 2, HIGH);
  matrix.drawPixel(x, 4, HIGH);
  x += 2;
  
  // Draw minutes
  drawMiniDigit(minutes / 10, x, 1); // tens place
  x += 4;
  drawMiniDigit(minutes % 10, x, 1); // ones place
}

// Display compact message using standard font
void displayCompactText(String text, int y = 0) {
  int len = text.length();
  int charWidth = 4; // Smaller width for characters
  int x = (32 - (len * charWidth)) / 2; // Center text (32 is matrix width for 4 modules)
  
  for (int i = 0; i < len; i++) {
    matrix.drawChar(x + (i * charWidth), y, text.charAt(i), HIGH, LOW, 1);
  }
  matrix.write();
}

// Display startup information
void displayStartupInfo() {
  // Display name
  matrix.fillScreen(LOW);
  displayCompactText(DEVICE_NAME);
  delay(1500);
  
  // Display IP address
  matrix.fillScreen(LOW);
  String ip = WiFi.localIP().toString();
  displayCompactText("IP", 0);
  displayCompactText(ip, 8);
  delay(2000);
  
  // Display timezone
  matrix.fillScreen(LOW);
  displayCompactText(timezone_name);
  delay(1500);
  
  // Display website
  matrix.fillScreen(LOW);
  displayCompactText("ONEOS.IR");
  delay(1500);
}

// Display scrolling message
void scrollText(String message) {
  for (int i = 0; i < width * message.length() + matrix.width() - spacer; i++) {
    matrix.fillScreen(LOW);
    int letter = i / width;
    int x = (matrix.width() - 1) - i % width;
    int y = (matrix.height() - 8) / 2; // Center text vertically
    while (x + width - spacer >= 0 && letter >= 0) {
      if (letter < message.length()) {
        matrix.drawChar(x, y, message[letter], HIGH, LOW, 1);
      }
      letter--;
      x -= width;
    }
    matrix.write();
    delay(wait / 2);
  }
}

void setup() {
  Serial.begin(9600);

  Serial.println("www.oneos.ir");
  Serial.println("========================");
  
  // Initialize LittleFS
  if (!LittleFS.begin()) {
    Serial.println("Error initializing LittleFS");
  } else {
    Serial.println("LittleFS initialized");
    loadConfig();
  }
  
  // Initialize LED matrix
  matrix.setIntensity(brightness);
  matrix.setRotation(0, 1);
  matrix.setRotation(1, 1);
  matrix.setRotation(2, 1);
  matrix.setRotation(3, 1);
  matrix.fillScreen(LOW);
  matrix.write();

  // Display WiFi connection message on LED matrix
  displayCompactText("WIFI");
  
  // Initialize WiFiManager
  WiFiManager wifiManager;
  
  // Create access point named "ClockSetup"
  wifiManager.autoConnect("ClockSetup");
  
  Serial.println("Connected to WiFi network");
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  
  // Set time
  configTime(0 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  applyTimezoneSettings();

  // Initialize web server and update system
  setupWebServer();
  
  // Connect to MQTT
  if (useMQTT) {
    connectMQTT();
  }
  
  // Display startup information
  displayStartupInfo();
}

// Periodically display date
unsigned long lastDateDisplay = 0;
const unsigned long DATE_DISPLAY_INTERVAL = 60000; // Show date every minute

void loop() {
  server.handleClient();
  
  if (useMQTT) {
    if (!mqttClient.connected()) {
      connectMQTT();
    }
    mqttClient.loop();
  }
  
  // Get current time
  time_t now = time(nullptr);
  struct tm *timeinfo = localtime(&now);
  
  // Format time string for MQTT
  char timeStr[9];
  sprintf(timeStr, "%02d:%02d:%02d", timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  
  // Publish time to MQTT if enabled
  if (useMQTT) {
    publishTimeToMQTT(timeStr);
  }

  unsigned long currentMillis = millis();
  
  // Show date periodically
  if (currentMillis - lastDateDisplay >= DATE_DISPLAY_INTERVAL) {
    lastDateDisplay = currentMillis;
    
    // Show date
    matrix.fillScreen(LOW);
    char dateStr[11];
    sprintf(dateStr, "%02d/%02d/%04d", 
            timeinfo->tm_mday, 
            timeinfo->tm_mon + 1,
            timeinfo->tm_year + 1900);
    scrollText(dateStr);
    
    // Show timezone
    scrollText(timezone_name);
  }

  // Clear display and show time using mini digits
  matrix.fillScreen(LOW);
  drawMiniTime(timeinfo->tm_hour, timeinfo->tm_min);
  matrix.write();
  
  delay(1000); // Update time every second
}