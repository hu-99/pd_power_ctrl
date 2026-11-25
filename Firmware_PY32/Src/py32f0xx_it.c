#include "bsp_board.h"
#include "bsp_board.h"
#include "main.h"
#include "py32f0xx_it.h"

void NMI_Handler(void) {
    /* Direct register write avoids function calls if SP/stack is compromised. */
    if ((RCC->APBENR2 & RCC_APBENR2_TIM1EN) != 0U) {
        TIM1->CCR3 = 0U;
        TIM1->BDTR &= ~TIM_BDTR_MOE;
    }
}

void HardFault_Handler(void) {
    __disable_irq();
    /* Direct register cutoff avoids stack push so stacked corruption cannot lock up core. */
    if ((RCC->APBENR2 & RCC_APBENR2_TIM1EN) != 0U) {
        TIM1->CCR3 = 0U;
        TIM1->BDTR &= ~TIM_BDTR_MOE;
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
