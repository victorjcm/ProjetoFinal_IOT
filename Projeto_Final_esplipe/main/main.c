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
#include "driver/ledc.h"
#include "mqtt_client.h"
#include "esp_log.h"

// ==========================================
// CONFIGURAÇÕES DE REDE E MQTT
// ==========================================
#define WIFI_SSID      "Pitucha Mercu Novo"
#define WIFI_PASS      "pituxa1989"

// ATENÇÃO: Coloque o IP do seu computador aqui
#define BROKER_URI     "mqtt://192.168.0.146" 
#define MQTT_TOPIC     "ifpb/projeto/porta"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG = "ESP_B_MOTOR";
esp_mqtt_client_handle_t mqtt_client = NULL;

// ==========================================
// CONFIGURAÇÕES DE HARDWARE (Motor e Sensor)
// ==========================================
#define SENSOR_GPIO             GPIO_NUM_26 // Sensor magnético MC38
#define SERVO_GPIO              GPIO_NUM_27 // Servomotor

#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT 
#define LEDC_FREQUENCY          50                

#define SERVO_SPEED_STEP        1.6f 

// Calibração perfeita da peça 3D
int angulo_repouso = 100; 
int angulo_minimo = 0;    

volatile float target_angle = 100.0f; // Começa na posição fechada

// ==========================================
// FUNÇÕES DE HARDWARE
// ==========================================
static void init_sensor(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << SENSOR_GPIO),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&io_conf);
}

static void init_servo(void) {
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .timer_num        = LEDC_TIMER,
        .duty_resolution  = LEDC_DUTY_RES,
        .freq_hz          = LEDC_FREQUENCY,
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = SERVO_GPIO,
        .duty           = 0, 
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);
}

static void set_servo_angle(float angle) {
    float min_duty = 204.0f;
    float max_duty = 1024.0f;
    uint32_t duty = (uint32_t)(min_duty + ((max_duty - min_duty) * angle) / 180.0f);
    ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, duty);
    ledc_update_duty(LEDC_MODE, LEDC_CHANNEL);
}

// ==========================================
// FUNÇÕES WI-FI E MQTT
// ==========================================
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data) {
    esp_mqtt_event_handle_t event = event_data;
    switch ((esp_mqtt_event_id_t)event_id) {
        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "Conectado ao Broker! Escutando comandos...");
            esp_mqtt_client_subscribe(event->client, MQTT_TOPIC, 0);
            break;
            
        case MQTT_EVENT_DATA:
            {
                char payload[20];
                int len = event->data_len < 19 ? event->data_len : 19;
                memcpy(payload, event->data, len);
                payload[len] = '\0';
                
                ESP_LOGI(TAG, "Comando MQTT Recebido: %s", payload);
                
                // Muda o alvo do motor baseado na string recebida
                if (strcmp(payload, "ABRIR") == 0) {
                    target_angle = (float)angulo_minimo;
                    ESP_LOGI(TAG, "Ação: Abrindo a porta...");
                } 
                else if (strcmp(payload, "FECHAR") == 0) {
                    target_angle = (float)angulo_repouso;
                    ESP_LOGI(TAG, "Ação: Fechando a porta...");
                }
            }
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
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
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
    ESP_LOGI(TAG, "Iniciando ESP B (Atuador MQTT)...");
    
    init_servo();
    init_sensor();

    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();
    
    wifi_init_sta(); 
    mqtt_app_start();

    // Começa exatamente no ângulo perfeito de repouso
    float current_angle = (float)angulo_repouso;
    set_servo_angle(current_angle);
    
    int print_counter = 0;

    while (1) {
        // 1. APROXIMAÇÃO SUAVE
        if (current_angle < target_angle) {
            current_angle += SERVO_SPEED_STEP;
            if (current_angle > target_angle) current_angle = target_angle;
        } 
        else if (current_angle > target_angle) {
            current_angle -= SERVO_SPEED_STEP;
            if (current_angle < target_angle) current_angle = target_angle;
        }

        // 2. TRAVA RÍGIDA DE SEGURANÇA (Proteção da peça 3D)
        if (current_angle > angulo_repouso) {
            current_angle = angulo_repouso;
        }

        // 3. ATUALIZA O MOTOR
        set_servo_angle(current_angle);

        // 4. LEITURA DO SENSOR MAGNÉTICO E LOG
        int estado_sensor = gpio_get_level(SENSOR_GPIO);
        const char* status_porta = (estado_sensor == 1) ? "ABERTA" : "FECHADA";

        print_counter++;
        if (print_counter >= 50) { // Imprime a cada ~1 segundo para não poluir o terminal
            ESP_LOGI(TAG, "Sensor Físico: %s | Alvo: %3d | Posição Atual Motor: %3.1f graus", 
                     status_porta, (int)target_angle, current_angle);
            print_counter = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}