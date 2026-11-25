#include "bsp_board.h"
#include "main.h"
#include "py32f0xx_it.h"

void NMI_Handler(void) {
    /* Direct register write avoids function calls; OSSI=1 + MOE=0 drives PA0 actively LOW. */
    if ((RCC->APBENR2 & RCC_APBENR2_TIM1EN) != 0U) {
        TIM1->CCR3 = 0U;
        TIM1->BDTR = (TIM1->BDTR & ~TIM_BDTR_MOE) | TIM_BDTR_OSSI;
    }
}

void HardFault_Handler(void) {
    __disable_irq();
    /* Direct register cutoff avoids stack push; OSSI=1 + MOE=0 drives PA0 actively LOW instead of Hi-Z. */
    if ((RCC->APBENR2 & RCC_APBENR2_TIM1EN) != 0U) {
        TIM1->CCR3 = 0U;
        TIM1->BDTR = (TIM1->BDTR & ~TIM_BDTR_MOE) | TIM_BDTR_OSSI;
    }
    while (1) {
    }
}

void EXTI4_15_IRQHandler(void) {
    if (LL_EXTI_IsActiveFlag(LL_EXTI_LINE_4) != 0U) {
        LL_EXTI_ClearFlag(LL_EXTI_LINE_4);
        bsp_alert_exti_isr();
    }
}
