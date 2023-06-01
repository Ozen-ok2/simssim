#include <stdio.h>
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <freertos/event_groups.h>
#include <esp_wifi.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <mqtt_client.h>
#include <driver/gpio.h>
#include <nvs_flash.h>

#define WIFI_SSID "BLACK"
#define WIFI_PASSWORD "LORY2022"
#define MQTT_BROKER_IP "34.229.145.165"
#define MQTT_BROKER_PORT 1883
#define MQTT_USERNAME "autohome"
#define MQTT_PASSWORD "comida05"
#define MQTT_PUBLISH_TOPIC "/engcomp/button/0401"
#define MQTT_SUBSCRIBE_TOPIC "/engcomp/button/0401"

TaskHandle_t switchControlTaskHandle;
TaskHandle_t blinkTaskHandle;
TaskHandle_t counterTaskHandle;
TaskHandle_t mqttTaskHandle;
QueueHandle_t switchQueue;

esp_mqtt_client_handle_t mqttClient;

static void switchControlTask(void *pvParameters)
{
    while (1)
    {
        // Monitorar o estado da tecla e enviar para as tarefas blink e counter
        int switchState = gpio_get_level(GPIO_NUM_4);
        xQueueSend(switchQueue, &switchState, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void blinkTask(void *pvParameters)
{
    int switchState;
    while (1)
    {
        // Receber o estado da tecla da fila
        xQueueReceive(switchQueue, &switchState, portMAX_DELAY);

        // Liga o LED indicativo se o switch estiver pressionado
        if (switchState == 1)
        {
            gpio_set_level(GPIO_NUM_5, 1);
        }
        else
        {
            gpio_set_level(GPIO_NUM_5, 0);
        }
    }
}

static void mqttTask(void *pvParameters)
{
    esp_mqtt_client_config_t mqttConfig = {
        .uri = "mqtt://" MQTT_BROKER_IP ":1883",
        .username = MQTT_USERNAME,
        .password = MQTT_PASSWORD,
    };

    esp_err_t ret;
    int retry_count = 0;
    int retry_count2 = 0;

    while (1)
    {
        mqttClient = esp_mqtt_client_init(&mqttConfig);
        ret = esp_mqtt_client_start(mqttClient);

        if (ret != ESP_OK)
        {
            printf("Erro ao conectar ao MQTT. Tentando novamente em 5 segundos...\n");
            retry_count++;
            vTaskDelay(pdMS_TO_TICKS(5000));
        }
        else
        {
            printf("Conectado ao servidor MQTT.\n");
            break;
        }

        if (retry_count > 4)
        {
            printf("Falha ao conectar ao servidor MQTT após várias tentativas. Reiniciando...\n");
            esp_restart();
        }
    }



    int counter = 0;
    while (1)
    {
        // Aguardar a notificação da tarefa counter
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        // Enviar a contagem para o servidor MQTT
        char counterStr[10];
        snprintf(counterStr, sizeof(counterStr), "%d", counter);

        esp_err_t publishErr = esp_mqtt_client_publish(mqttClient, MQTT_PUBLISH_TOPIC, counterStr, 0, 1, 0);
        if (publishErr != ESP_OK)
        {
            printf("Falha ao publicar no MQTT: %d\n", publishErr);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void counterTask(void *pvParameters)
{
    int switchState;
    int counter = 0;
    while (1)
    {
        // Receber o estado da tecla da fila
        xQueueReceive(switchQueue, &switchState, portMAX_DELAY);

        // Incrementar o contador se o switch estiver pressionado
        if (switchState == 1)
        {
            counter++;
        }
        printf("Contador: %d \n", counter);
        vTaskDelay(pdMS_TO_TICKS(1000));
        // Enviar o valor do contador para a tarefa MQTT
        xTaskNotifyGive(mqttTaskHandle);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t wifi_event_handler(void* ctx, system_event_t* event)
{
    switch (event->event_id)
    {
        case SYSTEM_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_DISCONNECTED:
            esp_wifi_connect();
            break;
        case SYSTEM_EVENT_STA_GOT_IP:
            printf("Conectado ao Wi-Fi\n");
            break;
        default:
            break;
    }
    return ESP_OK;
}

static void wifi_init_sta()
{
    tcpip_adapter_init();
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .bssid_set = false
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
}

void app_main()
{
    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // Connect to Wi-Fi
    wifi_init_sta();

    // Configurar GPIO
    gpio_config_t ioConfig = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = BIT(GPIO_NUM_4),
    };
    gpio_config(&ioConfig);

    ioConfig.mode = GPIO_MODE_OUTPUT;
    ioConfig.pin_bit_mask = BIT(GPIO_NUM_5);
    gpio_config(&ioConfig);

    // Criar fila para comunicação entre as tarefas
    switchQueue = xQueueCreate(1, sizeof(int));

    // Criar tarefas
    xTaskCreate(switchControlTask, "Switch_Control", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, &switchControlTaskHandle);
    xTaskCreate(blinkTask, "Blink", configMINIMAL_STACK_SIZE, NULL, tskIDLE_PRIORITY + 1, &blinkTaskHandle);
    xTaskCreate(counterTask, "Counter", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 1, &counterTaskHandle);
    xTaskCreate(mqttTask, "MQTT", configMINIMAL_STACK_SIZE * 4, NULL, tskIDLE_PRIORITY + 1, &mqttTaskHandle);
}
