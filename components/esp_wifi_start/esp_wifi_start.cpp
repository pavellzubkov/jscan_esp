#include <esp_wifi_start.h>
#include <string>

#define JHOST_NAME "jscan"
#define EXAMPLE_ESP_WIFI_SSID "J1939_AP"
#define EXAMPLE_ESP_WIFI_CHANNEL 12
#define EXAMPLE_ESP_WIFI_PASS "12345678"
#define EXAMPLE_MAX_STA_CONN 2

/* The event group allows multiple bits for each event, but we only care about two events:
 * - we are connected to the AP with an IP
 * - we failed to connect after the maximum amount of retries */
#define WIFI_CONNECTED_BIT BIT0

namespace WiFiNetStart
{
    namespace
    {
        static const char *TAG = "wifi softAP";
        AppState *state;

        wifi_config_t wifi_config = {
            .ap = {
                EXAMPLE_ESP_WIFI_SSID,
                EXAMPLE_ESP_WIFI_PASS,

            }};

        static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
        {
            switch (event_id)
            {
            case WIFI_EVENT_AP_STACONNECTED:
            {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGW(TAG, "AP mode sta " MACSTR " connected, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED:
            {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGW(TAG, "AP mode sta " MACSTR " disconnected, AID=%d",
                         MAC2STR(event->mac), event->aid);
                break;
            }

            case WIFI_EVENT_STA_START:
            {
                esp_wifi_connect();
                ESP_LOGW(TAG, "STA mode start");
                break;
            }

            case WIFI_EVENT_STA_CONNECTED:
            {
                wifi_event_sta_connected_t *event = (wifi_event_sta_connected_t *)event_data;
                ESP_LOGW(TAG, "STA mode connected to AP=%s", event->ssid);
                break;
            }

            case WIFI_EVENT_STA_DISCONNECTED:
            {
                esp_wifi_connect();
                ESP_LOGW(TAG, "STA mode retry to connect to the AP %s", wifi_config.sta.ssid);

                break;
            }

            case IP_EVENT_STA_GOT_IP:
            {
                ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
                ESP_LOGW(TAG, "STA mode got ip:" IPSTR, IP2STR(&event->ip_info.ip));
                break;
            }

            default:
                ESP_LOGW(TAG, "WiFi got event: %d", event_id);
                break;
            }
        }

        esp_err_t wifi_init_softap(wifi_config_t *wifi_conf)
        {

            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
            ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, wifi_conf));
            ESP_ERROR_CHECK(esp_wifi_start());

            ESP_LOGW(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
                     wifi_conf->ap.ssid, wifi_conf->ap.password, EXAMPLE_ESP_WIFI_CHANNEL);
            return ESP_OK;
        }    

    }

    esp_err_t Run(AppState *adr)
    {
        state = adr;
        wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

        ESP_ERROR_CHECK(esp_wifi_init(&cfg));
        ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE)); // disable power save

        ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                            ESP_EVENT_ANY_ID,
                                                            &wifi_event_handler,
                                                            NULL,
                                                            NULL));

        wifi_config.ap.max_connection = EXAMPLE_MAX_STA_CONN;
        wifi_config.ap.channel = EXAMPLE_ESP_WIFI_CHANNEL;
        
        ESP_ERROR_CHECK(esp_netif_init());
         state->sysModule.netifService = esp_netif_create_default_wifi_ap();

        esp_netif_ip_info_t ipInfo;
        IP4_ADDR(&ipInfo.ip, 10, 10, 10, 10);
        IP4_ADDR(&ipInfo.gw, 10, 10, 10, 10);
        IP4_ADDR(&ipInfo.netmask, 255, 255, 255, 0);
        esp_netif_dhcps_stop(state->sysModule.netifService);
        esp_netif_set_ip_info(state->sysModule.netifService, &ipInfo);
        
        
        ESP_ERROR_CHECK(esp_netif_dhcps_start(state->sysModule.netifService));

        ESP_ERROR_CHECK(wifi_init_softap(&wifi_config));        

        return ESP_OK;
    }
}
