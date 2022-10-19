#ifndef ___WS_SERVER_H__
#define ___WS_SERVER_H__

#include <app_common.h>
#include "esp_http_server.h"




namespace WebSockServer{
   esp_err_t Run(AppState *adr); 
}



#endif // ___WS_SERVER_H__