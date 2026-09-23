#ifndef __DELAY_H
#define __DELAY_H

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void delay_ms(uint32_t ms);

#endif