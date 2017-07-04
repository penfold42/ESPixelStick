/*
* ESPixelStick.ino
*
* Project: ESPixelStick - An ESP8266 and E1.31 based pixel driver
* Copyright (c) 2016 Shelby Merrick
* http://www.forkineye.com
*
*  This program is provided free for you to use in any way that you wish,
*  subject to the laws and regulations where you are using it.  Due diligence
*  is strongly suggested before using this code.  Please give credit where due.
*
*  The Author makes no warranty of any kind, express or implied, with regard
*  to this program or the documentation contained in this document.  The
*  Author shall not be liable in any event for incidental or consequential
*  damages in connection with, or arising out of, the furnishing, performance
*  or use of these programs.
*
*/


/*****************************************/
/*        BEGIN - Configuration          */
/*****************************************/

/* Output Mode - There can be only one! (-Conor MacLeod) */
/* NOW MOVED TO ESPixelStick.h */
//#define ESPS_MODE_PIXEL
//#define ESPS_MODE_SERIAL

/* Fallback configuration if config.json is empty or fails */
const char ssid[] = "blaster";
const char passphrase[] = "omgthisismywirelesskeyhaha";

/*****************************************/
/*         END - Configuration           */
/*****************************************/

//#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <Ticker.h>
#include <AsyncMqttClient.h>
#include <ESP8266mDNS.h>
#include <ESPAsyncTCP.h>
#include <ESPAsyncUDP.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Hash.h>
#include <SPI.h>
#include <E131Async.h>
#include "ESPixelStick.h"
#include "udpraw.h"
#include "EFUpdate.h"
#include "wshandler.h"

extern "C" {
#include <user_interface.h>
}

// MQTT Sub topics
// state
const char MQTT_LIGHT_STATE_TOPIC[] = "/light/status";
const char MQTT_LIGHT_COMMAND_TOPIC[] = "/light/switch";

// brightness
const char MQTT_LIGHT_BRIGHTNESS_STATE_TOPIC[] = "/brightness/status";
const char MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC[] = "/brightness/set";

// colors (rgb)
const char MQTT_LIGHT_RGB_STATE_TOPIC[] = "/rgb/status";
const char MQTT_LIGHT_RGB_COMMAND_TOPIC[] = "/rgb/set";

// payloads by default (on/off)
const char LIGHT_ON[] = "ON";
const char LIGHT_OFF[] = "OFF";

// variables used to store the state, the brightness and the color of the light
boolean m_rgb_state = false;
uint8_t m_rgb_brightness = 100;
uint8_t m_rgb_red = 255;
uint8_t m_rgb_green = 255;
uint8_t m_rgb_blue = 255;

// buffer used to send/receive data with MQTT
const uint8_t MSG_BUFFER_SIZE = 20;
char m_msg_buffer[MSG_BUFFER_SIZE]; 





uint8_t             *seqTracker;        /* Current sequence numbers for each Universe */
uint32_t            lastUpdate;         /* Update timeout tracker */

/* Globals */
E131Async       e131(10);       /* E131Async with X buffers */
testing_t       testing;
config_t        config;
uint32_t        *seqError;      /* Sequence error tracking for each universe */
uint16_t        uniLast = 1;    /* Last Universe to listen for */
bool            reboot = false; /* Reboot flag */
AsyncWebServer  web(HTTP_PORT); /* Web Server */
AsyncWebSocket  ws("/ws");      /* Web Socket Plugin */

/* Output Drivers */
#if defined(ESPS_MODE_PIXEL)
#include "PixelDriver.h"
PixelDriver     pixels;         /* Pixel object */
#elif defined(ESPS_MODE_SERIAL)
#include "SerialDriver.h"
SerialDriver    serial;         /* Serial object */
#else
#error "No valid output mode defined."
#endif

/* Forward Declarations */
void loadConfig();
void initWifi();
void initWeb();
void updateConfig();

WiFiEventHandler wifiConnectHandler;
WiFiEventHandler wifiDisconnectHandler;
Ticker wifiTicker;

AsyncMqttClient mqtt;
Ticker mqttTicker;

RF_PRE_INIT() {
    //wifi_set_phy_mode(PHY_MODE_11G);    // Force 802.11g mode
    system_phy_set_powerup_option(31);  // Do full RF calibration on power-up
    system_phy_set_max_tpw(82);         // Set max TX power
}

void setup() {
    // Configure SDK params
    wifi_set_sleep_type(NONE_SLEEP_T);

    // Initial pin states
    pinMode(DATA_PIN, OUTPUT);
    digitalWrite(DATA_PIN, LOW);

    // Setup serial log port
    LOG_PORT.begin(115200);
    delay(10);

    // Enable SPIFFS
    SPIFFS.begin();

    LOG_PORT.println("");
    LOG_PORT.print(F("ESPixelStick v"));
    for (uint8_t i = 0; i < strlen_P(VERSION); i++)
        LOG_PORT.print((char)(pgm_read_byte(VERSION + i)));
    LOG_PORT.println("");

    // Load configuration from SPIFFS and set Hostname
    loadConfig();
    WiFi.hostname(config.hostname);
    config.testmode = TestMode::DISABLED;

    // Setup WiFi Handlers
    wifiConnectHandler = WiFi.onStationModeGotIP(onWifiConnect);
    wifiDisconnectHandler = WiFi.onStationModeDisconnected(onWiFiDisconnect);

    // Setup MQTT Handlers
    if (config.mqtt) {
        mqtt.onConnect(onMqttConnect);
        mqtt.onDisconnect(onMqttDisconnect);
        mqtt.onMessage(onMqttMessage);
        mqtt.setServer(config.mqtt_ip.c_str(), config.mqtt_port);
        if (config.mqtt_user.length() > 0)
            mqtt.setCredentials(config.mqtt_user.c_str(), config.mqtt_password.c_str());
    }

    // Fallback to default SSID and passphrase if we fail to connect
    initWifi();
    if (WiFi.status() != WL_CONNECTED) {
        LOG_PORT.println(F("*** Timeout - Reverting to default SSID ***"));
        config.ssid = ssid;
        config.passphrase = passphrase;
        initWifi();
    }

    // If we fail again, go SoftAP or reboot
    if (WiFi.status() != WL_CONNECTED) {
        if (config.ap_fallback) {
            LOG_PORT.println(F("**** FAILED TO ASSOCIATE WITH AP, GOING SOFTAP ****"));
            WiFi.mode(WIFI_AP);
            String ssid = "ESPixelStick " + String(config.hostname);
            WiFi.softAP(ssid.c_str());
        } else {
            LOG_PORT.println(F("**** FAILED TO ASSOCIATE WITH AP, REBOOTING ****"));
            ESP.restart();
        }
    }

    // Configure and start the web server
    initWeb();

    // Setup E1.31
    if (config.multicast)
        e131.begin(E131_MULTICAST, config.universe,
                uniLast - config.universe + 1);
    else
        e131.begin(E131_UNICAST);

    // Configure the outputs
#if defined (ESPS_MODE_PIXEL)
    pixels.setPin(DATA_PIN);
    updateConfig();
    pixels.show();
#else
    updateConfig();
#endif
}

/////////////////////////////////////////////////////////
// 
//  WiFi Section
//
/////////////////////////////////////////////////////////

void initWifi() {
    // Switch to station mode and disconnect just in case
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    connectWifi();
    uint32_t timeout = millis();
    while (WiFi.status() != WL_CONNECTED) {
        LOG_PORT.print(".");
        delay(500);
        if (millis() - timeout > CONNECT_TIMEOUT) {
            LOG_PORT.println("");
            LOG_PORT.println(F("*** Failed to connect ***"));
            break;
        }
    }
}

void connectWifi() {
    delay(secureRandom(100,500));

    LOG_PORT.println("");
    LOG_PORT.print(F("Connecting to "));
    LOG_PORT.print(config.ssid);
    LOG_PORT.print(F(" as "));
    LOG_PORT.println(config.hostname);

    WiFi.begin(config.ssid.c_str(), config.passphrase.c_str());
    if (config.dhcp) {
        LOG_PORT.print(F("Connecting with DHCP"));
    } else {
        // We don't use DNS, so just set it to our gateway
        WiFi.config(IPAddress(config.ip[0], config.ip[1], config.ip[2], config.ip[3]),
                    IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3]),
                    IPAddress(config.netmask[0], config.netmask[1], config.netmask[2], config.netmask[3]),
                    IPAddress(config.gateway[0], config.gateway[1], config.gateway[2], config.gateway[3])
        );
        LOG_PORT.print(F("Connecting with Static IP"));
    }
}

void onWifiConnect(const WiFiEventStationModeGotIP &event) {
    LOG_PORT.println("");
    LOG_PORT.print(F("Connected with IP: "));
    LOG_PORT.println(WiFi.localIP());

    // Setup MQTT connection if enabled
    if (config.mqtt)
        connectToMqtt();

    // Setup mDNS / DNS-SD
    //TODO: Reboot or restart mdns when config.id is changed?
     char chipId[7] = { 0 };
    snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
    MDNS.setInstanceName(config.id + " (" + String(chipId) + ")");
    if (MDNS.begin(config.hostname.c_str())) {
        MDNS.addService("http", "tcp", HTTP_PORT);
        MDNS.addService("e131", "udp", E131_DEFAULT_PORT);
        MDNS.addServiceTxt("e131", "udp", "TxtVers", String(RDMNET_DNSSD_TXTVERS));
        MDNS.addServiceTxt("e131", "udp", "ConfScope", RDMNET_DEFAULT_SCOPE);
        MDNS.addServiceTxt("e131", "udp", "E133Vers", String(RDMNET_DNSSD_E133VERS));
        MDNS.addServiceTxt("e131", "udp", "CID", String(chipId));
        MDNS.addServiceTxt("e131", "udp", "Model", "ESPixelStick");
        MDNS.addServiceTxt("e131", "udp", "Manuf", "Forkineye");
    } else {
        LOG_PORT.println(F("*** Error setting up mDNS responder ***"));
    }
}

void onWiFiDisconnect(const WiFiEventStationModeDisconnected &event) {
    LOG_PORT.println(F("*** WiFi Disconnected ***"));

    // Pause MQTT reconnect while WiFi is reconnecting
    mqttTicker.detach();
    wifiTicker.once(2, connectWifi);
}

/////////////////////////////////////////////////////////
// 
//  MQTT Section
//
/////////////////////////////////////////////////////////

void connectToMqtt() {
    LOG_PORT.print(F("- Connecting to MQTT Broker "));
    LOG_PORT.println(config.mqtt_ip);
    mqtt.connect();
}

void onMqttConnect(bool sessionPresent) {
    LOG_PORT.println(F("- MQTT Connected"));

    // Setup subscriptions
    mqtt.subscribe(String(config.mqtt_topic + MQTT_LIGHT_COMMAND_TOPIC).c_str(), 0);
    mqtt.subscribe(String(config.mqtt_topic + MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC).c_str(), 0);
    mqtt.subscribe(String(config.mqtt_topic + MQTT_LIGHT_RGB_COMMAND_TOPIC).c_str(), 0);

    // Publish state
    publishRGBState();
    publishRGBBrightness();
    publishRGBColor();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    LOG_PORT.println(F("*** MQTT Disconnected ***"));
    if (WiFi.isConnected()) {
        mqttTicker.once(2, connectToMqtt);
    }
}

void onMqttMessage(char* topic, char* p_payload,
        AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {

    String payload;
    for (uint8_t i = 0; i < len; i++)
        payload.concat((char)p_payload[i]);
/*
Serial.print("Topic: ");
Serial.print(topic);
Serial.print(" | Payload: ");
Serial.println(payload);
*/
    // handle message topic
    if (String(config.mqtt_topic + MQTT_LIGHT_COMMAND_TOPIC).equals(topic)) {
    // test if the payload is equal to "ON" or "OFF"
        if (payload.equals(String(LIGHT_ON))) {
            config.testmode = TestMode::MQTT;
            if (m_rgb_state != true) {
                m_rgb_state = true;
                setStatic(m_rgb_red, m_rgb_green, m_rgb_blue);
                publishRGBState();
            }
        } else if (payload.equals(String(LIGHT_OFF))) {
            config.testmode = TestMode::DISABLED;
            if (m_rgb_state != false) {
                m_rgb_state = false;
                setStatic(0, 0, 0);
                publishRGBState();
            }
        }
    } else if (String(config.mqtt_topic + MQTT_LIGHT_BRIGHTNESS_COMMAND_TOPIC).equals(topic)) {
        uint8_t brightness = payload.toInt();
        if (brightness < 0 || brightness > 100) {
            return;
        } else {
            m_rgb_brightness = brightness;
            setStatic(m_rgb_red, m_rgb_green, m_rgb_blue);
            publishRGBBrightness();
        }
    } else if (String(config.mqtt_topic + MQTT_LIGHT_RGB_COMMAND_TOPIC).equals(topic)) {
        // get the position of the first and second commas
        uint8_t firstIndex = payload.indexOf(',');
        uint8_t lastIndex = payload.lastIndexOf(',');
    
        uint8_t rgb_red = payload.substring(0, firstIndex).toInt();
        if (rgb_red < 0 || rgb_red > 255)
            return;
        else
            m_rgb_red = rgb_red;
            
        uint8_t rgb_green = payload.substring(firstIndex + 1, lastIndex).toInt();
        if (rgb_green < 0 || rgb_green > 255)
            return;
        else
            m_rgb_green = rgb_green;
    
        uint8_t rgb_blue = payload.substring(lastIndex + 1).toInt();
        if (rgb_blue < 0 || rgb_blue > 255)
            return;
        else
            m_rgb_blue = rgb_blue;
   
        setStatic(m_rgb_red, m_rgb_green, m_rgb_blue);
        publishRGBColor();
    }
}

// function called to publish the state of the led (on/off)
void publishRGBState() {
    if (m_rgb_state) {
        mqtt.publish(String(config.mqtt_topic + MQTT_LIGHT_STATE_TOPIC).c_str(),
                0, true, LIGHT_ON);
    } else {
        mqtt.publish(String(config.mqtt_topic + MQTT_LIGHT_STATE_TOPIC).c_str(),
                0, true, LIGHT_OFF);
    }
}

// function called to publish the brightness of the led (0-100)
void publishRGBBrightness() {
    snprintf(m_msg_buffer, MSG_BUFFER_SIZE, "%d", m_rgb_brightness);
    mqtt.publish(String(config.mqtt_topic + MQTT_LIGHT_BRIGHTNESS_STATE_TOPIC).c_str(),
            0, true, m_msg_buffer);
}

// function called to publish the colors of the led (xx(x),xx(x),xx(x))
void publishRGBColor() {
    snprintf(m_msg_buffer, MSG_BUFFER_SIZE, "%d,%d,%d", m_rgb_red, m_rgb_green, m_rgb_blue);
    mqtt.publish(String(config.mqtt_topic + MQTT_LIGHT_RGB_STATE_TOPIC).c_str(),
            0, true, m_msg_buffer);
}


/////////////////////////////////////////////////////////
// 
//  Web Section
//
/////////////////////////////////////////////////////////

/* Configure and start the web server */
void initWeb() {
    /* Handle OTA update from asynchronous callbacks */
    Update.runAsync(true);

    /* Setup WebSockets */
    ws.onEvent(wsEvent);
    web.addHandler(&ws);

    /* Heap status handler */
    web.on("/heap", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(200, "text/plain", String(ESP.getFreeHeap()));
    });

    /* JSON Config Handler */
    web.on("/conf", HTTP_GET, [](AsyncWebServerRequest *request) {
        String jsonString;
        serializeConfig(jsonString);
        request->send(200, "text/json", jsonString);
    });

    /* Firmware upload handler */
    web.on("/updatefw", HTTP_POST, [](AsyncWebServerRequest *request) {
        ws.textAll("X6");
    }, handle_fw_upload);

    /* Static Handler */
    web.serveStatic("/", SPIFFS, "/www/").setDefaultFile("index.html");

    web.onNotFound([](AsyncWebServerRequest *request) {
        request->send(404, "text/plain", "Page not found");
    });

    web.begin();

    LOG_PORT.print(F("- Web Server started on port "));
    LOG_PORT.println(HTTP_PORT);
}

/////////////////////////////////////////////////////////
// 
//  JSON / Configuration Section
//
/////////////////////////////////////////////////////////

// Configuration Validations
void validateConfig() {
    // E1.31 Limits
    if (config.universe < 1)
        config.universe = 1;

    if (config.channel_start < 1)
        config.channel_start = 1;
    else if (config.channel_start > UNIVERSE_LIMIT)
        config.channel_start = UNIVERSE_LIMIT;

    // Set default MQTT port if missing
    if (config.mqtt_port == 0)
        config.mqtt_port = MQTT_PORT;

    // Generate default MQTT topic if needed
    if (!config.mqtt_topic.length()) {
        char chipId[7] = { 0 };
        snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
        config.mqtt_topic = "diy/esps/" + String(chipId);
    }


#if defined(ESPS_MODE_PIXEL)
    // Set Mode
    config.devmode = DevMode::MPIXEL;

    // Generic channel limits for pixels
    if (config.channel_count % 3)
        config.channel_count = (config.channel_count / 3) * 3;

    if (config.channel_count > PIXEL_LIMIT * 3)
        config.channel_count = PIXEL_LIMIT * 3;
    else if (config.channel_count < 1)
        config.channel_count = 1;

    // GECE Limits
    if (config.pixel_type == PixelType::GECE) {
        config.pixel_color = PixelColor::RGB;
        if (config.channel_count > 63 * 3)
            config.channel_count = 63 * 3;
    }

#elif defined(ESPS_MODE_SERIAL)
    // Set Mode
    config.devmode = DevMode::MSERIAL;

    // Generic serial channel limits
    if (config.channel_count > RENARD_LIMIT)
        config.channel_count = RENARD_LIMIT;
    else if (config.channel_count < 1)
        config.channel_count = 1;

    if (config.serial_type == SerialType::DMX512 && config.channel_count > UNIVERSE_LIMIT)
        config.channel_count = UNIVERSE_LIMIT;

    // Baud rate check
    if (config.baudrate > BaudRate::BR_460800)
        config.baudrate = BaudRate::BR_460800;
    else if (config.baudrate < BaudRate::BR_38400)
        config.baudrate = BaudRate::BR_57600;
#endif
}

void updateConfig() {
    /* Validate first */
    validateConfig();

    /* Find the last universe we should listen for */
    uint16_t span = config.channel_start + config.channel_count - 1;
    if (span % UNIVERSE_LIMIT)
        uniLast = config.universe + span / UNIVERSE_LIMIT;
    else
        uniLast = config.universe + span / UNIVERSE_LIMIT - 1;

    /* Setup the sequence error tracker */
    uint8_t uniTotal = (uniLast + 1) - config.universe;

    if (seqTracker) free(seqTracker);
    if (seqTracker = static_cast<uint8_t *>(malloc(uniTotal)))
        memset(seqTracker, 0x00, uniTotal);

    if (seqError) free(seqError);
    if (seqError = static_cast<uint32_t *>(malloc(uniTotal * 4)))
        memset(seqError, 0x00, uniTotal * 4);

    /* Zero out packet stats */
    e131.stats.num_packets = 0;

    /* Initialize for our pixel type */
#if defined(ESPS_MODE_PIXEL)
    pixels.begin(config.pixel_type, config.pixel_color, config.channel_count / 3);
    pixels.setGamma(config.gamma);
#elif defined(ESPS_MODE_SERIAL)
    serial.begin(&SEROUT_PORT, config.serial_type, config.channel_count, config.baudrate);
#endif
    LOG_PORT.print(F("- Listening for "));
    LOG_PORT.print(config.channel_count);
    LOG_PORT.print(F(" channels, from Universe "));
    LOG_PORT.print(config.universe);
    LOG_PORT.print(F(" to "));
    LOG_PORT.println(uniLast);
}

// De-Serialize Network config
void dsNetworkConfig(JsonObject &json) {
    // Fallback to embedded ssid and passphrase if null in config
    config.ssid = json["network"]["ssid"].as<String>();
    if (!config.ssid.length())
        config.ssid = ssid;

    config.passphrase = json["network"]["passphrase"].as<String>();
    if (!config.passphrase.length())
        config.passphrase = passphrase;

    // Network
    for (int i = 0; i < 4; i++) {
        config.ip[i] = json["network"]["ip"][i];
        config.netmask[i] = json["network"]["netmask"][i];
        config.gateway[i] = json["network"]["gateway"][i];
    }
    config.dhcp = json["network"]["dhcp"];
    config.ap_fallback = json["network"]["ap_fallback"];

    // Generate default hostname if needed
    config.hostname = json["network"]["hostname"].as<String>();
    if (!config.hostname.length()) {
        char chipId[7] = { 0 };
        snprintf(chipId, sizeof(chipId), "%06x", ESP.getChipId());
        config.hostname = "esps-" + String(chipId);
    }
}

// De-serialize Device Config
void dsDeviceConfig(JsonObject &json) {
    // Device
    config.id = json["device"]["id"].as<String>();

    // E131
    config.universe = json["e131"]["universe"];
    config.channel_start = json["e131"]["channel_start"];
    config.channel_count = json["e131"]["channel_count"];
    config.multicast = json["e131"]["multicast"];

    // MQTT
    config.mqtt = json["mqtt"]["enabled"];
    config.mqtt_ip = json["mqtt"]["ip"].as<String>();
    config.mqtt_port = json["mqtt"]["port"];
    config.mqtt_user = json["mqtt"]["user"].as<String>();
    config.mqtt_password = json["mqtt"]["password"].as<String>();
    config.mqtt_topic = json["mqtt"]["topic"].as<String>();

#if defined(ESPS_MODE_PIXEL)
    /* Pixel */
    config.pixel_type = PixelType(static_cast<uint8_t>(json["pixel"]["type"]));
    config.pixel_color = PixelColor(static_cast<uint8_t>(json["pixel"]["color"]));
    config.gamma = json["pixel"]["gamma"];

#elif defined(ESPS_MODE_SERIAL)
    /* Serial */
    config.serial_type = SerialType(static_cast<uint8_t>(json["serial"]["type"]));
    config.baudrate = BaudRate(static_cast<uint32_t>(json["serial"]["baudrate"]));
#endif
}

/* Load configugration JSON file */
void loadConfig() {
    /* Zeroize Config struct */
    memset(&config, 0, sizeof(config));

    /* Load CONFIG_FILE json. Create and init with defaults if not found */
    File file = SPIFFS.open(CONFIG_FILE, "r");
    if (!file) {
        LOG_PORT.println(F("- No configuration file found."));
        config.ssid = ssid;
        config.passphrase = passphrase;
        saveConfig();
    } else {
        /* Parse CONFIG_FILE json */
        size_t size = file.size();
        if (size > CONFIG_MAX_SIZE) {
            LOG_PORT.println(F("*** Configuration File too large ***"));
            return;
        }

        std::unique_ptr<char[]> buf(new char[size]);
        file.readBytes(buf.get(), size);

        DynamicJsonBuffer jsonBuffer;
        JsonObject &json = jsonBuffer.parseObject(buf.get());
        if (!json.success()) {
            LOG_PORT.println(F("*** Configuration File Format Error ***"));
            return;
        }

        dsNetworkConfig(json);
        dsDeviceConfig(json);

        LOG_PORT.println(F("- Configuration loaded."));
    }

    /* Validate it */
    validateConfig();
}

// Serialize the current config into a JSON string
void serializeConfig(String &jsonString, bool pretty, bool creds) {
    // Create buffer and root object
    DynamicJsonBuffer jsonBuffer;
    JsonObject &json = jsonBuffer.createObject();

    // Device
    JsonObject &device = json.createNestedObject("device");
    device["id"] = config.id.c_str();
    device["mode"] = static_cast<uint8_t>(config.devmode);

    // Network
    JsonObject &network = json.createNestedObject("network");
    network["ssid"] = config.ssid.c_str();
    if (creds)
        network["passphrase"] = config.passphrase.c_str();
    network["hostname"] = config.hostname.c_str();
    JsonArray &ip = network.createNestedArray("ip");
    JsonArray &netmask = network.createNestedArray("netmask");
    JsonArray &gateway = network.createNestedArray("gateway");
    for (int i = 0; i < 4; i++) {
        ip.add(config.ip[i]);
        netmask.add(config.netmask[i]);
        gateway.add(config.gateway[i]);
    }
    network["dhcp"] = config.dhcp;
    network["ap_fallback"] = config.ap_fallback;

    // MQTT
    JsonObject &_mqtt = json.createNestedObject("mqtt");
    _mqtt["enabled"] = config.mqtt;
    _mqtt["ip"] = config.mqtt_ip.c_str();
    _mqtt["port"] = config.mqtt_port;
    _mqtt["user"] = config.mqtt_user.c_str();
    _mqtt["password"] = config.mqtt_password.c_str();
    _mqtt["topic"] = config.mqtt_topic.c_str();

    // E131
    JsonObject &e131 = json.createNestedObject("e131");
    e131["universe"] = config.universe;
    e131["channel_start"] = config.channel_start;
    e131["channel_count"] = config.channel_count;
    e131["multicast"] = config.multicast;

#if defined(ESPS_MODE_PIXEL)
    // Pixel
    JsonObject &pixel = json.createNestedObject("pixel");
    pixel["type"] = static_cast<uint8_t>(config.pixel_type);
    pixel["color"] = static_cast<uint8_t>(config.pixel_color);
    pixel["gamma"] = config.gamma;

#elif defined(ESPS_MODE_SERIAL)
    // Serial
    JsonObject &serial = json.createNestedObject("serial");
    serial["type"] = static_cast<uint8_t>(config.serial_type);
    serial["baudrate"] = static_cast<uint32_t>(config.baudrate);
#endif

    if (pretty)
        json.prettyPrintTo(jsonString);
    else
        json.printTo(jsonString);
}

/* Save configuration JSON file */
void saveConfig() {
    /* Update Config */
    updateConfig();

    /* Serialize Config */
    String jsonString;
    serializeConfig(jsonString, true, true);

    /* Save Config */
    File file = SPIFFS.open(CONFIG_FILE, "w");
    if (!file) {
        LOG_PORT.println(F("*** Error creating configuration file ***"));
        return;
    } else {
        file.println(jsonString);
        LOG_PORT.println(F("* Configuration saved."));
    }
}


/////////////////////////////////////////////////////////
// 
//  Set routines for Testing and MQTT
//
/////////////////////////////////////////////////////////

void setStatic(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t i = 0;
    while (i <= config.channel_count - 3) {
#if defined(ESPS_MODE_PIXEL)
        pixels.setValue(i++, r);
        pixels.setValue(i++, g);
        pixels.setValue(i++, b);
#elif defined(ESPS_MODE_SERIAL)
        serial.setValue(i++, r);
        serial.setValue(i++, g);
        serial.setValue(i++, b);
#endif
    }
}


/////////////////////////////////////////////////////////
// 
//  Main Loop
//
/////////////////////////////////////////////////////////
void loop() {
   /* check for raw packets on port 2801 */
    handle_raw_port();

    /* check for rotary encoder and buttons */
    handle_buttons();

    e131_packet_t packet;

    // Reboot handler
    if (reboot) {
        delay(REBOOT_DELAY);
        ESP.restart();
    }

    if (config.testmode == TestMode::DISABLED || config.testmode == TestMode::VIEW_STREAM) {
        // Parse a packet and update pixels
        if (!e131.pbuff->isEmpty(e131.pbuff)) {
            e131.pbuff->pull(e131.pbuff, &packet);
            uint16_t universe = htons(packet.universe);
            uint8_t *data = packet.property_values + 1;
            if ((universe >= config.universe) && (universe <= uniLast)) {
                // Universe offset and sequence tracking
                uint8_t uniOffset = (universe - config.universe);
                if (packet.sequence_number != seqTracker[uniOffset]++) {
                    seqError[uniOffset]++;
                    seqTracker[uniOffset] = packet.sequence_number + 1;
                }

                // Offset the channels if required
                uint16_t offset = 0;
                offset = config.channel_start - 1;

                // Find start of data based off the Universe
                int16_t dataStart = uniOffset * UNIVERSE_LIMIT - offset;

                // Calculate how much data we need for this buffer
                uint16_t dataStop = config.channel_count;
                if ((dataStart + UNIVERSE_LIMIT) < dataStop)
                    dataStop = dataStart + UNIVERSE_LIMIT;

                // Set the data
                uint16_t buffloc = 0;

                // ignore data from start of first Universe before channel_start
                if (dataStart < 0) {
                    dataStart = 0;
                    buffloc = config.channel_start-1;
                }

                for (int i = dataStart; i < dataStop; i++) {
#if defined(ESPS_MODE_PIXEL)
                    pixels.setValue(i, data[buffloc]);
#elif defined(ESPS_MODE_SERIAL)
                    serial.setValue(i, data[buffloc]);
#endif
                    buffloc++;
                }
            }
        }
    } else {  // Other testmodes
        switch (config.testmode) {
            case TestMode::STATIC: {
                setStatic(testing.r, testing.g, testing.b);
                break;
            }

            case TestMode::CHASE:
                // Run chase routine
                if (millis() - testing.last > 100) {
                    // Rime for new step
                    testing.last = millis();
#if defined(ESPS_MODE_PIXEL)
                    // Clear whole string
                    for (int y =0; y < config.channel_count; y++)
                        pixels.setValue(y, 0);
                    // Set pixel at step
                    int ch_offset = testing.step*3;
                    pixels.setValue(ch_offset++, testing.r);
                    pixels.setValue(ch_offset++, testing.g);
                    pixels.setValue(ch_offset, testing.b);
                    testing.step++;
                    if (testing.step >= (config.channel_count/3))
                        testing.step = 0;
#elif defined(ESPS_MODE_SERIAL)
                    for (int y =0; y < config.channel_count; y++)
                        serial.setValue(y, 0);
                    // Set pixel at step
                    serial.setValue(testing.step++, 0xFF);
                    if (testing.step >= config.channel_count)
                        testing.step = 0;
#endif
                }
                break;

            case TestMode::RAINBOW:
                // Run rainbow routine
                if (millis() - testing.last > 50) {
                    testing.last = millis();
                    uint16_t i, WheelPos, num_pixels;
                    num_pixels = config.channel_count / 3;
                    if (testing.step < 255) {
                        for (i=0; i < (num_pixels); i++) {
                            int ch_offset = i*3;
                            WheelPos = 255 - (((i * 256 / num_pixels) + testing.step) & 255);
#if defined(ESPS_MODE_PIXEL)
                            if (WheelPos < 85) {
                                pixels.setValue(ch_offset++, 255 - WheelPos * 3);
                                pixels.setValue(ch_offset++, 0);
                                pixels.setValue(ch_offset, WheelPos * 3);
                            } else if (WheelPos < 170) {
                                WheelPos -= 85;
                                pixels.setValue(ch_offset++, 0);
                                pixels.setValue(ch_offset++, WheelPos * 3);
                                pixels.setValue(ch_offset, 255 - WheelPos * 3);
                            } else {
                                WheelPos -= 170;
                                pixels.setValue(ch_offset++, WheelPos * 3);
                                pixels.setValue(ch_offset++,255 - WheelPos * 3);
                                pixels.setValue(ch_offset, 0);
                            }
#elif defined(ESPS_MODE_SERIAL)
                            if (WheelPos < 85) {
                                serial.setValue(ch_offset++, 255 - WheelPos * 3);
                                serial.setValue(ch_offset++, 0);
                                serial.setValue(ch_offset, WheelPos * 3);
                            } else if (WheelPos < 170) {
                                WheelPos -= 85;
                                serial.setValue(ch_offset++, 0);
                                serial.setValue(ch_offset++, WheelPos * 3);
                                serial.setValue(ch_offset, 255 - WheelPos * 3);
                            } else {
                                WheelPos -= 170;
                                serial.setValue(ch_offset++, WheelPos * 3);
                                serial.setValue(ch_offset++,255 - WheelPos * 3);
                                serial.setValue(ch_offset, 0);
                            }
#endif
                        }
                    } else {
                        testing.step = 0;
                    }
                    testing.step++;
                }
                break;
        }
    }


/* Streaming refresh */
#if defined(ESPS_MODE_PIXEL)
    if (pixels.canRefresh())
        pixels.show();
#elif defined(ESPS_MODE_SERIAL)
    if (serial.canRefresh())
        serial.show();
#endif
}
