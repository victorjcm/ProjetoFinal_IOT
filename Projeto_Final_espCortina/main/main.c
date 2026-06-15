#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "driver/ledc.h" 
#include "driver/gpio.h" 
#include "coap3/coap.h"      // Biblioteca do CoAP

// ==========================================
// CONFIGURAÇÕES DE REDE
// ==========================================
#define WIFI_SSID      "MERCUSYS_7E02"
#define WIFI_PASS      "70960594"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static const char *TAG = "ESP_CORTINA";

// ==========================================
// CONFIGURAÇÕES DO HARDWARE (SERVO E BOTÃO)
// ==========================================
#define SERVO_PIN 13     // Servomotor no pino D13
#define BOTAO_PIN 27     // Botão no pino D27

#define FREQ_HZ 50
#define SERVO_POS_INICIAL   614  // Posição Aberta (0 graus)
#define SERVO_POS_FINAL   205   // Posição Fechada (90 graus)

// Variáveis globais de controle
bool cortina_aberta = false; // Assume que a cortina inicia fechada
bool modo_automatico = true; // Inicia obedecendo o sensor de luz

// ==========================================
// FUNÇÃO PARA MOVER O MOTOR
// ==========================================
void mover_cortina(bool abrir) {
    if (abrir) {
        ESP_LOGI(TAG, ">>> Movendo cortina para: ABERTA <<<");
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, SERVO_POS_INICIAL);
    } else {
        ESP_LOGI(TAG, ">>> Movendo cortina para: FECHADA <<<");
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, SERVO_POS_FINAL);
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    cortina_aberta = abrir;
}

// ==========================================
// FUNÇÕES WI-FI
// ==========================================
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

// ==========================================
// SERVIDOR CoAP (Escutando o Gateway)
// ==========================================
static void hnd_put_cortina(coap_resource_t *resource, coap_session_t *session,
                            const coap_pdu_t *request, const coap_string_t *query,
                            coap_pdu_t *response) {
    size_t size;
    const uint8_t *data;
    
    coap_get_data(request, &size, &data);
    
    char comando[16];
    size_t copy_len = size < sizeof(comando) - 1 ? size : sizeof(comando) - 1;
    memcpy(comando, data, copy_len);
    comando[copy_len] = '\0';

    ESP_LOGI(TAG, "[CoAP] Comando recebido do Gateway: %s", comando);

    // Lógica de Estados Absolutos
    if (strcmp(comando, "ABRIR") == 0) {
        if (!cortina_aberta) {
            mover_cortina(true);
        } else {
            ESP_LOGI(TAG, "Comando ignorado: Cortina ja esta ABERTA.");
        }
        
    } else if (strcmp(comando, "FECHAR") == 0) {
        if (cortina_aberta) {
            mover_cortina(false);
        } else {
            ESP_LOGI(TAG, "Comando ignorado: Cortina ja esta FECHADA.");
        }
        
    // Mantido por compatibilidade ou para o botão físico (se necessário no futuro)
    } else if (strcmp(comando, "INVERTER") == 0) {
        mover_cortina(!cortina_aberta);
        
    } else if (strcmp(comando, "AUTO_ON") == 0) {
        modo_automatico = true;
        ESP_LOGI(TAG, "Modo Automatico: ATIVADO");
        
    } else if (strcmp(comando, "AUTO_OFF") == 0) {
        modo_automatico = false;
        ESP_LOGI(TAG, "Modo Automatico: DESATIVADO");
    }

    coap_pdu_set_code(response, COAP_RESPONSE_CODE_CHANGED);
}

static void coap_server_task(void *p) {
    coap_context_t *ctx = coap_new_context(NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "Falha ao criar contexto CoAP");
        vTaskDelete(NULL);
    }

    // Abre socket UDP na porta 5683 para receber comandos do gateway
    coap_address_t listen_addr;
    coap_address_init(&listen_addr);
    listen_addr.addr.sin.sin_family = AF_INET;
    listen_addr.addr.sin.sin_addr.s_addr = INADDR_ANY;
    listen_addr.addr.sin.sin_port = htons(5683);
    coap_new_endpoint(ctx, &listen_addr, COAP_PROTO_UDP);

    // Rota: coap://IP_DA_CORTINA/cortina
    coap_resource_t *res = coap_resource_init(coap_make_str_const("cortina"), 0);
    coap_register_handler(res, COAP_REQUEST_PUT, hnd_put_cortina);
    coap_add_resource(ctx, res);

    ESP_LOGI(TAG, "Servidor CoAP rodando (Aguardando comandos da Nuvem)...");

    while (1) {
        coap_io_process(ctx, 1000);
    }
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando Node da Cortina (Grupo 4)...");

    //  Inicializações de Memória e Rede
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();

    //  Configurar Botão
    gpio_config_t config_botao = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << BOTAO_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1
    };
    gpio_config(&config_botao);

    //  Configurar PWM do Motor
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_LOW_SPEED_MODE,
        .timer_num        = LEDC_TIMER_0,
        .duty_resolution  = LEDC_TIMER_13_BIT, 
        .freq_hz          = FREQ_HZ,           
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_LOW_SPEED_MODE,
        .channel        = LEDC_CHANNEL_0,
        .timer_sel      = LEDC_TIMER_0,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = SERVO_PIN,
        .duty           = 0, 
        .hpoint         = 0
    };
    ledc_channel_config(&ledc_channel);

    //  Iniciar motor na posição padrão
    mover_cortina(false); // Começa com a cortina fechada

    //  Conectar no Wi-Fi
    wifi_init_sta(); 

    //  Ligar o servidor CoAP em segundo plano
    xTaskCreate(coap_server_task, "coap_server", 8192, NULL, 5, NULL);

    ESP_LOGI(TAG, "===============================================");
    ESP_LOGI(TAG, " SISTEMA PRONTO! Modo Automatico = ATIVADO");
    ESP_LOGI(TAG, "===============================================");

    int estado_anterior_botao = 1; 

    //  Loop de monitoramento do botão físico
    while (1) {
        int estado_atual_botao = gpio_get_level(BOTAO_PIN);

        // Se pressionado (transição de 1 para 0)
        if (estado_atual_botao == 0 && estado_anterior_botao == 1) {
            ESP_LOGI(TAG, "Comando MANUAL detectado (Botao Fisico)!");
            
            
            if (modo_automatico) {
                ESP_LOGW(TAG, "Intervencao Fisica: Desativando Modo Automatico.");
                modo_automatico = false; // assumir o controle com o botao fisico
            }
            
            mover_cortina(!cortina_aberta);
            
            // Debounce 
            vTaskDelay(pdMS_TO_TICKS(300)); 
        }

        estado_anterior_botao = estado_atual_botao;
        vTaskDelay(pdMS_TO_TICKS(20)); // Pausa para não travar a CPU
    }
}