#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "py32f0xx_ll_adc.h"
#include "py32f0xx_ll_bus.h"
#include "py32f0xx_ll_cortex.h"
#include "py32f0xx_ll_dma.h"
#include "py32f0xx_ll_exti.h"
#include "py32f0xx_ll_flash.h"
#include "py32f0xx_ll_gpio.h"
#include "py32f0xx_ll_i2c.h"
#include "py32f0xx_ll_iwdg.h"
#include "py32f0xx_ll_pwr.h"
#include "py32f0xx_ll_rcc.h"
#include "py32f0xx_ll_spi.h"
#include "py32f0xx_ll_system.h"
#include "py32f0xx_ll_tim.h"
#include "py32f0xx_ll_usart.h"
#include "py32f0xx_ll_utils.h"

#if defined(USE_FULL_ASSERT)
#include "py32_assert.h"
#endif

void APP_ErrorHandler(void);

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
