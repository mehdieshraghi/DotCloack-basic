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
String mqtt_user = "";
String mqtt_password = "";

#define MQTT_TOPIC_MESSAGE "clock/message"  // New topic for displaying messages

// For storing received MQTT messages
String mqtt_display_message = "";
unsigned long message_display_time = 0;
const unsigned long MESSAGE_DISPLAY_DURATION = 10000;  // Display message for 10 seconds

// Timezone Configuration
float timezone_offset = 0.0; // Default GMT+0
String timezone_name = "GMT"; // Default timezone name
bool use_12h_format = false; // Default 24h format

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
bool show_seconds = true; // Show seconds by default

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

// AM/PM indicators (2x5 pixels)
const byte MINI_AM_INDICATOR[5] = { B01, B11, B11, B11, B11 }; // A
const byte MINI_PM_INDICATOR[5] = { B11, B11, B11, B10, B10 }; // P

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
  doc["use_12h_format"] = use_12h_format;
  doc["show_seconds"] = show_seconds;
  
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
        use_12h_format = doc["use_12h_format"];
        show_seconds = doc["show_seconds"];
        
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

// Sync time with NTP servers
void syncNTP() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.println("NTP sync requested");
  
  // Wait for time to be synchronized
  time_t now = time(nullptr);
  int retries = 0;
  while (now < 8 * 3600 * 2 && retries < 20) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
    retries++;
  }
  Serial.println("");
  
  // Apply timezone
  applyTimezoneSettings();
}

// Apply timezone settings
void applyTimezoneSettings() {
  // Calculate hours and minutes offset
  int hours = (int)timezone_offset;
  int minutes = (int)((timezone_offset - hours) * 60);
  
  // Format timezone string based on offset
  char tz[50];
  if (minutes == 0) {
    sprintf(tz, "GMT%+d", hours);
  } else {
    sprintf(tz, "GMT%+d:%02d", hours, abs(minutes));
  }
  
  setenv("TZ", tz, 1);
  tzset(); // Apply timezone change
  Serial.print("Timezone set to: ");
  Serial.println(tz);
}

// Generate timezone selection HTML with 30-minute intervals
String generateTimezoneOptions() {
  String options = "";
  for (float i = -12.0; i <= 14.0; i += 0.5) {
    String label = "GMT";
    int hours = (int)i;
    int minutes = (int)((i - hours) * 60);
    
    if (i > 0) label += "+";
    if (i != 0) {
      label += String(hours);
      if (minutes != 0) {
        label += ":";
        if (minutes < 10) label += "0";
        label += String(minutes);
      }
    }
    
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
    html += ".btn{background:#4CAF50;color:white;border:none;padding:10px 15px;border-radius:5px;cursor:pointer;font-size:16px;margin:5px;}";
    html += ".btn:hover{background:#45a049;}";
    html += ".btn-update{background:#2196F3;}";
    html += ".btn-update:hover{background:#0b7dda;}";
    html += ".btn-sync{background:#ff9800;}";
    html += ".btn-sync:hover{background:#e68a00;}";
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
    html += "<div class='form-group'>";
    html += "<input type='checkbox' id='use_12h_format' name='use_12h_format' " + String(use_12h_format ? "checked" : "") + ">";
    html += "<label for='use_12h_format' class='checkbox-label'>Use 12-hour format (AM/PM)</label>";
    html += "</div>";
    html += "<div class='form-group'>";
    html += "<input type='checkbox' id='show_seconds' name='show_seconds' " + String(show_seconds ? "checked" : "") + ">";
    html += "<label for='show_seconds' class='checkbox-label'>Show seconds</label>";
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
    
    // NTP Sync and Firmware Update Panel
    html += "<div class='panel' style='margin-top:30px;'>";
    html += "<h2>System Operations</h2>";
    html += "<p>Current version: " + String(VERSION) + "</p>";
    html += "<div style='text-align:center;'>";
    html += "<a href='/sync' class='btn btn-sync' style='display:inline-block;text-decoration:none;'>Sync Time (NTP)</a>";
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
      timezone_offset = server.arg("timezone").toFloat();
    }
    
    if (server.hasArg("timezone_name")) {
      timezone_name = server.arg("timezone_name");
    }
    
    use_12h_format = server.hasArg("use_12h_format");
    show_seconds = server.hasArg("show_seconds");
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

  // NTP Sync Endpoint
  server.on("/sync", HTTP_GET, []() {
    syncNTP();
    server.sendHeader("Location", "/");
    server.send(303);
  });
  
  server.begin();
  Serial.println("Web server started");
}

// Connect to MQTT and setup callback
void connectMQTT() {
  if (!useMQTT) return;
  
  mqttClient.setServer(mqtt_server.c_str(), mqtt_port);
  
  // Set the message callback
  mqttClient.setCallback(mqttCallback);
  
  if (mqttClient.connect("ESP8266Clock", mqtt_user.c_str(), mqtt_password.c_str())) {
    Serial.println("Connected to MQTT server");
    // Subscribe to message topic
    mqttClient.subscribe(MQTT_TOPIC_MESSAGE);
  } else {
    Serial.print("Error connecting to MQTT, state: ");
    Serial.println(mqttClient.state());
  }
}

// MQTT message callback
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  
  if (topicStr == MQTT_TOPIC_MESSAGE) {
    // Convert payload to string
    mqtt_display_message = "";
    for (unsigned int i = 0; i < length; i++) {
      mqtt_display_message += (char)payload[i];
    }
    
    // Set time to display the message
    message_display_time = millis();
    
    Serial.print("Received message: ");
    Serial.println(mqtt_display_message);
  }
}


// Draw custom 3x5 mini digit - now calls the more general drawMiniChar
void drawMiniDigit(int digit, int x, int y) {
  if (digit < 0 || digit > 9) return;
  drawMiniChar('0' + digit, x, y);
}

// Draw AM/PM indicator (2x5 pixels)
void drawAMPM(bool isPM, int x, int y) {
  const byte* indicator = isPM ? MINI_PM_INDICATOR : MINI_AM_INDICATOR;
  
  for (int row = 0; row < 5; row++) {
    byte rowData = indicator[row];
    for (int col = 0; col < 2; col++) {
      if (bitRead(rowData, 1 - col)) {
        matrix.drawPixel(x + col, y + row, HIGH);
      }
    }
  }
}

// Calculate display dimensions based on current settings with extra spacing
void getTimeDisplayDimensions(int& width, int& height) {
  width = 0;
  height = 7; // Fixed height of the 3x5 digits + 1 pixel margin
  
  // HH:MM with extra spacing
  width += 4 * 3; // Four digits (hours and minutes)
  width += 1 * 3; // Three spaces between digits
  width += 2;     // Colon width
  width += 6;     // Extra spacing around colons (3 before + 3 after)
  
  // Seconds if enabled with extra spacing
  if (show_seconds) {
    width += 5;     // Extra space before second colon
    width += 2;     // Second colon width
    width += 3;     // Extra space after second colon
    width += 2 * 3; // Two digits (seconds)
    width += 1;     // Space between seconds digits
  }
  
  // AM/PM indicator if using 12h format
  if (use_12h_format) {
    width += 4;     // Extra space before AM/PM
    width += 2;     // Width of AM/PM indicator
  }
}

// Draw time with spacing matching the photo
void drawCenteredTime(int hours, int minutes, int seconds) {
  // Adjust hours for 12-hour format if needed
  bool isPM = false;
  if (use_12h_format) {
    isPM = hours >= 12;
    hours = hours % 12;
    if (hours == 0) hours = 12; // 12 AM/PM instead of 0
  }
  
  // Fixed starting position optimized based on the photo
  int x = 0;
  int y = 1; // Fixed vertical position for better visibility
  
  // Draw hours
  drawMiniDigit(hours / 10, x, y);
  x += 4;
  drawMiniDigit(hours % 10, x, y);
  
  // Add space before colon (matching photo)
  x += 4;
  
  // Draw first colon
  drawMiniChar(':', x, y);
  x += 2;
  
  // Space after colon (matching photo)
  x += 2;
  
  // Draw minutes
  drawMiniDigit(minutes / 10, x, y);
  x += 4;
  drawMiniDigit(minutes % 10, x, y);
  
  // Draw seconds if enabled
  if (show_seconds) {
    // Space before second colon (matching photo)
    x += 4;
    
    // Draw second colon
    drawMiniChar(':', x, y);
    x += 2;
    
    // Space after second colon (matching photo)
    x += 2;
    
    // Draw seconds
    drawMiniDigit(seconds / 10, x, y);
    x += 4;
    drawMiniDigit(seconds % 10, x, y);
  }
  
  // Draw AM/PM indicator if using 12h format
  if (use_12h_format && !show_seconds) { // Only show AM/PM if seconds are disabled
    x += 4;
    drawAMPM(isPM, x, y);
  }
}

// Define mini letters for common characters
const byte MINI_LETTERS[36][5] = {
  // 0-9 (first 10 are the same as MINI_DIGITS)
  { B111, B101, B101, B101, B111 },  // 0
  { B010, B110, B010, B010, B111 },  // 1
  { B111, B001, B111, B100, B111 },  // 2
  { B111, B001, B111, B001, B111 },  // 3
  { B101, B101, B111, B001, B001 },  // 4
  { B111, B100, B111, B001, B111 },  // 5
  { B111, B100, B111, B101, B111 },  // 6
  { B111, B001, B001, B001, B001 },  // 7
  { B111, B101, B111, B101, B111 },  // 8
  { B111, B101, B111, B001, B111 },  // 9
  // A-Z (next 26 characters)
  { B010, B101, B111, B101, B101 },  // A
  { B110, B101, B110, B101, B110 },  // B
  { B011, B100, B100, B100, B011 },  // C
  { B110, B101, B101, B101, B110 },  // D
  { B111, B100, B110, B100, B111 },  // E
  { B111, B100, B110, B100, B100 },  // F
  { B011, B100, B101, B101, B011 },  // G
  { B101, B101, B111, B101, B101 },  // H
  { B111, B010, B010, B010, B111 },  // I
  { B111, B010, B010, B010, B110 },  // J
  { B101, B101, B110, B101, B101 },  // K
  { B100, B100, B100, B100, B111 },  // L
  { B101, B111, B111, B101, B101 },  // M
  { B110, B101, B101, B101, B101 },  // N
  { B111, B101, B101, B101, B111 },  // O
  { B111, B101, B111, B100, B100 },  // P
  { B010, B101, B101, B110, B011 },  // Q
  { B110, B101, B110, B101, B101 },  // R
  { B011, B100, B010, B001, B110 },  // S
  { B111, B010, B010, B010, B010 },  // T
  { B101, B101, B101, B101, B111 },  // U
  { B101, B101, B101, B010, B010 },  // V
  { B101, B101, B111, B111, B101 },  // W
  { B101, B101, B010, B101, B101 },  // X
  { B101, B101, B010, B010, B010 },  // Y
  { B111, B001, B010, B100, B111 }   // Z
};

// Draw a mini character (3x5 pixels)
void drawMiniChar(char ch, int x, int y) {
  byte index = 0;
  
  if (ch >= '0' && ch <= '9') {
    index = ch - '0';
  } else if (ch >= 'A' && ch <= 'Z') {
    index = (ch - 'A') + 10;
  } else if (ch >= 'a' && ch <= 'z') {
    index = (ch - 'a') + 10; // Same as uppercase
  } else if (ch == '.') {
    matrix.drawPixel(x + 1, y + 4, HIGH); // Just a dot at the bottom
    return;
  } else if (ch == ':') {
    matrix.drawPixel(x + 1, y + 1, HIGH); // Top dot
    matrix.drawPixel(x + 1, y + 3, HIGH); // Bottom dot
    return;
  } else if (ch == '/') {
    matrix.drawPixel(x + 2, y, HIGH);
    matrix.drawPixel(x + 1, y + 1, HIGH);
    matrix.drawPixel(x + 1, y + 2, HIGH);
    matrix.drawPixel(x + 1, y + 3, HIGH);
    matrix.drawPixel(x, y + 4, HIGH);
    return;
  } else if (ch == '-') {
    matrix.drawPixel(x, y + 2, HIGH);
    matrix.drawPixel(x + 1, y + 2, HIGH);
    matrix.drawPixel(x + 2, y + 2, HIGH);
    return;
  } else if (ch == ' ') {
    return; // Space - draw nothing
  } else {
    // Unknown character, draw a block
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 5; j++) {
        matrix.drawPixel(x + i, y + j, HIGH);
      }
    }
    return;
  }
  
  // Draw the character from the definition
  for (int row = 0; row < 5; row++) {
    byte rowData = MINI_LETTERS[index][row];
    for (int col = 0; col < 3; col++) {
      if (bitRead(rowData, 2 - col)) {
        matrix.drawPixel(x + col, y + row, HIGH);
      }
    }
  }
}

// Display compact message using mini font
void displayCompactText(String text, int y = 0) {
  int len = text.length();
  int charWidth = 4; // Character width including spacing (3px + 1px space)
  int x = (32 - (len * charWidth)) / 2; // Center text (32 is matrix width for 4 modules)
  
  for (int i = 0; i < len; i++) {
    drawMiniChar(text.charAt(i), x + (i * charWidth), y);
  }
  matrix.write();
}

// Display startup information
void displayStartupInfo() {
  // Display name
  matrix.fillScreen(LOW);
  displayCompactText(DEVICE_NAME, 1);
  delay(1500);
  
  // Display IP address
  matrix.fillScreen(LOW);
  String ip = WiFi.localIP().toString();
  String ipText = "IP " + ip;
  for (int i = 0; i < ipText.length() * 4 + matrix.width(); i++) {
    matrix.fillScreen(LOW);
    int startPos = matrix.width() - i;
    int x = startPos;
    
    // Draw scrolling text using mini font
    for (int j = 0; j < ipText.length(); j++) {
      if (x >= -3 && x < matrix.width()) {
        drawMiniChar(ipText.charAt(j), x, 1);
      }
      x += 4;
    }
    matrix.write();
    delay(50);
  }
  delay(500);
  
  // Display timezone
  matrix.fillScreen(LOW);
  displayCompactText(timezone_name, 1);
  delay(1500);
  
  // Display website
  matrix.fillScreen(LOW);
  displayCompactText("ONEOS.IR", 1);
  delay(1500);
}

// Display scrolling message using mini font - optimized for date display
void scrollText(String message) {
  // Determine if message will fit on screen without scrolling
  int messageWidth = message.length() * 4;
  
  if (messageWidth <= 32) {
    // Message fits on screen - display centered
    matrix.fillScreen(LOW);
    int startX = (32 - messageWidth) / 2;
    for (int i = 0; i < message.length(); i++) {
      drawMiniChar(message.charAt(i), startX + (i * 4), 1);
    }
    matrix.write();
    delay(2000); // Show static message
  } else {
    // Message needs scrolling
    for (int i = 0; i < message.length() * 4 + 32; i++) {
      matrix.fillScreen(LOW);
      int startPos = 32 - i;
      int x = startPos;
      
      // Draw scrolling text using mini font
      for (int j = 0; j < message.length(); j++) {
        if (x >= -3 && x < 32) {
          drawMiniChar(message.charAt(j), x, 1);
        }
        x += 4;
      }
      matrix.write();
      delay(40); // Slightly faster scroll
    }
  }
}

void setup() {
  Serial.begin(9600);
  Serial.println("\n\n========================");
  Serial.println("SmartClock - v" + String(VERSION));
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
  syncNTP();

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


  unsigned long currentMillis = millis();
  
  // Show date periodically
  if (currentMillis - lastDateDisplay >= DATE_DISPLAY_INTERVAL) {
    lastDateDisplay = currentMillis;
    
    // Show date in DD/MM/YYYY format
    matrix.fillScreen(LOW);
    char dateStr[11];
    sprintf(dateStr, "%02d/%02d/%04d", 
            timeinfo->tm_mday, 
            timeinfo->tm_mon + 1,
            timeinfo->tm_year + 1900);
    scrollText(dateStr);
    
    // Show timezone if not too long
    if (timezone_name.length() <= 8) {
      matrix.fillScreen(LOW);
      displayCompactText(timezone_name, 1);
      delay(2000);
    }
  }

  // Clear display and show time with proper spacing
  matrix.fillScreen(LOW);
  drawCenteredTime(timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec);
  matrix.write();
  
  delay(1000); // Update time every second
}