#ifndef __J1939TWAI_H__
#define __J1939TWAI_H__

#define J1939_MY_ADR 25
#define CAN_TX_GPIO_NUM (gpio_num_t)5
#define CAN_RX_GPIO_NUM (gpio_num_t)4
#define CAN_RX_TASK_PRIO 8
#define CAN_TX_TASK_PRIO 9
#include <app_common.h>
#include <driver/twai.h>


#define MYJ_BUFFERS_LENGTH 2
#define MYJ_BUFFERS_SHORT_LENGTH 4
#define MYJ_BUFFERS_BIG_LENGTH 4

#define MYJ_TPCM_PGN 60416 // connection managment frame
#define MYJ_TPDT_PGN 60160 // data transfer

#define MYJ_TP_TIMEOUT 1000

struct J1939TransportMsgBuf
{
  uint8_t iswriting;
  uint8_t packages_length;
  uint8_t expected_package;
  TickType_t startTick;
  J1939Msg msg;
};

namespace J1939Twai
{
    esp_err_t Run(AppState *state);
}
#endif // __J1939TWAI_H__