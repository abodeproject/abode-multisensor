
#define MQTT_MAX_PACKET_SIZE 512

#include <ESP8266WiFi.h>
#include <WiFiClient.h> 
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <PubSubClient.h>
#include <EEPROM.h>
#include <DHT.h>

ESP8266WebServer server(80);

/* Wifi Vars */
const char *password = "abodesensor";
const int max_tries = 30;
int connected = 0;
char ssid[12];
byte mac[6];
IPAddress apIP(192, 168, 4, 1);
IPAddress netMsk(255, 255, 255, 0);

/* DNS Vars */
const byte DNS_PORT = 53;
DNSServer dnsServer;

/* OTA Vars */
const int OTAport = 8266;

/* PubSub Vars */
WiFiClient espClient;
PubSubClient mqtt(espClient);
String mqttTopic;
int mqtt_connected = 0;
int mqttAttemptInterval = 10000;
int lastMqttAttempt = 0;
int heartbeatInterval = 30000;
int lastHeartbeat = 0;

/* Config Vars */
const int CONFIGVERSION = 1;
struct Config{
  int version;
  char name[32];
  char wifi_ssid[32];
  char wifi_pass[32];
  char mqtt_server[32];
  char mqtt_user[24];
  char mqtt_pass[24];
  char mqtt_topic[32];
  char ota_pass[24];
  int mqtt_port;
  int motion_age;
};

Config cfg;

/* LED Vars */
#define REDPIN D1
#define GREENPIN D2
#define BLUEPIN D3

/* DHT Vars */
#define DHTPIN D7
#define DHTTYPE DHT22
DHT dht(DHTPIN, DHTTYPE);
int DHTPollInterval = 10000;
int lastDHTPoll = 0;
float tempValue;
float humValue;
float tempLast;
float humLast;


/* PIR Vars */
#define PIRPIN D5
int PIRPollInterval = 1000;
int lastPIRPoll = 0;
int pirValue;
int pirLast;
int pirLastOff = 0;

/* LDR Vars */
#define LDRPIN A0
int PLDRPollInterval = 10000;
int lastLDRPoll = 0;
float ldrValue;
float ldrLast;

void setup() {
  set_led(255, 170, 0);
  
  delay(1000);
  Serial.begin(115200);
  Serial.println();

  pinMode(DHTPIN, INPUT);
  pinMode(PIRPIN, INPUT);
  pinMode(LDRPIN, INPUT);
  
  read_config();
  wifi_setup();
  setup_http();
  check_mqtt();
  ota_setup();
  
  pir_setup();
  dht.begin();
}

void loop() {
  server.handleClient();
  dnsServer.processNextRequest();
  ArduinoOTA.handle();

  check_mqtt();
  set_led_status();

  pollDHT();
  pollPIR();
  pollLDR();

  process_heartbeat();
}

void process_heartbeat() {
  if (millis() < lastHeartbeat) {
    lastHeartbeat = millis();
  }
  
  if (millis() - lastHeartbeat > heartbeatInterval && connected == 1) {
    Serial.println("Sending heartbeat.");
    mqttPublish();
    lastHeartbeat = millis();
  }
}

void setup_http() {
  
  server.on("/", handleRoot);
  server.on("/settings", handleSettings);
  server.on("/save", handleSave);
  server.on("/restart", handleRestart);
  server.on("/wifi.json", handleNetworks);
  server.on("/status.json", handleStatus);
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.println("HTTP server started.");
  
}

void set_led_status() {

  if (WiFi.status() != WL_CONNECTED) {
    set_led(255, 0, 0);
  } else if (!mqtt.connected()) {
    set_led(255, 255, 0);
  } else {
    set_led(0, 32, 0);
  }
  
}

void set_led(int red, int green, int blue) {
  analogWrite(REDPIN, red);
  analogWrite(GREENPIN, green);
  analogWrite(BLUEPIN, blue);
}

void ota_setup() {
  if (connected == 0) {
    return;
  }

  Serial.print("Setting up OTA for ");
  Serial.print(cfg.name);
  Serial.print("...");
  ArduinoOTA.setPort(OTAport);
  ArduinoOTA.setHostname(cfg.name);
  ArduinoOTA.setPassword((const char *)cfg.ota_pass);

  ArduinoOTA.onStart([]() {
    set_led(0, 0, 255);
    Serial.print("Performing OTA Update");
  });

  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
      Serial.print(".");
  });

  ArduinoOTA.onEnd([]() {
    Serial.print("complete");
  });

  ArduinoOTA.onError([](ota_error_t error) {
    Serial.print("failed (");
    Serial.print(error);
    Serial.println(")");
  });

  ArduinoOTA.begin();
  Serial.println("done");
}

void check_mqtt() {
  if (connected == 0) {
    return;
  }

  if (mqtt.connected()) {
    return;
  }
  
  if (millis() - lastMqttAttempt > mqttAttemptInterval) {
    lastMqttAttempt = millis();
    
    if (cfg.mqtt_server == "") {
      Serial.println("No MQTT Server configured");
      return; 
    }
  
    if (cfg.mqtt_port < 1) {
      Serial.println("No MQTT Port configured");
      return; 
    }
  
    if (cfg.mqtt_topic == "") {
      Serial.println("No MQTT Topic configured");
      return;
    }

  
    mqtt.setServer(cfg.mqtt_server, cfg.mqtt_port);
    mqtt.setCallback(mqttHandler);
  
    Serial.print("Connecting to MQTT Server '");
    Serial.print(cfg.mqtt_server);
    Serial.print(":");
    Serial.print(cfg.mqtt_port);
    Serial.print("'");
  
    int mqtt_tries = 0;
    int mqtt_max_tries = 3;
    
    while (!mqtt.connected()) {
      mqtt_tries++;
  
      if (mqtt_tries > mqtt_max_tries) {
        break;
      }
      
      Serial.print(".");
      if (mqtt.connect(cfg.name)) {
        mqtt.subscribe(cfg.mqtt_topic);
        break;
      }
      
      delay(1000);
    }
  
    if (mqtt.connected()) {
      Serial.println("connected");
      mqtt_connected = 1;
    } else {
      Serial.print("failed (");
      Serial.print(mqtt.state());
      Serial.println(")");
      mqtt_connected = 0;
    }
  }

  /* Handle rollover */
  if (millis() < lastDHTPoll) {
    lastDHTPoll = millis();
  }
  
}

void mqttPublish() {
  if (mqtt_connected == 0) {
    return;
  }

  String document = "{";
  document = "" + document + ""
  "\"name\": \"" + cfg.name + "\", "
  "\"ip\": \"" + WiFi.localIP().toString() + "\", "
  "\"time\": " + millis() + ", "
  "\"temperature\": " + tempValue + ", "
  "\"humidity\": " + humValue + ", "
  "\"motion\": " + pirValue + ", "
  "\"lux\": " + ldrValue + ""
  "}";

  char buffer[512];

  document.toCharArray(buffer, 512);
  Serial.println(buffer);
  Serial.print("Publishing to MQTT topic: ");
  Serial.print(cfg.mqtt_topic);
  boolean result = mqtt.publish(cfg.mqtt_topic, buffer, true);
  Serial.print(result ? " success" : " failed");
  Serial.print(" (");
  Serial.print(mqtt.state());
  Serial.println(")");

  if (!result) {
    mqtt.disconnect();
  }
}

void mqttHandler (char* topic, byte* payload, unsigned int length) {
  Serial.println(topic);
}

void wifi_setup() {
  /* Try to connect to the Wifi */
  WiFi.mode(WIFI_STA);

  if (strlen(cfg.wifi_ssid) != 0) {
    Serial.print("Connecting to WiFi '");
    Serial.print(cfg.wifi_ssid);
    Serial.print("'");
    WiFi.begin(cfg.wifi_ssid, cfg.wifi_pass);
    
    int tries = 0;
  
    /* Wait for connection */
    while (WiFi.status() != WL_CONNECTED) {
      tries++;
  
      if (tries > max_tries) {
        Serial.println("failed");
        break;
      }
      
      Serial.print(".");
      delay(1000);
    }
  }

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.macAddress(mac);
    String ssid_str = "sensor_" + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
    ssid_str.toCharArray(ssid, 12);
    Serial.println("Configuring access point..." + ssid_str);
    WiFi.softAPConfig(apIP, apIP, netMsk);
    WiFi.softAP(ssid, password);
    
    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(myIP);

    dnsServer.start(DNS_PORT, "*", myIP);
  } else {
    connected = 1;
    Serial.println("connected");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  }
}

void read_config() {
  /* Read the EEPROM for a config */
  EEPROM.begin(256);
  Serial.println("Reading Config from EEPROM");
  EEPROM.get(0, cfg);

  Serial.print("Configuration Version: ");
  Serial.println(cfg.version);
  
  if (cfg.version != CONFIGVERSION) {
    Serial.println("No configuration found");

    cfg.version = CONFIGVERSION;
    Serial.print("Setting configuration version: ");
    Serial.println(cfg.version);
    
    WiFi.macAddress(mac);
    String("sensor_" + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX)).toCharArray(cfg.name, 32);
    String("").toCharArray(cfg.wifi_ssid, 32);
    String("").toCharArray(cfg.wifi_pass, 32);
    String("").toCharArray(cfg.mqtt_server, 32);
    String("").toCharArray(cfg.mqtt_user, 32);
    String("").toCharArray(cfg.mqtt_pass, 32);
    String("").toCharArray(cfg.mqtt_topic, 32);
    String("").toCharArray(cfg.ota_pass, 24);
    cfg.mqtt_port = 1883;
    cfg.motion_age = 60;
  
  }
}

void pollDHT() {

  /* Handle rollover */
  if (millis() < lastDHTPoll) {
    lastDHTPoll = millis();
  }
  
  if (millis() - lastDHTPoll > DHTPollInterval) {
    
    float newTempValue = dht.readTemperature(true); //to use celsius remove the true text inside the parentheses  
    float newHumValue = dht.readHumidity();

    lastDHTPoll = millis();
    
    if (isnan(newTempValue) || isnan(newHumValue)) {
      Serial.println("Failed to poll DHT");
      return;
    }

    tempValue = newTempValue;
    humValue = newHumValue;

    if (tempValue != tempLast || humValue != humLast) {
      mqttPublish();

      tempLast = tempValue;
      humLast = humValue;
      
      Serial.print("Polled DHT: t:");
      Serial.print(tempValue);
      Serial.print(" h:");
      Serial.println(humValue);
    }
  }
  
}

void pir_setup() {
  pirValue = digitalRead(PIRPIN);
  
  attachInterrupt(digitalPinToInterrupt(PIRPIN), handlePirOn, RISING);
}

void handlePirOn() {
  detachInterrupt(PIRPIN);
  attachInterrupt(digitalPinToInterrupt(PIRPIN), handlePirOff, FALLING);
  
  pirValue = HIGH;
  pirLastOff = 0;

  if (pirValue != pirLast) {
    Serial.println("Motion detected");
    mqttPublish();
    pirLast = pirValue;
  } else {
    Serial.println("Resetting motion age");
  }
}

void handlePirOff() {
  detachInterrupt(PIRPIN);
  attachInterrupt(digitalPinToInterrupt(PIRPIN), handlePirOn, RISING);

  Serial.println("Motion no longer detected, waiting for motion age");
  pirLastOff = millis();
}

void pollPIR() {

  if (pirLastOff == 0) {
    return;
  }

  
  if (millis() - pirLastOff > cfg.motion_age * 1000) {
    Serial.println("Motion age reached, no motion detected");
    pirValue = LOW;
    pirLastOff = 0;
    mqttPublish();
  }
}

void pollLDR() {

  if (millis() - lastLDRPoll > PLDRPollInterval) {
    
    ldrValue = analogRead(LDRPIN);
    ldrValue = ldrValue / 1024 * 100;
    if (ldrValue > 100) {
      ldrValue = 100;
    }
    lastLDRPoll = millis();

    if (ldrValue != ldrLast) {
      mqttPublish();
      ldrLast = ldrValue;
      
      Serial.print("Polled LDR: ");
      Serial.println(ldrValue);
    }
  }

  /* Handle rollover */
  if (millis() < lastLDRPoll) {
    lastLDRPoll = millis();
  }
}

boolean isIp(String str) {
  for (int i = 0; i < str.length(); i++) {
    int c = str.charAt(i);
    if (c != '.' && (c < '0' || c > '9')) {
      return false;
    }
  }
  return true;
}

String toStringIp(IPAddress ip) {
  String res = "";
  for (int i = 0; i < 3; i++) {
    res += String((ip >> (8 * i)) & 0xFF) + ".";
  }
  res += String(((ip >> 8 * 3)) & 0xFF);
  return res;
}

boolean captivePortal() {
   if (!isIp(server.hostHeader())) {
    Serial.println("Request redirected to captive portal");
    server.sendHeader("Location", String("http://") + toStringIp(server.client().localIP()) + "/settings", true);
    server.send ( 302, "text/plain", ""); 
    server.client().stop();
    return true;
   }

   return false;
}
void handleNotFound() {
  if (captivePortal()) { // If caprive portal redirect instead of displaying the error page.
    return;
  }

  server.send ( 404, "text/plain", "Not found" );
}

const String styles = "<style>"
"body {"
  "color: #c8c8c8;"
  "background-color: #636a71;"
  "font-family: \"Helvetica Neue\", Helvetica, Arial, sans-serif;"
  "font-size: 12px;"
  "padding: 10px;"
"}"
"#content {"
  "margin: 20px auto;"
  "background-color: #272b30;"
  "max-width: 400px;"
  "border: 1px solid black;"
  "border-radius: 4px 4px;"
  "padding: 10px;"
  "text-align: center;"
"}"
"#nav {"
  "list-style: none;"
  "margin: 10px 10px;"
  "padding: 4px;"
  "text-align: center;"
  "border-bottom: 1px solid gray;"
"}"
"#nav li {"
  "display: inline;"
  "padding: 0px 10px;"
"}"
"#nav li a {"
  "color: white;"
"}"
"#e {"
  "font-weight: bold;"
  "color: red;"
"}"
"#i {"
  "font-weight: bold;"
  "color: green;"
"}"
"#networks {"
  "list-style: none;"
  "padding: 0px;"
  "margin: 0px 10px;"
  "text-align: left;"
"}"
"#networks li {"
  "cursor: pointer;"
  "padding: 2px;"
  "margin: 2px;"
  "background-color: #131619;"
  "border: 1px solid #47535e;"
  "border-radius: 2px 2px;"
  "font-size: 1.2em;"
  "padding: 2px;"
"}"
".encryption {"
  "visibility: hidden;"
  "margin-right: 1em;"
  "font-weight: bold;"
  "color: orange;"
"}"
"#networks li.encrypted .encryption {"
  "visibility: visible;"
"}"
".signal {"
  "float: right;"
  "font-size: .8em;"
"}"
".label {"
  "font-weight: bold;"
"}"
"input {"
  "margin: 2px;"
  "padding: 4px;"
  "border: 1px solid gray;"
  "border-radius: 6px 6px;"
"}"
"</style>";

void handleRoot() {
  String document = ""
  "<html>"
  "<head>"
    "<title>Multi-Sensor</title>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "" + styles + ""
  "</head>"
  "<body>"
  "<div id='content'>"
  "<ul id='nav'>"
  "<li><a href='/'>Status</a></li>"
  "<li><a href='/settings'>Settings</a></li>"
  "<li><a href='/restart'>Restart</a></li>"
  "</ul>"
  "<table border='1' width='100%'>"
  "<tr>"
  "<td>Temp</td>"
  "<td>Humidity</td>"
  "<td>Motion</td>"
  "<td>Lux</td>"
  "<td>Time</td>"
  "</tr>"
  "<tr>"
  "<td>" + tempValue + "</td>"
  "<td>" + humValue + "</td>"
  "<td>" + pirValue + "</td>"
  "<td>" + ldrValue + "</td>"
  "<td>" + millis() + "</td>"
  "</tr>"
  "</table>"
  "</div>"
  "</body>"
  "</html>";
  
  server.send(200, "text/html", document);
};

void handleSettings() {
  Serial.println("Client connected");

  String document = ""
  "<html>"
  "<head>"
    "<title>Multi-Sensor</title>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "" + styles + ""
  "</head>"
  "<script language='JavaScript'>\n"
  "function getNetworks() {\n"
    "document.getElementById('i').innerHTML='Loading Networks...';\n"
    "fetch('/wifi.json')\n"
    ".then(function (r) {\n"
      "r.json().then(function (N) {;\n"
        "var nets = {};\n"
        "document.getElementById('networks').innerHTML='';\n"
        "N.forEach(function (n) { nets[n.n] = n; });\n"
        "Object.values(nets).forEach(function(n) {\n"
          "var e = document.createElement('li');\n"
          "e.className = (n.e) ? 'encrypted' : '';\n"
          "e.innerHTML += '<span class=\"encryption\">&#9919;</span>' + n.n + '<span class=\"signal\">' + n.s + '</span>';\n"
          "e.onclick=function() {\n"
          "document.forms.config.wifi_ssid.value=n.n;\n"
          "};\n"
          "document.getElementById('networks').appendChild(e);\n"
          "});\n"
        "})\n"
        "document.getElementById('i').innerHTML='';\n"
      "})\n"
      ".catch(function (e) {\n"
        "console.log(e);\n"
        "document.getElementById('i').innerHTML='';\n"
        "document.getElementById('e').innerHTML='Failed to get networks';\n"
      "});\n"
  "}\n"
  "</script>"
  "<body>"
  "<div id='content'>"
  "<ul id='nav'>"
  "<li><a href='/'>Status</a></li>"
  "<li><a href='/settings'>Settings</a></li>"
  "<li><a href='/restart'>Restart</a></li>"
  "</ul>"
  "<form name='config' action='/save' method='post'>"
  "<h3>General</h3>"
  "<div class='label'>Sensor Name</div>"
  "<div><input type='input' placeholder='Name' name='name' value='" + cfg.name + "'></div>"
  "<div class='label'>OTA Password</div>"
  "<div><input type='input' placeholder='OTA Password' name='ota_password' autocapitalize='off' value='" + cfg.ota_pass + "'></div>"
  "<div class='label'>Motion Age (seconds)</div>"
  "<div><input type='input' placeholder='Motion Age (seconds)' name='motion_age' value='" + cfg.motion_age + "'></div>"
  "<h3>MQTT Settings</h3>"
  "<div class='label'>Server</div>"
  "<div><input type='input' placeholder='MQTT Server' name='mqtt_server' autocapitalize='off' value='" + cfg.mqtt_server + "'></div>"
  "<div class='label'>User</div>"
  "<div><input type='input' placeholder='MQTT User' name='mqtt_user' autocapitalize='off' value='" + cfg.mqtt_user + "'></div>"
  "<div class='label'>Password</div>"
  "<div><input type='input' placeholder='MQTT Password' name='mqtt_password'autocapitalize='off' ></div>"
  "<div class='label'>Topic</div>"
  "<div><input type='input' placeholder='MQTT Topic' name='mqtt_topic' autocapitalize='off' value='" + cfg.mqtt_topic + "'></div>"
  "<div class='label'>Port</div>"
  "<div><input type='input' placeholder='MQTT Port' name='mqtt_port' value='" + cfg.mqtt_port + "'></div>"
  "<h3>WiFi Settings</h3>"
  "<div>&nbsp;</div>"
  "<button type='button' onclick='getNetworks()'>Scan Networks</button>"
  "<div>&nbsp;</div>"
  "<div id='i'></div>"
  "<div id='e'></div>"
  "<div>"
    "<ul id='networks'></ul>"
  "</div>"
  "<div>&nbsp;</div>"
  "<div class='label'>SSID</div>"
  "<div><input type='input' placeholder='SSID' name='wifi_ssid' value='" + cfg.wifi_ssid + "'></div>"
  "<div class='label'>Password</div>"
  "<div><input type='input' placeholder='Password' name='wifi_password'></div>"
  "<div>&nbsp;</div>"
  "<div><input type='submit' value='Save'></div>"
  "</form>"
  "</div>"
  "</body>"
  "</html>";
  server.send(200, "text/html", document);
}

void returnFail(String msg) {
  String document = ""
  "<html>"
  "<head>"
    "<title>Multi-Sensor</title>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "" + styles + ""
  "</head>"
  "<body>"
  "<div id='content'>"
  "<ul id='nav'>"
  "<li><a href='/'>Status</a></li>"
  "<li><a href='/settings'>Settings</a></li>"
  "<li><a href='/restart'>Restart</a></li>"
  "</ul>"
  "<div id='e'>" + msg + "</div>"
  "</div>"
  "</body>"
  "</html>";
  
  server.send(400, "text/plain", document);
}

void handleSave() {

  int changed = 0;
  int reboot = 0;

  if (server.hasArg("name")) {
    changed = 1;
    Serial.println("Saving Sensor Name");
    char name_char[32];
    server.arg("name").toCharArray(name_char, 32);
    strcpy(cfg.name, name_char);
  }

  if (server.hasArg("ota_password") && server.arg("ota_password") != "") {
    changed = 1;
    reboot = 1;
    Serial.println("Saving OTA Password");
    char ota_pass_char[24];
    server.arg("ota_password").toCharArray(ota_pass_char, 24);
    strcpy(cfg.ota_pass, ota_pass_char);
  }

  if (server.hasArg("mqtt_server")) {
    changed = 1;
    reboot = 1;
    Serial.println("Saving MQTT Server");
    char mqtt_server_char[32];
    server.arg("mqtt_server").toCharArray(mqtt_server_char, 32);
    strcpy(cfg.mqtt_server, mqtt_server_char);
  }

  if (server.hasArg("mqtt_user")) {
    changed = 1;
    reboot = 1;
    Serial.println("Saving MQTT User");
    char mqtt_user_char[32];
    server.arg("mqtt_user").toCharArray(mqtt_user_char, 32);
    strcpy(cfg.mqtt_user, mqtt_user_char);
  }

  if (server.hasArg("mqtt_password") && server.arg("mqtt_password") != "") {
    changed = 1;
    reboot = 1;
    Serial.println("Saving MQTT Password");
    char mqtt_password_char[32];
    server.arg("mqtt_password").toCharArray(mqtt_password_char, 32);
    strcpy(cfg.mqtt_pass, mqtt_password_char);
  }

  if (server.hasArg("mqtt_port")) {
    changed = 1;
    reboot = 1;
    Serial.println("Saving MQTT Port");
    cfg.mqtt_port = server.arg("mqtt_port").toInt();
  }

  if (server.hasArg("mqtt_topic")) {
    changed = 1;
    reboot = 1;
    Serial.println("Saving MQTT Topic");
    char mqtt_topic_char[32];
    server.arg("mqtt_topic").toCharArray(mqtt_topic_char, 32);
    strcpy(cfg.mqtt_topic, mqtt_topic_char);
  }

  if (server.hasArg("wifi_ssid")) {
    changed = 1;
    reboot = 1;
    Serial.println("Saving Wifi SSID");
    char wifi_ssid_char[32];
    server.arg("wifi_ssid").toCharArray(wifi_ssid_char, 32);
    strcpy(cfg.wifi_ssid, wifi_ssid_char);
  }

  if (server.hasArg("wifi_password") && server.arg("wifi_password") != "") {
    changed = 1;
    reboot = 1;
    Serial.println("Saving WIFI Password");
    char wifi_password_char[32];
    server.arg("wifi_password").toCharArray(wifi_password_char, 32);
    strcpy(cfg.wifi_pass, wifi_password_char);
  }

  if (server.hasArg("motion_age")) {
    changed = 1;
    Serial.println("Saving Motion Age");
    cfg.motion_age = server.arg("motion_age").toInt();
  }

  if (changed == 0) {
    server.send(200, "text/html", "Nothing changed");
    return;
  }

  Serial.println("Saving configuration");
  EEPROM.begin(256);
  EEPROM.put(0, cfg);
  EEPROM.commit();
  
  String document = ""
  "<html>"
  "<head>"
    "<title>Multi-Sensor</title>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "" + styles + ""
  "</head>"
  "<body>"
  "<div id='content'>Applying configuration. Device may reboot if WiFi or MQTT settings changed.</div>"
  "</body>"
  "</html>";
  
  server.send(200, "text/html", document);
  if (reboot) {
    Serial.println("Restarting...");
    delay(2000);
  
    WiFi.forceSleepBegin(); wdt_reset(); ESP.restart(); while(1)wdt_reset();
  }
}

void handleRestart() {
  String document = ""
  "<html>"
  "<head>"
    "<title>Multi-Sensor</title>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "" + styles + ""
  "</head>"
  "<body>"
  "<div id='content'>Restarting</div>"
  "</body>"
  "</html>";
  
  server.send(200, "text/html", document);
  delay(1000);
  
  WiFi.forceSleepBegin(); wdt_reset(); ESP.restart(); while(1)wdt_reset();
}

void handleStatus() {
  String document = "{";
  
  document = "" + document + ""
  "\"temperature\": " + tempValue + ", "
  "\"humidity\": " + humValue + ", "
  "\"motion\": " + pirValue + ", "
  "\"lux\": " + ldrValue + ", "
  "\"time\": " + millis() + ", "
  "\"name\": \"" + cfg.name + "\", "
  "\"ip\": \"" + WiFi.localIP().toString() + "\""
  "}";
  
  server.send(200, "application/json", document);
}

void handleNetworks() {
  Serial.println("Scanning for networks");
  
  int n = WiFi.scanNetworks();
  Serial.println("Scan done");
  
  if (n == 0) {
    Serial.println("No networks found");
    server.send(404, "application/json", "{\"message\": \"No networks found\"}");
  } else {
    String networks = "[";
    
    Serial.print(n);
    Serial.println(" networks found");
    for (int i = 0; i < n; ++i)
    {
      
      if (i > 0) {
        networks = networks + ", ";
      }
      
      int encryption = (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? 0 : 1;
      networks = networks + "{\"n\": \"" + WiFi.SSID(i) + "\", \"s\": " + WiFi.RSSI(i) + ", \"e\": " + encryption + "}";
    }

    networks = networks + "]";
    server.send(200, "application/json", networks);
  }
  
}




