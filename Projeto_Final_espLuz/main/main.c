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
#include "esp_adc/adc_oneshot.h"

// Inclusão da biblioteca CoAP 
#include "coap3/coap.h"

// ==========================================
// CONFIGURAÇÕES DE REDE E GATEWAY
// ==========================================
#define WIFI_SSID      "MERCUSYS_7E02"
#define WIFI_PASS      "70960594"

// IPdo gateway
#define GATEWAY_IP     "192.168.1.105"
// Porta padrão do protocolo CoAP
#define COAP_PORT      5683 
// Caminho do recurso dos dados
#define COAP_URI_PATH  "sensor_luz"

static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static const char *TAG = "ESP_SENSOR_COAP";

// Pino D34 corresponde ao Canal 6 do ADC1
#define LDR_CANAL ADC_CHANNEL_6 

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
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
    esp_wifi_start();

    ESP_LOGI(TAG, "Aguardando conexão com %s...", WIFI_SSID);
    xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
}

// ==========================================
// TAREFA DO CLIENTE COAP
// ==========================================
void coap_client_task(void *p) {
    ESP_LOGI(TAG, "Iniciando Leitura LDR e Cliente CoAP...");

    // Configuração do ADC (Sensor)
    adc_oneshot_unit_handle_t adc1_handle;
    adc_oneshot_unit_init_cfg_t init_config1 = { .unit_id = ADC_UNIT_1 };
    adc_oneshot_new_unit(&init_config1, &adc1_handle);

    adc_oneshot_chan_cfg_t config = { .bitwidth = ADC_BITWIDTH_DEFAULT, .atten = ADC_ATTEN_DB_12 };
    adc_oneshot_config_channel(adc1_handle, LDR_CANAL, &config);

    // Variáveis CoAP
    coap_address_t dst_addr;
    coap_context_t *ctx = NULL;
    coap_session_t *session = NULL;
    
    // Resolve o IP do Gateway
    coap_address_init(&dst_addr);
    dst_addr.addr.sin.sin_family = AF_INET;
    dst_addr.addr.sin.sin_port = htons(COAP_PORT);
    inet_pton(AF_INET, GATEWAY_IP, &dst_addr.addr.sin.sin_addr);

    // Inicializa o contexto de rede CoAP
    ctx = coap_new_context(NULL);
    if (!ctx) {
        ESP_LOGE(TAG, "Falha ao criar contexto CoAP");
        vTaskDelete(NULL);
    }

    // Cria a sessão UDP apontando para o seu Computador
    session = coap_new_client_session(ctx, NULL, &dst_addr, COAP_PROTO_UDP);
    if (!session) {
        ESP_LOGE(TAG, "Falha ao criar sessão CoAP");
        coap_free_context(ctx);
        vTaskDelete(NULL);
    }

    char valor_str[15];

    while (1) {
        int valor_ldr = 0;
        
        // Lê o LDR e converte para string
        adc_oneshot_read(adc1_handle, LDR_CANAL, &valor_ldr);
        sprintf(valor_str, "%d", valor_ldr);

        // Cria o pacote (PDU) do tipo POST
        coap_pdu_t *pdu = coap_pdu_init(COAP_MESSAGE_NON, COAP_REQUEST_CODE_POST, coap_new_message_id(session), coap_session_max_pdu_size(session));
        
        if (pdu) {
            // Define a rota do POST: /sensor_luz
            coap_add_option(pdu, COAP_OPTION_URI_PATH, strlen(COAP_URI_PATH), (const uint8_t *)COAP_URI_PATH);
            
            // Adiciona o Payload: O valor da luz
            coap_add_data(pdu, strlen(valor_str), (const uint8_t *)valor_str);
            
            // Dispara pela rede
            if (coap_send(session, pdu) != COAP_INVALID_MID) {
                ESP_LOGI(TAG, "-> POST CoAP /%s enviado p/ Gateway: %s", COAP_URI_PATH, valor_str);
            }
        }

        // Dá tempo ao FreeRTOS para processar o tráfego da rede
        coap_io_process(ctx, 1000); 
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

// ==========================================
// LOOP PRINCIPAL
// ==========================================
void app_main(void)
{
    ESP_LOGI(TAG, "Iniciando Node do Sensor via CoAP (Grupo 4)...");

    // Inicializações Base
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    esp_netif_init();
    esp_event_loop_create_default();

    // Inicia Wi-Fi
    wifi_init_sta(); 

    // Libera o Loop Principal para outras coisas e joga o CoAP para uma Tarefa Independente
    xTaskCreate(coap_client_task, "coap_client", 8192, NULL, 5, NULL);
}
