#include "bsp_board.h"
#include "main.h"
#include "py32f0xx_it.h"

void NMI_Handler(void) {
    bsp_pwm_set_duty_permille(0U);
}

void HardFault_Handler(void) {
    bsp_pwm_set_duty_permille(0U);
    __disable_irq();
    while (1) {
    }
}

void EXTI4_15_IRQHandler(void) {
    if (LL_EXTI_IsActiveFlag(LL_EXTI_LINE_4) != 0U) {
        LL_EXTI_ClearFlag(LL_EXTI_LINE_4);
        bsp_alert_exti_isr();
    }
}
