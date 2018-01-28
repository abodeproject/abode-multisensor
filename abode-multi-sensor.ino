#include <ESP8266WiFi.h>
#include <WiFiClient.h> 
#include <ArduinoOTA.h>
#include <ESP8266WebServer.h>
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

/* OTA Vars */
const int OTAport = 8266;

/* PubSub Vars */
WiFiClient espClient;
PubSubClient mqtt(espClient);
String mqttTopic;
int mqtt_connected = 0;
int mqttAttemptInterval = 10000;
int lastMqttAttempt = 0;


/* Config Vars */
struct Config{
  char name[32];
  char wifi_ssid[32];
  char wifi_pass[32];
  char mqtt_server[32];
  char mqtt_user[32];
  char mqtt_pass[32];
  char mqtt_topic[32];
  char ota_pass[24];
  int mqtt_port;
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
  ota_setup();
  mqtt_setup();
  
  server.on("/", handleRoot);
  server.on("/settings", handleSettings);
  server.on("/save", handleSave);
  server.on("/restart", handleRestart);
  server.on("/wifi.json", handleNetworks);
  server.on("/status.json", handleStatus);
  server.begin();
  Serial.println("HTTP server started.");
}

void loop() {
  server.handleClient();
  ArduinoOTA.handle();

  if (!mqtt.connected()) {
    mqtt_setup();
  }

  if (WiFi.status() != WL_CONNECTED) {
    set_led(255, 0, 0);
  } else if (!mqtt.connected()) {
    set_led(255, 255, 0);
  } else {
    set_led(0, 32, 0);
  }

  pollDHT();
  pollPIR();
  pollLDR();

  delay(1000);
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

void mqtt_setup() {
  if (connected == 0) {
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
      if (mqtt.connect("sensor")) {
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
  "\"temperature\": " + tempValue + ", "
  "\"humidity\": " + humValue + ", "
  "\"motion\": " + pirValue + ", "
  "\"lux\": " + ldrValue + ", "
  "\"time\": " + millis() + ""
  "}";

  char buffer[128];

  document.toCharArray(buffer, 128);
  Serial.println(document);
  Serial.print("Publishing to MQTT topic: ");
  Serial.println(cfg.mqtt_topic);
  mqtt.publish(cfg.mqtt_topic, buffer, true);
}

void mqttHandler (char* topic, byte* payload, unsigned int length) {
  Serial.println(topic);
}

void wifi_setup() {
  /* Try to connect to the Wifi */
  WiFi.mode(WIFI_STA);
  
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

  if (WiFi.status() != WL_CONNECTED) {
    WiFi.mode(WIFI_AP);
    WiFi.macAddress(mac);
    String ssid_str = "abode_" + String(mac[3], HEX) + String(mac[4], HEX) + String(mac[5], HEX);
    ssid_str.toCharArray(ssid, 12);
    Serial.println("Configuring access point..." + ssid_str);
    WiFi.softAP(ssid, password);
    
    IPAddress myIP = WiFi.softAPIP();
    Serial.print("AP IP address: ");
    Serial.println(myIP);
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
}

void pollDHT() {

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
      humLast = tempValue;
      
      Serial.print("Polled DHT: t:");
      Serial.print(tempValue);
      Serial.print(" h:");
      Serial.println(humValue);
    }
  }

  /* Handle rollover */
  if (millis() < lastDHTPoll) {
    lastDHTPoll = millis();
  }
  
}

void pollPIR() {

  if (millis() - lastPIRPoll > PIRPollInterval) {
    
    pirValue = digitalRead(PIRPIN);
    lastPIRPoll = millis();

    if (pirValue != pirLast) {
      mqttPublish();
      pirLast = pirValue;
      
      Serial.print("Polled PIR: ");
      Serial.println(pirValue);
    }
  }

  /* Handle rollover */
  if (millis() < lastPIRPoll) {
    lastPIRPoll = millis();
  }
}

void pollLDR() {

  if (millis() - lastLDRPoll > PLDRPollInterval) {
    
    ldrValue = analogRead(LDRPIN);
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
  "width: 400px;"
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
"</style>";

void handleRoot() {
  String document = ""
  "<html>" + styles + ""
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
  "<html>" + styles + ""
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
          "document.forms.config.ssid.value=n.n;\n"
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
  "<h4>General</h4>"
  "<div><input type='input' placeholder='Name' name='name' value='" + cfg.name + "'></div>"
  "<div><input type='input' placeholder='OTA Password' name='ota_password' value='" + cfg.ota_pass + "'></div>"
  "<h4>MQTT Settings</h4>"
  "<div><input type='input' placeholder='MQTT Server' name='mqtt_server' value='" + cfg.mqtt_server + "'></div>"
  "<div><input type='input' placeholder='MQTT User' name='mqtt_user' value='" + cfg.mqtt_user + "'></div>"
  "<div><input type='input' placeholder='MQTT Password' name='mqtt_password'></div>"
  "<div><input type='input' placeholder='MQTT Topic' name='mqtt_topic' value='" + cfg.mqtt_topic + "'></div>"
  "<div><input type='input' placeholder='MQTT Port' name='mqtt_port' value='" + cfg.mqtt_port + "'></div>"
  "<h4>WiFi Settings</h4>"
  "<div>&nbsp;</div>"
  "<button onclick='getNetworks()'>Scan Networks</button>"
  "<div>&nbsp;</div>"
  "<div id='i'></div>"
  "<div id='e'></div>"
  "<div>"
    "<ul id='networks'></ul>"
  "</div>"
  "<div>&nbsp;</div>"
  "<div><input type='input' placeholder='SSID' name='wifi_ssid' value='" + cfg.wifi_ssid + "'></div>"
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
  "<html>" + styles + ""
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

  if (server.hasArg("name")) {
    changed = 1;
    Serial.println("Saving Sensor Name");
    char name_char[32];
    server.arg("name").toCharArray(name_char, 32);
    strcpy(cfg.name, name_char);
  }

  if (server.hasArg("ota_password") && server.arg("ota_password") != "") {
    changed = 1;
    Serial.println("Saving OTA Password");
    char ota_pass_char[24];
    server.arg("ota_password").toCharArray(ota_pass_char, 24);
    strcpy(cfg.ota_pass, ota_pass_char);
  }

  if (server.hasArg("mqtt_server")) {
    changed = 1;
    Serial.println("Saving MQTT Server");
    char mqtt_server_char[32];
    server.arg("mqtt_server").toCharArray(mqtt_server_char, 32);
    strcpy(cfg.mqtt_server, mqtt_server_char);
  }

  if (server.hasArg("mqtt_user")) {
    changed = 1;
    Serial.println("Saving MQTT User");
    char mqtt_user_char[32];
    server.arg("mqtt_user").toCharArray(mqtt_user_char, 32);
    strcpy(cfg.mqtt_user, mqtt_user_char);
  }

  if (server.hasArg("mqtt_password") && server.arg("mqtt_password") != "") {
    changed = 1;
    Serial.println("Saving MQTT Password");
    char mqtt_password_char[32];
    server.arg("mqtt_password").toCharArray(mqtt_password_char, 32);
    strcpy(cfg.mqtt_pass, mqtt_password_char);
  }

  if (server.hasArg("mqtt_port")) {
    changed = 1;
    Serial.println("Saving MQTT Port");
    cfg.mqtt_port = server.arg("mqtt_port").toInt();
  }

  if (server.hasArg("mqtt_topic")) {
    changed = 1;
    Serial.println("Saving MQTT Topic");
    char mqtt_topic_char[32];
    server.arg("mqtt_topic").toCharArray(mqtt_topic_char, 32);
    strcpy(cfg.mqtt_topic, mqtt_topic_char);
  }

  if (server.hasArg("wifi_ssid")) {
    changed = 1;
    Serial.println("Saving Wifi SSID");
    char wifi_ssid_char[32];
    server.arg("wifi_ssid").toCharArray(wifi_ssid_char, 32);
    strcpy(cfg.wifi_ssid, wifi_ssid_char);
  }

  if (server.hasArg("wifi_password") && server.arg("wifi_password") != "") {
    changed = 1;
    Serial.println("Saving WIFI Password");
    char wifi_password_char[32];
    server.arg("wifi_password").toCharArray(wifi_password_char, 32);
    strcpy(cfg.wifi_pass, wifi_password_char);
  }

  if (changed == 0) {
    server.send(200, "text/html", "Nothing changed");
    return;
  }

  Serial.println("Saving configuration");
  EEPROM.begin(256);
  EEPROM.put(0, cfg);
  EEPROM.commit();
  delay(1000);
  
  String document = ""
  "<html>" + styles + ""
  "<body>"
  "<div>Applying configuration. Device will reboot and attempt to connect to wifi.</div>"
  "</body>"
  "</html>";
  
  server.send(200, "text/html", document);
  Serial.println("Restarting...");
  delay(2000);

  WiFi.forceSleepBegin(); wdt_reset(); ESP.restart(); while(1)wdt_reset();
}

void handleRestart() {
  String document = ""
  "<html>" + styles + ""
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
  "\"lux\": " + ldrValue + ""
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




