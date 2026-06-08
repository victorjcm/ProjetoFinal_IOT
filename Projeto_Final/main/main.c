#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"
#include "mqtt_client.h"
#include "esp_log.h"

// ==========================================
// CONFIGURAÇÕES DE REDE E MQTT
// ==========================================
#define WIFI_SSID      "Pitucha Mercu Novo"
#define WIFI_PASS      "pituxa1989"

// ATENÇÃO: Substitua pelo IP do seu computador (ex: mqtt://192.168.0.146)
#define BROKER_URI     "mqtt://192.168.0.146" 
#define MQTT_TOPIC     "ifpb/projeto/porta"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "ESP_A_TECLADO";
esp_mqtt_client_handle_t mqtt_client = NULL;

// ==========================================
// CONFIGURAÇÕES DO TECLADO
// ==========================================
static const gpio_num_t rows[4] = { GPIO_NUM_19, GPIO_NUM_4,  GPIO_NUM_16, GPIO_NUM_5 };
static const gpio_num_t cols[3] = { GPIO_NUM_18, GPIO_NUM_21, GPIO_NUM_17 };

static const char keypad[4][3] = {
    {'1', '2', '3'},
    {'4', '5', '6'},
    {'7', '8', '9'},
    {'*', '0', '#'}
};

void keypad_init(void) {
    for (int i = 0; i < 4; i++) {
        gpio_reset_pin(rows[i]);
        gpio_set_direction(rows[i], GPIO_MODE_OUTPUT);
        gpio_set_level(rows[i], 1);
    }
    for (int i = 0; i < 3; i++) {
        gpio_reset_pin(cols[i]);
        gpio_set_direction(cols[i], GPIO_MODE_INPUT);
        gpio_pullup_en(cols[i]);
    }
}

char keypad_getkey(void) {
    for (int r = 0; r < 4; r++) {
        for (int i = 0; i < 4; i++) {
            gpio_set_level(rows[i], 1);
        }
        gpio_set_level(rows[r], 0);
        esp_rom_delay_us(50);

        for (int c = 0; c < 3; c++) {
            if (gpio_get_level(cols[c]) == 0) {
                vTaskDelay(pdMS_TO_TICKS(50)); // Debounce
                if (gpio_get_level(cols[c]) == 0) {
                    char key = keypad[r][c];
                    while (gpio_get_level(cols[c]) == 0) {
                        vTaskDelay(pdMS_TO_TICKS(10));
                    }
                    return key;
                }
            }
        }
    }
    return '\0';
}

// ==========================================
// FUNÇÕES WI-FI E MQTT
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao Broker MQTT com sucesso!");
            break;
        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGI(TAG, "Desconectado do Broker MQTT.");
            break;
        default:
            break;
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "Tentando reconectar ao roteador...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Conectado ao Wi-Fi! IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init_sta(void) {
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Aguardando conexão com %s...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

static void mqtt_app_start(void) {
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = BROKER_URI,
    };
    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void app_main(void) {
    ESP_LOGI(TAG, "Iniciando Controle de Acesso (Teclado)...");
    
    // 1. Inicializações Base
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    
    // 2. Inicializa o Teclado
    keypad_init();
    
    // 3. Conecta no Wi-Fi e no Servidor MQTT
    wifi_init_sta(); 
    mqtt_app_start();
    
    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, " SISTEMA PRONTO! Pressione '1' ou '2' no teclado.");
    ESP_LOGI(TAG, "===============================================");

    // 4. Loop Infinito lendo o teclado
    while (1) {
        char tecla = keypad_getkey();

        if (tecla != '\0') {
            ESP_LOGI(TAG, "Tecla pressionada: %c", tecla);

            // Garante que o cliente MQTT já existe antes de tentar enviar
            if (mqtt_client != NULL) {
                if (tecla == '1') {
                    // Publica a mensagem "ABRIR" no tópico
                    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, "ABRIR", 0, 1, 0);
                    ESP_LOGI(TAG, "-> Comando enviado via MQTT: ABRIR");
                } 
                else if (tecla == '2') {
                    // Publica a mensagem "FECHAR" no tópico
                    esp_mqtt_client_publish(mqtt_client, MQTT_TOPIC, "FECHAR", 0, 1, 0);
                    ESP_LOGI(TAG, "-> Comando enviado via MQTT: FECHAR");
                }
            }
        }

        // Pausa curta para não sobrecarregar o processador
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}