#ifndef _APP_COMMON_H_
#define _APP_COMMON_H_

#include <esp_log.h>
#include <stdlib.h>
#include <stdbool.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdio.h>
#include <driver/ledc.h>
#include <driver/twai.h>
#include <freertos/semphr.h>
#include <string.h>
#include <cJSON.h>
#include <esp_netif.h>
#include <esp_http_server.h>
#include <esp_err.h>

typedef unsigned char byte;

/*
 * Defines the prototype to which task functions must conform.  Defined in this
 * file to ensure the type is known before portable.h is included.
 */
typedef void (*J1939RequestFunction_t)(uint8_t fromECU, uint32_t pgnN);

struct wsSendBytes
{
  uint32_t numBytes;
  uint8_t *buf;
};

#define WEB_DEPLOY_SF true
#define WEB_MOUNT_POINT "/spiffs"
#define WEB_CONFIG_FILENAME "/spiffs/config.json"
#define WEB_MDNS_HOST_NAME "espserv"
#define WEB_MDNS_INSTANCE "esp home web server"

enum WsMessageType : uint8_t
{
  MES_J1939,
  MES_CAN_STATUS,
  MES_WiFi_STATUS,
  MES_WS_PING
};

enum WsMessageTypeOut : uint8_t
{
  MESS_WORK = 1,
  MESS_SET_SPEED,
  MESS_SET_GEAR,
  MESS_SEND_1939REQ,
};

enum WsMessJ1939Req : uint8_t
{
  GET_STORED_CODES = 1,
  CLEAR_STORED_CODES
};

enum WorkReg
{
  WORKREG_MONITOR = 0,
  WORKREG_MON_RPM,
  WORKREG_MON_TRANS,
  WORKREG_MON_ALL
};

struct J1939Msg
{
  uint32_t lPGN;
  uint16_t nDataLen;
  uint8_t nPriority;
  uint8_t nSrcAddr;
  uint8_t nDestAddr;
  uint8_t nData[1784];
};

struct J1939MsgShort
{
  uint32_t lPGN;
  uint16_t nDataLen;
  uint8_t nPriority;
  uint8_t nSrcAddr;
  uint8_t nDestAddr;
  uint8_t nData[8];
};

struct J1939Module
{
  WorkReg workType;
  J1939Msg message;
  J1939RequestFunction_t reqFunc;
  QueueHandle_t mesShortQueue;
  QueueHandle_t mesLongQueue;  
};

struct SystemModule
{
  esp_netif_t *netifService;
  httpd_handle_t server;
};

struct AppState
{
  long msgType;    //тип передаваемого сообщения в сокет 1 - J1939 2 AppState 3 ADC
  long j1939state; //передает в сокет состояние j1939 1-ok 0 bad
  long workReg;    //режим работы 1 - MONITOR 2 TSC
  long wifiState;
  J1939Module j1939module;
  
  SystemModule sysModule;
 
  
}; //__attribute__((packed));

#define WEBSOCK_STATE_OFF 0
#define WEBSOCK_STATE_ON 1

#define MESS_OUT_J1939 1
#define MESS_OUT_SERVICE 2
#define MESS_OUT_ADC 3

#define bitset(byte, nbit) ((byte) |= (1 << (nbit)))
#define bitclear(byte, nbit) ((byte) &= ~(1 << (nbit)))
#define bitflip(byte, nbit) ((byte) ^= (1 << (nbit)))
#define bitcheck(byte, nbit) ((byte) & (1 << (nbit)))

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))

#endif
/*********************************************************************************************************
  END FILE
*********************************************************************************************************/