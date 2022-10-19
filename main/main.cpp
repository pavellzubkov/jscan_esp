#include <app_common.h>
#include <esp_wifi_start.h>
#include <esp_fs_start.h>
#include <_ws_server.h>
#include <j1939twai.h>


static AppState app_state;

extern "C" void app_main();

void app_main()
{    

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(FSService::RunSPIFFS(&app_state)); 

    ESP_ERROR_CHECK(WiFiNetStart::Run(&app_state));

    ESP_ERROR_CHECK(FSService::RunHTTP(&app_state));

    ESP_ERROR_CHECK(WebSockServer::Run(&app_state));

    ESP_ERROR_CHECK(FSService::RunREST(&app_state));   

    ESP_ERROR_CHECK(J1939Twai::Run(&app_state));

}