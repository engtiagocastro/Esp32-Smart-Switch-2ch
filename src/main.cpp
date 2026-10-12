/**
 * @file main.cpp
 * @brief Firmware Oficial SHX v1.0.1 para Interruptor Smart 2 Canais
 * @author Tiago Cunha (Desenvolvido com Gemini)
 * @version 1.0.1 - Debounce e Reconexão MQTT Aprimorados
 * @date 09 de Agosto de 2025
 */

//=================================================================================
// BIBLIOTECAS
//=================================================================================
#include <Arduino.h>
#include <WiFi.h>
#include <AsyncMqttClient.h>
#include <Preferences.h>
#include <DHT.h>
#include <IRremote.h>
#include "wificonfig.hpp"

//=================================================================================
// DEFINIÇÕES E CONSTANTES
//=================================================================================
#define RELAY_1_PIN 18
#define RELAY_2_PIN 19
#define BUTTON_1_PIN 5
#define BUTTON_2_PIN 23
#define LED_WIFI_PIN 26
#define LED_RELAY_1_PIN 17
#define LED_RELAY_2_PIN 16
#define DHT_PIN 27
#define IR_RECV_PIN 32

#define IR_CODE_RELAY_1 0xEF106E01
#define IR_CODE_RELAY_2 0xEE116E01

#define AVAILABILITY_TOPIC "home/switch/availability"

#define DHT_TYPE DHT11
#define LONG_PRESS_TIME 5000
#define BUTTON_DEBOUNCE 50
#define SENSOR_READ_INTERVAL 30000
#define MQTT_PUBLISH_INTERVAL 30000
#define PROJECT_INTERRUPTOR

//=================================================================================
// OBJETOS E VARIÁVEIS GLOBAIS
//=================================================================================
SetupWiFi wifiConfig;
AsyncMqttClient mqttClient;
Preferences preferences;
DHT dht(DHT_PIN, DHT_TYPE);
TimerHandle_t mqttReconnectTimer, sensorReadTimer;

enum SystemState { CONNECTING_WIFI, CONNECTING_MQTT, OPERATIONAL };
SystemState currentState = CONNECTING_WIFI;

float temperatura = 0.0, umidade = 0.0;
bool relay1State = false, relay2State = false;

// Variáveis para debounce robusto
bool lastReadingB1 = LOW, button1State = LOW;
unsigned long lastDebounceTime1 = 0;
bool lastReadingB2 = LOW, button2State = LOW;
unsigned long lastDebounceTime2 = 0;
unsigned long button1PressTime = 0;
bool button1Held = false;

//=================================================================================
// PROTÓTIPOS DE FUNÇÕES
//=================================================================================
void connectToWifi();
void connectToMqtt();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(AsyncMqttClientDisconnectReason reason);
void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total);
void controlRelay(int relay, bool state, bool publish = true);
void handleButtons();
void handleIR();
void readSensorAndPublish();

//=================================================================================
// SETUP
//=================================================================================
void setup() {
    Serial.begin(115200);
    Serial.println("\n>>> Iniciando SHX Smart Switch v1.0.1 <<<");

    IrReceiver.begin(IR_RECV_PIN, ENABLE_LED_FEEDBACK);

    pinMode(RELAY_1_PIN, OUTPUT); pinMode(RELAY_2_PIN, OUTPUT);
    pinMode(LED_RELAY_1_PIN, OUTPUT); pinMode(LED_RELAY_2_PIN, OUTPUT);
    pinMode(LED_WIFI_PIN, OUTPUT); pinMode(BUTTON_1_PIN, INPUT);
    pinMode(BUTTON_2_PIN, INPUT);
    digitalWrite(LED_WIFI_PIN, LOW);

    preferences.begin("relay_states", true);
    relay1State = preferences.getBool("relay1", false);
    relay2State = preferences.getBool("relay2", false);
    preferences.end();
    
    controlRelay(1, relay1State, false);
    controlRelay(2, relay2State, false);

    wifiConfig.carregarConfiguracoes();
    dht.begin();
    
    mqttClient.setWill(AVAILABILITY_TOPIC, 1, true, "offline");
    mqttClient.onConnect(onMqttConnect);
    mqttClient.onDisconnect(onMqttDisconnect);
    mqttClient.onMessage(onMqttMessage);
    
    mqttReconnectTimer = xTimerCreate("mqttTimer", pdMS_TO_TICKS(5000), pdFALSE, (void*)0, [](TimerHandle_t xTimer){ connectToMqtt(); });
    sensorReadTimer = xTimerCreate("sensorTimer", pdMS_TO_TICKS(SENSOR_READ_INTERVAL), pdTRUE, (void*)0, [](TimerHandle_t xTimer){ readSensorAndPublish(); });
    
    connectToWifi();
}

//=================================================================================
// LOOP PRINCIPAL
//=================================================================================
void loop() {
    handleButtons();
    handleIR();

    if(currentState == CONNECTING_WIFI) {
        digitalWrite(LED_WIFI_PIN, (millis() % 1000 < 500));
    } else if (currentState == CONNECTING_MQTT) {
        digitalWrite(LED_WIFI_PIN, (millis() % 2000 < 1000));
    }
}

//=================================================================================
// FUNÇÕES DE CONTROLE (BOTÕES, IR, RELÉ)
//=================================================================================
void handleIR() {
    if (IrReceiver.decode()) {
        unsigned long irValue = IrReceiver.decodedIRData.decodedRawData;
        if (irValue != 0) {
            switch (irValue) {
                case IR_CODE_RELAY_1: controlRelay(1, !relay1State); break;
                case IR_CODE_RELAY_2: controlRelay(2, !relay2State); break;
            }
        }
        IrReceiver.resume();
    }
}


void handleButtons() {
    unsigned long currentMillis = millis();

    bool reading1 = (digitalRead(BUTTON_1_PIN) == HIGH);
    if (reading1 != lastReadingB1) {
        lastDebounceTime1 = currentMillis;
        Serial.println("[B1] INFO: Mudanca detectada, resetando debounce timer.");
    }

    if ((currentMillis - lastDebounceTime1) > BUTTON_DEBOUNCE) {
        if (reading1 != button1State) {
            Serial.printf("[B1] LOG: Estado estabilizado e alterado! Novo estado: %s\n", reading1 ? "HIGH" : "LOW");
            button1State = reading1;
            if (button1State == HIGH) {
                Serial.println("[B1] INFO: Botao PRESSIONADO.");
                button1PressTime = currentMillis;
                button1Held = false;
            } else {
                Serial.println("[B1] INFO: Botao SOLTO.");
                if (!button1Held) {
                    Serial.println("[B1] ACTION: Toque curto! Acionando controlRelay().");
                    controlRelay(1, !relay1State);
                }
            }
        }
    }
    lastReadingB1 = reading1;

    if (button1State == HIGH && !button1Held && (currentMillis - button1PressTime >= LONG_PRESS_TIME)) {
        button1Held = true;
        Serial.println("[B1] ACTION: Toque longo! Iniciando portal de configuracao.");
        wifiConfig.iniciarPortalConfiguracao(true);
    }

    bool reading2 = (digitalRead(BUTTON_2_PIN) == HIGH);
    if (reading2 != lastReadingB2) {
        lastDebounceTime2 = currentMillis;
        Serial.println("[B2] INFO: Mudanca detectada, resetando debounce timer.");
    }

    if ((currentMillis - lastDebounceTime2) > BUTTON_DEBOUNCE) {
        if (reading2 != button2State) {
            Serial.printf("[B2] LOG: Estado estabilizado e alterado! Novo estado: %s\n", reading2 ? "HIGH" : "LOW");
            button2State = reading2;
            if (button2State == HIGH) {
                Serial.println("[B2] ACTION: Botao PRESSIONADO! Acionando controlRelay().");
                controlRelay(2, !relay2State);
            } else {
                Serial.println("[B2] INFO: Botao SOLTO.");
            }
        }
    }
    lastReadingB2 = reading2;
}

void controlRelay(int relay, bool state, bool publish) {
    if (relay == 1) {
        relay1State = state;
        digitalWrite(RELAY_1_PIN, !relay1State);
        digitalWrite(LED_RELAY_1_PIN, relay1State);
    } else if (relay == 2) {
        relay2State = state;
        digitalWrite(RELAY_2_PIN, !relay2State);
        digitalWrite(LED_RELAY_2_PIN, relay2State);
    }
    
    Preferences prefs;
    prefs.begin("relay_states", false);
    prefs.putBool("relay1", relay1State);
    prefs.putBool("relay2", relay2State);
    prefs.end();

    if (publish && mqttClient.connected()) {
        char topic[30];
        sprintf(topic, "home/switch/relay%d/state", relay);
        mqttClient.publish(topic, 1, false, state ? "ON" : "OFF");
    }
}

//=================================================================================
// FUNÇÕES DE CONECTIVIDADE (WIFI, MQTT)
//=================================================================================
void connectToWifi() {
    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){ 
        digitalWrite(LED_WIFI_PIN, HIGH);
        currentState = CONNECTING_MQTT;
        connectToMqtt(); 
    }, ARDUINO_EVENT_WIFI_STA_GOT_IP);

    WiFi.onEvent([](WiFiEvent_t event, WiFiEventInfo_t info){
        digitalWrite(LED_WIFI_PIN, LOW);
        currentState = CONNECTING_WIFI;
        if(mqttClient.connected()) mqttClient.disconnect();
        xTimerStop(sensorReadTimer, 0);
        xTimerStart(mqttReconnectTimer, 0); // Tenta reconectar imediatamente após a queda
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
    
    WiFi.begin();
    currentState = CONNECTING_WIFI;
}

void connectToMqtt() {
  if (WiFi.isConnected() && wifiConfig.mqtt_broker.length() > 0) {
    currentState = CONNECTING_MQTT;
    mqttClient.setServer(wifiConfig.mqtt_broker.c_str(), wifiConfig.mqtt_porta);
    mqttClient.setCredentials(wifiConfig.mqtt_usuario.c_str(), wifiConfig.mqtt_senha.c_str());
    mqttClient.setClientId(("SHX-SmartSwitch-" + String(WiFi.macAddress())).c_str());
    mqttClient.connect();
  }
}

void onMqttConnect(bool sessionPresent) {
    Serial.println("Conectado ao Broker MQTT!");
    currentState = OPERATIONAL;
    digitalWrite(LED_WIFI_PIN, HIGH);
    
    mqttClient.publish(AVAILABILITY_TOPIC, 1, true, "online");
    mqttClient.subscribe("home/switch/relay1/set", 1);
    mqttClient.subscribe("home/switch/relay2/set", 1);

    const char* deviceJson = "{\"ids\":[\"shx_smartswitch\"],\"name\":\"SHX Smart Switch\",\"mf\":\"Eng. Tiago Cunha\",\"mdl\":\"ESP32 2CH\",\"sw\":\"1.0.1\"}";
    char payload[512];

    snprintf(payload, sizeof(payload), "{\"name\":\"Interruptor 1\",\"cmd_t\":\"home/switch/relay1/set\",\"stat_t\":\"home/switch/relay1/state\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"uniq_id\":\"shx_relay1\",\"dev\":%s}", AVAILABILITY_TOPIC, deviceJson);
    mqttClient.publish("homeassistant/switch/shx_smartswitch/relay1/config", 1, true, payload);
    
    snprintf(payload, sizeof(payload), "{\"name\":\"Interruptor 2\",\"cmd_t\":\"home/switch/relay2/set\",\"stat_t\":\"home/switch/relay2/state\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"uniq_id\":\"shx_relay2\",\"dev\":%s}", AVAILABILITY_TOPIC, deviceJson);
    mqttClient.publish("homeassistant/switch/shx_smartswitch/relay2/config", 1, true, payload);
    
    snprintf(payload, sizeof(payload), "{\"name\":\"Temperatura\",\"stat_t\":\"home/switch/temperatura\",\"unit_of_meas\":\"°C\",\"dev_cla\":\"temperature\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"uniq_id\":\"shx_temperature\",\"dev\":%s}", AVAILABILITY_TOPIC, deviceJson);
    mqttClient.publish("homeassistant/sensor/shx_smartswitch/temperature/config", 1, true, payload);

    snprintf(payload, sizeof(payload), "{\"name\":\"Umidade\",\"stat_t\":\"home/switch/umidade\",\"unit_of_meas\":\"%%\",\"dev_cla\":\"humidity\",\"avty_t\":\"%s\",\"pl_avail\":\"online\",\"pl_not_avail\":\"offline\",\"uniq_id\":\"shx_humidity\",\"dev\":%s}", AVAILABILITY_TOPIC, deviceJson);
    mqttClient.publish("homeassistant/sensor/shx_smartswitch/humidity/config", 1, true, payload);
    
    xTimerStart(sensorReadTimer, 0);
    readSensorAndPublish();
}

void onMqttDisconnect(AsyncMqttClientDisconnectReason reason) {
    Serial.println("Desconectado do Broker MQTT.");
    currentState = CONNECTING_MQTT;
    if (WiFi.isConnected()) {
        xTimerStart(mqttReconnectTimer, 0);
    }
}

void onMqttMessage(char* topic, char* payload, AsyncMqttClientMessageProperties properties, size_t len, size_t index, size_t total) {
    String message;
    for (size_t i = 0; i < len; i++) { message += (char)payload[i]; }
    message.toUpperCase();
    if (String(topic) == "home/switch/relay1/set") { controlRelay(1, (message == "ON")); } 
    else if (String(topic) == "home/switch/relay2/set") { controlRelay(2, (message == "ON")); }
}

//=================================================================================
// TAREFAS AGENDADAS
//=================================================================================
void readSensorAndPublish(){
    if (!mqttClient.connected()) return;
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t) && !isnan(h)) {
        temperatura = t;
        umidade = h;
        char tempStr[8], umiStr[8];
        dtostrf(temperatura, 4, 1, tempStr);
        dtostrf(umidade, 4, 1, umiStr);

        mqttClient.publish("home/switch/temperatura", 1, false, tempStr);
        mqttClient.publish("home/switch/umidade", 1, false, umiStr);
        mqttClient.publish("home/switch/relay1/state", 1, false, relay1State ? "ON" : "OFF");
        mqttClient.publish("home/switch/relay2/state", 1, false, relay2State ? "ON" : "OFF");
    }
}