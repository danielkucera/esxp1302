#include <stdio.h>
#include <string.h>         /* strncpy */

#include "esp_event.h"
#include "esp_log.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "driver/gpio.h"

#include "freertos/event_groups.h"

#include "web_config.h"
#include "ioe.h"

/* for buttons on the bottom board */
#ifndef USER_BUTTON_1
#define USER_BUTTON_1    23
#endif

#ifndef USER_BUTTON_2
#define USER_BUTTON_2    25
#endif

#define BUTTON_PRESSED    0
#define BUTTON_RELEASED   1

/* for wifi configures */
#define ESP_WIFI_SSID      "esp32"
#define ESP_WIFI_PASS      "esp32wifi"
#define ESP_WIFI_CHANNEL   1
#define MAX_STA_CONN       4

#define WIFI_MAXIMUM_RETRY  5

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static const char *WIFI_TAG = "wifi station";

/* FreeRTOS event group to signal when we are connected*/
static EventGroupHandle_t s_wifi_event_group;

extern config_s config[CONFIG_NUM];

bool wifi_ready = false;
char wifi_ssid[32];
char wifi_pswd[64];

char self_ip[16] = "(unknown)";

// buffer for display output
char out_info[96];

static bool reboot_flag = false;
static void reboot_timer_callback(void)
{
    if(reboot_flag){
        printf("\n!!! reboot timer called\n");
        esp_restart();
    }
}

void start_reboot_timer_ms(int reboot_delay)
{
    const esp_timer_create_args_t reboot_timer_args = {
            .callback = &reboot_timer_callback,
            .name = "one-shot"
    };
    esp_timer_handle_t reboot_timer;
    ESP_ERROR_CHECK(esp_timer_create(&reboot_timer_args, &reboot_timer));
    ESP_ERROR_CHECK(esp_timer_start_once(reboot_timer, reboot_delay * 1000));
}

bool network_ready(){
    return wifi_ready;
}

static void wifi_ap_event_handler(void* arg, esp_event_base_t event_base,
        int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(WIFI_TAG, "station "MACSTR" join, AID=%d",
                MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(WIFI_TAG, "station "MACSTR" leave, AID=%d",
                MAC2STR(event->mac), event->aid);
    }
}


void wifi_init_soft_ap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_ap_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = ESP_WIFI_SSID,
            .ssid_len = strlen(ESP_WIFI_SSID),
            .channel = ESP_WIFI_CHANNEL,
            .password = ESP_WIFI_PASS,
            .max_connection = MAX_STA_CONN,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        },
    };
    if (strlen(ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(WIFI_TAG, "wifi_init_soft_ap finished. SSID:%s password:%s channel:%d",
             ESP_WIFI_SSID, ESP_WIFI_PASS, ESP_WIFI_CHANNEL);
}


static int s_retry_num = 0;

static void wifi_sta_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    int reboot_delay_s = 60 * 3;  // reboot in 3 minute if wifi disconnected
    static bool reboot_timer_started = false;
    static bool task_started = false;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if(reboot_timer_started == false){
            reboot_timer_started = true;
            start_reboot_timer_ms(reboot_delay_s * 1000); // change to ms
        }

        wifi_ready = false;
        if (s_retry_num < WIFI_MAXIMUM_RETRY) {
            oled_show_str(0, 5, "You can re-config wifi at command line or under Soft-AP mode.  ", 1);
            ESP_LOGI(WIFI_TAG, "retry to connect to the AP");
            esp_wifi_connect();
            s_retry_num++;
        } else {
            // extra ' 's at the end are used to overwrite any old content on screen
            oled_show_one_line(0, 3, "Wifi Failed!", 1);
            oled_show_one_line(0, 4, "Reboot soon!", 1);
            oled_show_str(0, 5, "You can re-config wifi at command line or under Soft-AP mode.  ", 1);

            ESP_LOGI(WIFI_TAG, "Failed to connect to the AP; retry again...");
            esp_wifi_connect();
            s_retry_num = 0;
            reboot_flag = true;
        }
        ESP_LOGI(WIFI_TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        wifi_ready = true;
        ESP_LOGI(WIFI_TAG, "connected to ap (SSID:%s) succeeded", wifi_ssid);
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(WIFI_TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        sprintf(self_ip, "%d.%d.%d.%d", IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        reboot_flag = false;

        // display gateway's own IP
        snprintf(out_info, sizeof out_info, "IP=%s", (char *)self_ip);
        oled_show_one_line(0, 3, out_info, 1);

        // display Wi-Fi SSID and NS IP and port
        snprintf(out_info, sizeof out_info, "WiFi=%s", (char *)wifi_ssid);
        out_info[22] = '\0';
        oled_show_one_line(0, 4, out_info, 1);

/* TODO
        // restore or clean screen
        snprintf(out_info, sizeof out_info, "NS=%s:%d", (char *)udp_host, udp_port);
        //out_info[22] = '\0';
        if(strlen(udp_host) > 16)  // too long to put in a single line with ":port"
            snprintf(out_info + 13, 9, "...:%d", udp_port);
        oled_show_one_line(0, 5, out_info, 1);
*/

        oled_show_one_line(0, 6, "Update Time..", 1);
        oled_show_one_line(0, 7, "Update Temp and GPS..", 1);

        if(task_started == false){
            task_started = true;  // only run once
            printf("Wi-Fi ready. Start tasks...\n");

            // update the time by NTP
            sntp_setoperatingmode(SNTP_OPMODE_POLL);
            sntp_setservername(0, "cn.pool.ntp.org");
            sntp_init();

            config_wifi_mode(WIFI_MODE_STATION);
        }
    }
}

void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_sta_event_handler,
                                                        NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_sta_event_handler,
                                                        NULL, NULL));

    wifi_config_t wifi_config = {
        .sta = {},
    };


    if(wifi_ssid[0] == '\0'){
        ESP_LOGE(WIFI_TAG, "No Wi-Fi ssid provided. Skip Wi-Fi connection");
        return;
    }

    strncpy((char *)wifi_config.sta.ssid, (char *)wifi_ssid, 32);
    strncpy((char *)wifi_config.sta.password, (char *)wifi_pswd, 64);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(WIFI_TAG, "wifi_init_sta finished.");
}

char* wifi_get_ssid(){
    return wifi_ssid;
}

void wifi_set_ssid(char* ssid, int len){
    strncpy(wifi_ssid, ssid, len);
}

char* wifi_get_password(){
    return wifi_pswd;
}

void wifi_set_password(char* password, int len){
    strncpy(wifi_pswd, password, len);
}

void config_wifi(){

    // cancel the reboot now that user inputs something on the command line
    reboot_flag = false;

}

void start_network(){
    int reboot_delay_s;
    bool soft_ap_mode = false;

    gpio_set_direction(USER_BUTTON_1, GPIO_MODE_INPUT);
    gpio_set_direction(USER_BUTTON_2, GPIO_MODE_INPUT);

    if(BUTTON_PRESSED == 0){
        gpio_pullup_en(USER_BUTTON_1);
        gpio_pullup_en(USER_BUTTON_2);
    } else {
        gpio_pulldown_en(USER_BUTTON_1);
        gpio_pulldown_en(USER_BUTTON_2);
    }

    if(gpio_get_level(USER_BUTTON_1) == BUTTON_PRESSED){
        printf("User button 1 is PRESSED, go to soft AP mode\n");
        soft_ap_mode = true;
    } else if(gpio_get_level(USER_BUTTON_2) == BUTTON_PRESSED){
        printf("User button 2 is PRESSED, go to station mode\n");
        soft_ap_mode = false;
    } else if(config[WIFI_MODE].val == NULL){
        soft_ap_mode = true;  // if not set yet, go to soft_ap mode as the default
    } else if(strncmp(config[WIFI_MODE].val, "soft_ap", 7) == 0){
        soft_ap_mode = true;
    }

    if(soft_ap_mode == true){
        config_wifi_mode(WIFI_MODE_STATION);

        // set up timer for reboot
        reboot_delay_s = 60 * 10; // 10 minutes
        reboot_flag = true;
        start_reboot_timer_ms(reboot_delay_s * 1000); // change to ms

        ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_SOFT_AP");
        wifi_init_soft_ap();

        oled_show_one_line(0, 3, "IP=192.168.4.1", 2);
        oled_show_one_line(0, 5, "Soft AP mode", 1);

        sprintf(out_info, "SSID=%s", ESP_WIFI_SSID);
        oled_show_one_line(0, 6, out_info, 1);
        sprintf(out_info, "PSWD=%s", ESP_WIFI_PASS);
        oled_show_one_line(0, 7, out_info, 1);

    } else {
        oled_show_one_line(0, 3, "station mode", 1);

        oled_show_one_line(0, 4, "Wifi not connected.", 1);
        oled_show_str(0, 5, "You can re-config wifi at command line or under Soft-AP mode.  ", 1);

        // assume wifi can't connect; set up timer preparing for reboot soon
        config_wifi_mode(WIFI_MODE_SOFT_AP);
        reboot_delay_s = 60 * 5; // 5 minutes
        reboot_flag = true;
        start_reboot_timer_ms(reboot_delay_s * 1000); // change to ms

        ESP_LOGI(WIFI_TAG, "ESP_WIFI_MODE_STA");
        wifi_init_sta();
    }

}

