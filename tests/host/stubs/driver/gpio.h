#pragma once
// Заглушка driver/gpio.h для host-тестов: HardwareConfig.h использует только
// тип gpio_num_t и GPIO_NUM_5/4 (пины TWAI).
typedef int gpio_num_t;

#define GPIO_NUM_0 ((gpio_num_t)0)
#define GPIO_NUM_1 ((gpio_num_t)1)
#define GPIO_NUM_2 ((gpio_num_t)2)
#define GPIO_NUM_3 ((gpio_num_t)3)
#define GPIO_NUM_4 ((gpio_num_t)4)
#define GPIO_NUM_5 ((gpio_num_t)5)
#define GPIO_NUM_6 ((gpio_num_t)6)
#define GPIO_NUM_7 ((gpio_num_t)7)