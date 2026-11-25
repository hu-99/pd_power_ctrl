#include "bsp_board.h"
#include "main.h"
#include <stddef.h>
#include <string.h>

#define I2C_WAIT_TIMEOUT    12000UL
#define FLASH_WAIT_TIMEOUT  1000000UL
#define ADC_CAL_TIMEOUT     100000UL
#define UART_RX_RING_SIZE   32U /* Must be power of 2 */

static uint8_t s_reset_cause_mask;
static volatile uint32_t s_adc_dma_buf[BSP_ADC_DMA_LEN];
static volatile bool s_alert_exti_enabled;
static volatile bool s_alert_irq_latched;

static volatile uint8_t s_rx_ring[UART_RX_RING_SIZE];
static volatile uint8_t s_rx_head;
static volatile uint8_t s_rx_tail;

static char s_line_acc[UART_RX_RING_SIZE];
static uint8_t s_line_len;

static void clock_init_24mhz(void) {
    LL_RCC_HSI_Enable();
    LL_RCC_HSI_SetCalibFreq(LL_RCC_HSICALIBRATION_24MHz);
    while (LL_RCC_HSI_IsReady() != 1U) {
    }

    LL_RCC_SetAHBPrescaler(LL_RCC_SYSCLK_DIV_1);
    LL_RCC_SetSysClkSource(LL_RCC_SYS_CLKSOURCE_HSISYS);
    while (LL_RCC_GetSysClkSource() != LL_RCC_SYS_CLKSOURCE_STATUS_HSISYS) {
    }

    LL_FLASH_SetLatency(LL_FLASH_LATENCY_0);
    LL_RCC_SetAPB1Prescaler(LL_RCC_APB1_DIV_1);
    LL_SetSystemCoreClock(24000000U);
}

static void led_pb5_init(void) {
    LL_GPIO_InitTypeDef gpio = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOB);
    LL_GPIO_SetOutputPin(GPIOB, LL_GPIO_PIN_5);
    gpio.Pin = LL_GPIO_PIN_5;
    gpio.Mode = LL_GPIO_MODE_OUTPUT;
    gpio.Speed = LL_GPIO_SPEED_FREQ_LOW;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOB, &gpio);
}

static void alert_pa4_init(void) {
    LL_GPIO_InitTypeDef gpio = {0};
    LL_EXTI_InitTypeDef exti = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    gpio.Pin = LL_GPIO_PIN_4;
    gpio.Mode = LL_GPIO_MODE_INPUT;
    gpio.Pull = LL_GPIO_PULL_UP;
    LL_GPIO_Init(GPIOA, &gpio);

    /* Route PA4 -> EXTI4 falling-edge IRQ for microsecond PWM cutoff when INA226 ALE asserts. */
    LL_EXTI_SetEXTISource(LL_EXTI_CONFIG_PORTA, LL_EXTI_CONFIG_LINE4);
    exti.Line = LL_EXTI_LINE_4;
    exti.LineCommand = ENABLE;
    exti.Mode = LL_EXTI_MODE_IT;
    exti.Trigger = LL_EXTI_TRIGGER_FALLING;
    LL_EXTI_Init(&exti);
    LL_EXTI_DisableIT(LL_EXTI_LINE_4);
    LL_EXTI_ClearFlag(LL_EXTI_LINE_4);

    NVIC_SetPriority(EXTI4_15_IRQn, 0U);
    NVIC_EnableIRQ(EXTI4_15_IRQn);
}

static void usart2_pa2_pa3_init(void) {
    LL_GPIO_InitTypeDef gpio = {0};
    LL_USART_InitTypeDef usart = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_USART2);

    gpio.Pin = LL_GPIO_PIN_2 | LL_GPIO_PIN_3;
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull = LL_GPIO_PULL_UP;
    gpio.Alternate = LL_GPIO_AF4_USART2;
    LL_GPIO_Init(GPIOA, &gpio);

    usart.BaudRate = 115200U;
    usart.DataWidth = LL_USART_DATAWIDTH_8B;
    usart.StopBits = LL_USART_STOPBITS_1;
    usart.Parity = LL_USART_PARITY_NONE;
    usart.TransferDirection = LL_USART_DIRECTION_TX_RX;
    usart.HardwareFlowControl = LL_USART_HWCONTROL_NONE;
    usart.OverSampling = LL_USART_OVERSAMPLING_16;
    LL_USART_Init(USART2, &usart);
    LL_USART_ConfigAsyncMode(USART2);
    LL_USART_EnableIT_RXNE(USART2);
    LL_USART_Enable(USART2);

    NVIC_SetPriority(USART2_IRQn, 2U);
    NVIC_EnableIRQ(USART2_IRQn);
}

static void tim1_ch3_pa0_pwm_init(void) {
    LL_GPIO_InitTypeDef gpio = {0};
    LL_TIM_OC_InitTypeDef oc = {0};
    LL_TIM_InitTypeDef tim = {0};

    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_TIM1);
    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);

    gpio.Pin = LL_GPIO_PIN_0;
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    /* External 10k pull-down on MOSFET TRIG/PWM provides reset/Hi-Z safety; internal pull on push-pull is unused. */
    gpio.Pull = LL_GPIO_PULL_NO;
    gpio.Alternate = LL_GPIO_AF13_TIM1;
    LL_GPIO_Init(GPIOA, &gpio);

    tim.ClockDivision = LL_TIM_CLOCKDIVISION_DIV1;
    tim.CounterMode = LL_TIM_COUNTERMODE_UP;
    tim.Prescaler = 0U;
    tim.Autoreload = BSP_PWM_PERIOD - 1U;
    tim.RepetitionCounter = 0U;
    LL_TIM_Init(TIM1, &tim);
    LL_TIM_EnableARRPreload(TIM1);

    oc.OCMode = LL_TIM_OCMODE_PWM1;
    oc.OCState = LL_TIM_OCSTATE_ENABLE;
    oc.OCPolarity = LL_TIM_OCPOLARITY_HIGH;
    oc.OCIdleState = LL_TIM_OCIDLESTATE_LOW;
    oc.CompareValue = 0U;
    LL_TIM_OC_Init(TIM1, LL_TIM_CHANNEL_CH3, &oc);
    LL_TIM_OC_EnablePreload(TIM1, LL_TIM_CHANNEL_CH3);

    LL_TIM_EnableAllOutputs(TIM1);
    LL_TIM_EnableCounter(TIM1);
}

static void i2c1_pb6_pb7_init(void) {
    LL_GPIO_InitTypeDef gpio = {0};
    LL_I2C_InitTypeDef i2c = {0};

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOB);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_I2C1);

    gpio.Pin = LL_GPIO_PIN_6 | LL_GPIO_PIN_7;
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_OPENDRAIN;
    gpio.Pull = LL_GPIO_PULL_UP;
    gpio.Alternate = LL_GPIO_AF6_I2C;
    LL_GPIO_Init(GPIOB, &gpio);

    LL_APB1_GRP1_ForceReset(LL_APB1_GRP1_PERIPH_I2C1);
    LL_APB1_GRP1_ReleaseReset(LL_APB1_GRP1_PERIPH_I2C1);

    i2c.ClockSpeed = 100000U;
    i2c.DutyCycle = LL_I2C_DUTYCYCLE_2;
    i2c.OwnAddress1 = 0x00U;
    i2c.TypeAcknowledge = LL_I2C_NACK;
    LL_I2C_Init(I2C1, &i2c);
}

static bool i2c_wait_flag(uint32_t (*check_fn)(I2C_TypeDef *)) {
    uint32_t timeout = I2C_WAIT_TIMEOUT;
    while (check_fn(I2C1) != 1U) {
        if (LL_I2C_IsActiveFlag_AF(I2C1) == 1U) {
            return false;
        }
        if (--timeout == 0UL) {
            return false;
        }
    }
    return true;
}

static bool i2c_wait_clear_busy(void) {
    uint32_t timeout = I2C_WAIT_TIMEOUT;
    while (LL_I2C_IsActiveFlag_BUSY(I2C1) == 1U) {
        if (--timeout == 0UL) {
            /* Recover wedged peripheral state cleanly after hot-plug glitches. */
            i2c1_pb6_pb7_init();
            return false;
        }
    }
    return true;
}

static bool i2c_abort_stop(void) {
    LL_I2C_ClearFlag_AF(I2C1);
    LL_I2C_GenerateStopCondition(I2C1);
    return false;
}

static bool bsp_i2c1_write_reg(void *ctx,
                               uint8_t addr7,
                               uint8_t reg,
                               const uint8_t *buf,
                               uint8_t len) {
    (void)ctx;
    if ((buf == NULL) && (len > 0U)) {
        return false;
    }
    if (!i2c_wait_clear_busy()) {
        return false;
    }

    LL_I2C_DisableBitPOS(I2C1);
    LL_I2C_GenerateStartCondition(I2C1);
    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_SB)) {
        return i2c_abort_stop();
    }

    LL_I2C_TransmitData8(I2C1, (uint8_t)(addr7 << 1U));
    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_ADDR)) {
        return i2c_abort_stop();
    }
    LL_I2C_ClearFlag_ADDR(I2C1);

    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_TXE)) {
        return i2c_abort_stop();
    }
    LL_I2C_TransmitData8(I2C1, reg);

    for (uint8_t i = 0U; i < len; i++) {
        if (!i2c_wait_flag(LL_I2C_IsActiveFlag_TXE)) {
            return i2c_abort_stop();
        }
        LL_I2C_TransmitData8(I2C1, buf[i]);
    }

    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_BTF)) {
        return i2c_abort_stop();
    }
    LL_I2C_GenerateStopCondition(I2C1);
    return true;
}

static bool bsp_i2c1_read_reg(void *ctx,
                              uint8_t addr7,
                              uint8_t reg,
                              uint8_t *buf,
                              uint8_t len) {
    (void)ctx;
    if ((buf == NULL) || (len == 0U)) {
        return false;
    }
    if (!i2c_wait_clear_busy()) {
        return false;
    }

    LL_I2C_DisableBitPOS(I2C1);
    LL_I2C_GenerateStartCondition(I2C1);
    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_SB)) {
        return i2c_abort_stop();
    }

    LL_I2C_TransmitData8(I2C1, (uint8_t)(addr7 << 1U));
    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_ADDR)) {
        return i2c_abort_stop();
    }
    LL_I2C_ClearFlag_ADDR(I2C1);

    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_TXE)) {
        return i2c_abort_stop();
    }
    LL_I2C_TransmitData8(I2C1, reg);
    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_BTF)) {
        return i2c_abort_stop();
    }

    /* Repeated START for register read phase. */
    LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_ACK);
    LL_I2C_GenerateStartCondition(I2C1);
    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_SB)) {
        return i2c_abort_stop();
    }

    LL_I2C_TransmitData8(I2C1, (uint8_t)((addr7 << 1U) | 0x01U));
    if (!i2c_wait_flag(LL_I2C_IsActiveFlag_ADDR)) {
        return i2c_abort_stop();
    }

    if (len == 1U) {
        LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
        __disable_irq();
        LL_I2C_ClearFlag_ADDR(I2C1);
        LL_I2C_GenerateStopCondition(I2C1);
        __enable_irq();

        if (!i2c_wait_flag(LL_I2C_IsActiveFlag_RXNE)) {
            return i2c_abort_stop();
        }
        buf[0] = LL_I2C_ReceiveData8(I2C1);
        return true;
    }

    if (len == 2U) {
        LL_I2C_EnableBitPOS(I2C1);
        __disable_irq();
        LL_I2C_ClearFlag_ADDR(I2C1);
        LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
        __enable_irq();

        if (!i2c_wait_flag(LL_I2C_IsActiveFlag_BTF)) {
            return i2c_abort_stop();
        }
        __disable_irq();
        LL_I2C_GenerateStopCondition(I2C1);
        buf[0] = LL_I2C_ReceiveData8(I2C1);
        __enable_irq();
        buf[1] = LL_I2C_ReceiveData8(I2C1);
        return true;
    }

    LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_ACK);
    LL_I2C_ClearFlag_ADDR(I2C1);
    for (uint8_t rem = len; rem > 0U; rem--) {
        if (rem == 1U) {
            LL_I2C_AcknowledgeNextData(I2C1, LL_I2C_NACK);
            LL_I2C_GenerateStopCondition(I2C1);
        }
        if (!i2c_wait_flag(LL_I2C_IsActiveFlag_RXNE)) {
            return i2c_abort_stop();
        }
        *buf++ = LL_I2C_ReceiveData8(I2C1);
    }
    return true;
}

static const i2c_bus_ops_t s_i2c1_bus = {
    bsp_i2c1_write_reg,
    bsp_i2c1_read_reg,
    NULL
};

void USART2_IRQHandler(void) {
    if (LL_USART_IsActiveFlag_ORE(USART2) == 1U) {
        LL_USART_ClearFlag_ORE(USART2);
    }
    if (LL_USART_IsActiveFlag_RXNE(USART2) == 1U) {
        uint8_t ch = LL_USART_ReceiveData8(USART2);
        uint8_t next = (uint8_t)((s_rx_head + 1U) & (UART_RX_RING_SIZE - 1U));
        if (next != s_rx_tail) {
            s_rx_ring[s_rx_head] = ch;
            s_rx_head = next;
        }
    }
}

static uint8_t capture_and_clear_reset_cause(void) {
    uint8_t mask = 0U;
    if (LL_RCC_IsActiveFlag_PWRRST() == 1U) {
        mask |= BSP_RST_PWR;
    }
    if (LL_RCC_IsActiveFlag_PINRST() == 1U) {
        mask |= BSP_RST_PIN;
    }
    if (LL_RCC_IsActiveFlag_SFTRST() == 1U) {
        mask |= BSP_RST_SFT;
    }
    if (LL_RCC_IsActiveFlag_IWDGRST() == 1U) {
        mask |= BSP_RST_IWDG;
    }
    if (LL_RCC_IsActiveFlag_WWDGRST() == 1U) {
        mask |= BSP_RST_WWDG;
    }
    LL_RCC_ClearResetFlags();
    return mask;
}

static void delay_loops(uint32_t loops) {
    for (volatile uint32_t i = 0U; i < loops; i++) {
        __NOP();
    }
}

static void sort_i32_5(int32_t arr[5]) {
    for (uint8_t i = 0U; i < 5U; i++) {
        int32_t min_v = arr[i];
        uint8_t pos = i;
        for (uint8_t j = (uint8_t)(i + 1U); j < 5U; j++) {
            if (min_v > arr[j]) {
                min_v = arr[j];
                pos = j;
            }
        }
        arr[pos] = arr[i];
        arr[i] = min_v;
    }
}

static void adc_calibrate_py32(void) {
    uint32_t backup_dma;
    int32_t cal_vals[5];

    if (LL_ADC_IsEnabled(ADC1) != 0U) {
        return;
    }

    backup_dma = LL_ADC_REG_GetDMATransfer(ADC1);
    LL_ADC_REG_SetDMATransfer(ADC1, LL_ADC_REG_DMA_TRANSFER_NONE);

    for (uint8_t i = 0U; i < 5U; i++) {
        uint32_t timeout = ADC_CAL_TIMEOUT;
        uint32_t raw;
        LL_ADC_StartCalibration(ADC1);
        while ((LL_ADC_IsCalibrationOnGoing(ADC1) != 0U) && (--timeout > 0UL)) {
        }
        raw = *(__IO uint32_t *)0x40012448UL;
        cal_vals[i] = (int32_t)(raw << 9);
    }

    sort_i32_5(cal_vals);
    *(__IO uint32_t *)0x40012450UL = (uint32_t)(cal_vals[2] >> 9);
    *(__IO uint32_t *)0x40012454UL = *(__IO uint32_t *)0x4001244CUL;
    *(__IO uint32_t *)0x40012444UL |= 0x8000UL;
    delay_loops(6000UL);

    LL_ADC_REG_SetDMATransfer(ADC1, backup_dma);
}

static void adc1_dma1_init(void) {
    LL_ADC_InitTypeDef adc = {0};
    LL_ADC_REG_InitTypeDef adc_reg = {0};

    LL_ADC_Reset(ADC1);
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_ADC1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

    adc_calibrate_py32();

    /* DMA1_Channel1: circular periph->memory transfer [CH11_TS, CH12_VREF] x 4 frames. */
    LL_SYSCFG_SetDMARemap_CH1(LL_SYSCFG_DMA_MAP_ADC);
    LL_DMA_SetDataTransferDirection(DMA1, LL_DMA_CHANNEL_1, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    LL_DMA_SetChannelPriorityLevel(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PRIORITY_LOW);
    LL_DMA_SetMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MODE_CIRCULAR);
    LL_DMA_SetPeriphIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PERIPH_NOINCREMENT);
    LL_DMA_SetMemoryIncMode(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MEMORY_INCREMENT);
    LL_DMA_SetPeriphSize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_PDATAALIGN_WORD);
    LL_DMA_SetMemorySize(DMA1, LL_DMA_CHANNEL_1, LL_DMA_MDATAALIGN_WORD);
    LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_1, BSP_ADC_DMA_LEN);
    LL_DMA_ConfigAddresses(DMA1, LL_DMA_CHANNEL_1, (uint32_t)&ADC1->DR,
                           (uint32_t)s_adc_dma_buf, LL_DMA_DIRECTION_PERIPH_TO_MEMORY);
    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_1);

    LL_ADC_SetCommonPathInternalCh(__LL_ADC_COMMON_INSTANCE(ADC1),
                                   LL_ADC_PATH_INTERNAL_VREFINT |
                                   LL_ADC_PATH_INTERNAL_TEMPSENSOR);
    delay_loops(4000UL); /* Allow >=120us internal VREFINT/TS stabilization */

    adc.Clock = LL_ADC_CLOCK_SYNC_PCLK_DIV8; /* 24MHz / 8 = 3MHz */
    adc.Resolution = LL_ADC_RESOLUTION_12B;
    adc.DataAlignment = LL_ADC_DATA_ALIGN_RIGHT;
    adc.LowPowerMode = LL_ADC_LP_MODE_NONE;
    LL_ADC_Init(ADC1, &adc);
    LL_ADC_SetSamplingTimeCommonChannels(ADC1, LL_ADC_SAMPLINGTIME_239CYCLES_5); /* ~80us >= 15us */

    adc_reg.TriggerSource = LL_ADC_REG_TRIG_SOFTWARE;
    adc_reg.ContinuousMode = LL_ADC_REG_CONV_CONTINUOUS;
    adc_reg.DMATransfer = LL_ADC_REG_DMA_TRANSFER_UNLIMITED;
    adc_reg.Overrun = LL_ADC_REG_OVR_DATA_OVERWRITTEN;
    adc_reg.SequencerDiscont = LL_ADC_REG_SEQ_DISCONT_DISABLE;
    LL_ADC_REG_Init(ADC1, &adc_reg);

    LL_ADC_REG_SetSequencerScanDirection(ADC1, LL_ADC_REG_SEQ_SCAN_DIR_FORWARD);
    LL_ADC_REG_SetSequencerChannels(ADC1, LL_ADC_CHANNEL_TEMPSENSOR | LL_ADC_CHANNEL_VREFINT);

    LL_ADC_Enable(ADC1);
    delay_loops(6000UL);
    LL_ADC_REG_StartConversion(ADC1);
    delay_loops(24000UL); /* Pre-fill all 8 DMA circular slots before tasks start */
}

static void tft_spi1_dma2_init(void) {
    LL_GPIO_InitTypeDef gpio;
    LL_SPI_InitTypeDef spi;
    LL_DMA_InitTypeDef dma;

    LL_IOP_GRP1_EnableClock(LL_IOP_GRP1_PERIPH_GPIOA);
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SPI1);
    LL_AHB1_GRP1_EnableClock(LL_AHB1_GRP1_PERIPH_DMA1);

    /* Control pins: PA1 (RES), PA6 (DC), PA12 (CS) idle HIGH */
    LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_1 | LL_GPIO_PIN_6 | LL_GPIO_PIN_12);
    LL_GPIO_StructInit(&gpio);
    gpio.Pin = LL_GPIO_PIN_1 | LL_GPIO_PIN_6 | LL_GPIO_PIN_12;
    gpio.Mode = LL_GPIO_MODE_OUTPUT;
    gpio.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull = LL_GPIO_PULL_NO;
    LL_GPIO_Init(GPIOA, &gpio);

    /* SPI1 pins: PA5 (SCK, AF0), PA7 (MOSI, AF0) */
    LL_GPIO_StructInit(&gpio);
    gpio.Pin = LL_GPIO_PIN_5 | LL_GPIO_PIN_7;
    gpio.Mode = LL_GPIO_MODE_ALTERNATE;
    gpio.Speed = LL_GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.OutputType = LL_GPIO_OUTPUT_PUSHPULL;
    gpio.Pull = LL_GPIO_PULL_NO;
    gpio.Alternate = LL_GPIO_AF_0;
    LL_GPIO_Init(GPIOA, &gpio);

    /* DMA1_Channel2 mapped to SPI1_TX (DMA1_Channel1 is reserved for ADC1) */
    LL_SYSCFG_SetDMARemap_CH2(LL_SYSCFG_DMA_MAP_SPI1_TX);
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_2);
    LL_DMA_ClearFlag_GI2(DMA1);
    LL_DMA_StructInit(&dma);
    dma.PeriphOrM2MSrcAddress = LL_SPI_DMA_GetRegAddr(SPI1);
    dma.MemoryOrM2MDstAddress = 0UL;
    dma.Direction = LL_DMA_DIRECTION_MEMORY_TO_PERIPH;
    dma.Mode = LL_DMA_MODE_NORMAL;
    dma.PeriphOrM2MSrcIncMode = LL_DMA_PERIPH_NOINCREMENT;
    dma.MemoryOrM2MDstIncMode = LL_DMA_MEMORY_INCREMENT;
    dma.PeriphOrM2MSrcDataSize = LL_DMA_PDATAALIGN_BYTE;
    dma.MemoryOrM2MDstDataSize = LL_DMA_MDATAALIGN_BYTE;
    dma.NbData = 0UL;
    dma.Priority = LL_DMA_PRIORITY_MEDIUM;
    LL_DMA_Init(DMA1, LL_DMA_CHANNEL_2, &dma);

    /* SPI1 Master Mode 0 (CPOL=0, CPHA=1EDGE), 24MHz / 2 = 12MHz, 8-bit MSB first */
    LL_SPI_Disable(SPI1);
    LL_SPI_StructInit(&spi);
    spi.TransferDirection = LL_SPI_FULL_DUPLEX;
    spi.Mode = LL_SPI_MODE_MASTER;
    spi.DataWidth = LL_SPI_DATAWIDTH_8BIT;
    spi.ClockPolarity = LL_SPI_POLARITY_LOW;
    spi.ClockPhase = LL_SPI_PHASE_1EDGE;
    spi.NSS = LL_SPI_NSS_SOFT;
    spi.BaudRate = LL_SPI_BAUDRATEPRESCALER_DIV2;
    spi.BitOrder = LL_SPI_MSB_FIRST;
    LL_SPI_Init(SPI1, &spi);
    LL_SPI_Enable(SPI1);
}

void bsp_board_init(void) {
    LL_APB1_GRP2_EnableClock(LL_APB1_GRP2_PERIPH_SYSCFG);
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_PWR);

    s_reset_cause_mask = capture_and_clear_reset_cause();

    clock_init_24mhz();
    led_pb5_init();
    alert_pa4_init();
    usart2_pa2_pa3_init();
    tim1_ch3_pa0_pwm_init();
    i2c1_pb6_pb7_init();
    adc1_dma1_init();
    tft_spi1_dma2_init();
}

uint8_t bsp_get_reset_cause_mask(void) {
    return s_reset_cause_mask;
}

void bsp_iwdg_init(void) {
    LL_APB1_GRP1_EnableClock(LL_APB1_GRP1_PERIPH_DBGMCU);
    LL_DBGMCU_APB1_GRP1_FreezePeriph(LL_DBGMCU_APB1_GRP1_IWDG_STOP);

    LL_RCC_LSI_Enable();
    while (LL_RCC_LSI_IsReady() == 0U) {
    }

    LL_IWDG_Enable(IWDG);
    LL_IWDG_EnableWriteAccess(IWDG);
    /* LSI (~32.768kHz) / 32 = 1024 Hz -> reload 2048 ticks ~= 2.00 s timeout. */
    LL_IWDG_SetPrescaler(IWDG, LL_IWDG_PRESCALER_32);
    LL_IWDG_SetReloadCounter(IWDG, 2048U);
    while (LL_IWDG_IsReady(IWDG) == 0U) {
    }
    LL_IWDG_ReloadCounter(IWDG);
}

void bsp_iwdg_feed(void) {
    LL_IWDG_ReloadCounter(IWDG);
}

bool bsp_crumb_consume(bsp_wdg_crumb_t *out_crumb) {
    volatile bsp_wdg_crumb_t *slot = (volatile bsp_wdg_crumb_t *)BSP_CRUMB_ADDR;
    bsp_wdg_crumb_t local;
    uint32_t calc_crc;

    memcpy(&local, (const void *)slot, sizeof(local));
    memset((void *)slot, 0, sizeof(local));

    if ((out_crumb == NULL) || (local.magic != BSP_CRUMB_MAGIC)) {
        return false;
    }

    calc_crc = bbox_crc32((const uint8_t *)&local,
                          (uint32_t)(sizeof(bsp_wdg_crumb_t) - sizeof(uint32_t)));
    if (calc_crc != local.crc32) {
        return false;
    }

    *out_crumb = local;
    return true;
}

void bsp_crumb_update(uint32_t uptime_ms,
                      uint16_t vbus_mv,
                      uint16_t ibus_ma,
                      uint16_t duty_permille,
                      uint8_t alive_mask,
                      uint8_t flags) {
    volatile bsp_wdg_crumb_t *slot = (volatile bsp_wdg_crumb_t *)BSP_CRUMB_ADDR;
    bsp_wdg_crumb_t local;

    local.magic = BSP_CRUMB_MAGIC;
    local.uptime_ms = uptime_ms;
    local.vbus_mv = vbus_mv;
    local.ibus_ma = ibus_ma;
    local.duty_permille = duty_permille;
    local.alive_mask = alive_mask;
    local.flags = flags;
    local.crc32 = bbox_crc32((const uint8_t *)&local,
                             (uint32_t)(sizeof(bsp_wdg_crumb_t) - sizeof(uint32_t)));
    memcpy((void *)slot, &local, sizeof(local));
}

static uint32_t bbox_slot_addr(uint8_t slot) {
    return (slot == 0U) ? BSP_BBOX_SLOT0_ADDR : BSP_BBOX_SLOT1_ADDR;
}

static bool bsp_flash_wait_not_busy(void) {
    uint32_t timeout = FLASH_WAIT_TIMEOUT;
    while (LL_FLASH_IsActiveFlag_BUSY(FLASH) == 1U) {
        if (--timeout == 0UL) {
            return false;
        }
    }
    return true;
}

static bool bsp_flash_read_page(void *ctx, uint8_t slot, uint8_t *buf_128) {
    (void)ctx;
    if ((slot >= 2U) || (buf_128 == NULL)) {
        return false;
    }
    memcpy(buf_128, (const void *)bbox_slot_addr(slot), BBOX_PAGE_SIZE);
    return true;
}

static bool bsp_flash_erase_and_write_page(void *ctx, uint8_t slot, const uint8_t *buf_128) {
    uint32_t page_words[BBOX_PAGE_SIZE / 4U];
    uint32_t addr;
    uint32_t primask;
    bool ok = false;

    (void)ctx;
    if ((slot >= 2U) || (buf_128 == NULL)) {
        return false;
    }

    memcpy(page_words, buf_128, BBOX_PAGE_SIZE);
    addr = bbox_slot_addr(slot);

    /* Mask IRQs during Flash unlock/erase/program so no ISR fetches from stalled Flash. */
    primask = __get_PRIMASK();
    __disable_irq();

    LL_FLASH_Unlock(FLASH);
    LL_FLASH_TIMMING_SEQUENCE_CONFIG_24M();

    if (LL_FLASH_IsActiveFlag_WRPERR(FLASH) == 1U) {
        LL_FLASH_ClearFlag_WRPERR(FLASH);
    }
    if (LL_FLASH_IsActiveFlag_EOP(FLASH) == 1U) {
        LL_FLASH_ClearFlag_EOP(FLASH);
    }

    if (bsp_flash_wait_not_busy()) {
        LL_FLASH_EnablePageErase(FLASH);
        LL_FLASH_SetEraseAddress(FLASH, addr);
        ok = bsp_flash_wait_not_busy();
        LL_FLASH_DisablePageErase(FLASH);
    }

    if (ok) {
        const volatile uint32_t *flash_words = (const volatile uint32_t *)addr;
        for (uint32_t i = 0U; i < (BBOX_PAGE_SIZE / 4U); i++) {
            if (flash_words[i] != 0xFFFFFFFFUL) {
                ok = false;
                break;
            }
        }
    }

    if (ok) {
        LL_FLASH_EnablePageProgram(FLASH);
        LL_FLASH_PageProgram(FLASH, addr, page_words);
        ok = bsp_flash_wait_not_busy();
        LL_FLASH_DisablePageProgram(FLASH);
    }

    LL_FLASH_Lock(FLASH);
    __set_PRIMASK(primask);

    if (ok) {
        ok = (memcmp((const void *)addr, page_words, BBOX_PAGE_SIZE) == 0);
    }
    return ok;
}

static const bbox_flash_ops_t s_bbox_flash_ops = {
    bsp_flash_read_page,
    bsp_flash_erase_and_write_page,
    NULL
};

const bbox_flash_ops_t *bsp_bbox_flash_ops(void) {
    return &s_bbox_flash_ops;
}

bool bsp_adc_read_telemetry(uint16_t *out_vdda_mv, int16_t *out_temp_c) {
    uint32_t ts_sum = 0U;
    uint32_t vref_sum = 0U;
    uint32_t ts_avg;
    uint32_t vref_avg;
    uint16_t vdda_mv;
    int16_t temp_c = 25;
    uint16_t cal1 = *TEMPSENSOR_CAL1_ADDR;
    uint16_t cal2 = *TEMPSENSOR_CAL2_ADDR;

    for (uint8_t f = 0U; f < BSP_ADC_DMA_FRAMES; f++) {
        ts_sum += (uint32_t)(s_adc_dma_buf[f * BSP_ADC_DMA_CHANS] & 0x0FFFUL);
        vref_sum += (uint32_t)(s_adc_dma_buf[(f * BSP_ADC_DMA_CHANS) + 1U] & 0x0FFFUL);
    }

    ts_avg = ts_sum / BSP_ADC_DMA_FRAMES;
    vref_avg = vref_sum / BSP_ADC_DMA_FRAMES;
    if (vref_avg == 0U) {
        return false;
    }

    vdda_mv = (uint16_t)__LL_ADC_CALC_VREFANALOG_VOLTAGE(vref_avg, LL_ADC_RESOLUTION_12B);
    if ((cal1 < 4096U) && (cal2 < 4096U) && (cal2 != cal1)) {
        temp_c = (int16_t)__LL_ADC_CALC_TEMPERATURE_85((int32_t)vdda_mv,
                                                       (int32_t)ts_avg,
                                                       LL_ADC_RESOLUTION_12B);
    } else {
        temp_c = (int16_t)__LL_ADC_CALC_TEMPERATURE_TYP_PARAMS(2500, 760, 30,
                                                               (int32_t)vdda_mv,
                                                               (int32_t)ts_avg,
                                                               LL_ADC_RESOLUTION_12B);
    }

    if (out_vdda_mv != NULL) {
        *out_vdda_mv = vdda_mv;
    }
    if (out_temp_c != NULL) {
        *out_temp_c = temp_c;
    }
    return true;
}

void bsp_led_toggle(void) {
    LL_GPIO_TogglePin(GPIOB, LL_GPIO_PIN_5);
}

void bsp_pwm_set_duty_permille(uint16_t duty_permille) {
    uint32_t primask = __get_PRIMASK();
    uint32_t clamped;
    uint32_t ccr;

    /* Check ISR latch + write CCR3 atomically so EXTI4 trip cannot be overwritten by task context. */
    __disable_irq();
    if (s_alert_irq_latched) {
        duty_permille = 0U;
    }
    clamped = (duty_permille > 1000U) ? 1000U : duty_permille;
    ccr = (clamped * BSP_PWM_PERIOD) / 1000U;
    LL_TIM_OC_SetCompareCH3(TIM1, ccr);
    __set_PRIMASK(primask);
}

void bsp_alert_exti_set_enabled(bool enable) {
    if (s_alert_exti_enabled == enable) {
        return;
    }
    s_alert_exti_enabled = enable;
    if (enable) {
        LL_EXTI_ClearFlag(LL_EXTI_LINE_4);
        LL_EXTI_EnableIT(LL_EXTI_LINE_4);
    } else {
        LL_EXTI_DisableIT(LL_EXTI_LINE_4);
        LL_EXTI_ClearFlag(LL_EXTI_LINE_4);
        /* Do NOT clear s_alert_irq_latched here: transient I2C errors must not mask EXTI4 fault history. */
    }
}

void bsp_alert_exti_isr(void) {
    if (s_alert_exti_enabled && (LL_GPIO_IsInputPinSet(GPIOA, LL_GPIO_PIN_4) == 0U)) {
        TIM1->CCR3 = 0U;
        s_alert_irq_latched = true;
    }
}

void bsp_alert_clear_latch(void) {
    uint32_t primask = __get_PRIMASK();
    __disable_irq();
    s_alert_irq_latched = false;
    __set_PRIMASK(primask);
}

bool bsp_alert_is_active(void) {
    if (s_alert_irq_latched) {
        return true;
    }
    if (!s_alert_exti_enabled) {
        return false;
    }
    return (LL_GPIO_IsInputPinSet(GPIOA, LL_GPIO_PIN_4) == 0U);
}

const i2c_bus_ops_t *bsp_i2c1_bus(void) {
    return &s_i2c1_bus;
}

void bsp_uart_puts(const char *str) {
    if (str == NULL) {
        return;
    }
    while (*str != '\0') {
        while (LL_USART_IsActiveFlag_TXE(USART2) != 1U) {
        }
        LL_USART_TransmitData8(USART2, (uint8_t)(*str));
        str++;
    }
    while (LL_USART_IsActiveFlag_TC(USART2) != 1U) {
    }
}

void bsp_uart_put_u32(uint32_t val) {
    char buf[11];
    uint8_t idx = 0U;

    if (val == 0U) {
        bsp_uart_puts("0");
        return;
    }
    while (val > 0U) {
        buf[idx++] = (char)('0' + (val % 10U));
        val /= 10U;
    }
    while (idx > 0U) {
        char ch[2];
        idx--;
        ch[0] = buf[idx];
        ch[1] = '\0';
        bsp_uart_puts(ch);
    }
}

void bsp_uart_put_i32(int32_t val) {
    if (val < 0) {
        bsp_uart_puts("-");
        bsp_uart_put_u32((uint32_t)(-val));
    } else {
        bsp_uart_put_u32((uint32_t)val);
    }
}

bool bsp_uart_pop_line(char *out_buf, uint8_t max_len) {
    if ((out_buf == NULL) || (max_len < 2U)) {
        return false;
    }

    while (s_rx_tail != s_rx_head) {
        char c = (char)s_rx_ring[s_rx_tail];
        s_rx_tail = (uint8_t)((s_rx_tail + 1U) & (UART_RX_RING_SIZE - 1U));

        if ((c == '\r') || (c == '\n')) {
            if (s_line_len > 0U) {
                uint8_t copy_n = (s_line_len < (uint8_t)(max_len - 1U))
                                     ? s_line_len
                                     : (uint8_t)(max_len - 1U);
                for (uint8_t i = 0U; i < copy_n; i++) {
                    out_buf[i] = s_line_acc[i];
                }
                out_buf[copy_n] = '\0';
                s_line_len = 0U;
                return true;
            }
        } else if (s_line_len < (uint8_t)(UART_RX_RING_SIZE - 1U)) {
            s_line_acc[s_line_len++] = c;
        }
    }
    return false;
}

void bsp_tft_set_res(bool high) {
    if (high) {
        LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_1);
    } else {
        LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_1);
    }
}

void bsp_tft_set_dc(bool is_data) {
    if (is_data) {
        LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_6);
    } else {
        LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_6);
    }
}

void bsp_tft_set_cs(bool high) {
    if (high) {
        LL_GPIO_SetOutputPin(GPIOA, LL_GPIO_PIN_12);
    } else {
        LL_GPIO_ResetOutputPin(GPIOA, LL_GPIO_PIN_12);
    }
}

void bsp_tft_tx_byte(uint8_t byte) {
    uint32_t guard = 20000UL;
    while ((LL_SPI_IsActiveFlag_TXE(SPI1) == 0U) && (guard > 0UL)) {
        guard--;
    }
    LL_SPI_TransmitData8(SPI1, byte);
    guard = 20000UL;
    while (((LL_SPI_IsActiveFlag_TXE(SPI1) == 0U) || (LL_SPI_IsActiveFlag_BSY(SPI1) != 0U)) &&
           (guard > 0UL)) {
        guard--;
    }
}

void bsp_tft_tx_dma(const uint8_t *buf, uint16_t len) {
    uint32_t guard;

    if ((buf == NULL) || (len == 0U)) {
        return;
    }

    guard = 20000UL;
    while ((LL_SPI_IsActiveFlag_BSY(SPI1) != 0U) && (guard > 0UL)) {
        guard--;
    }

    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_2);
    LL_DMA_ClearFlag_GI2(DMA1);
    LL_DMA_ConfigAddresses(DMA1,
                           LL_DMA_CHANNEL_2,
                           (uint32_t)buf,
                           LL_SPI_DMA_GetRegAddr(SPI1),
                           LL_DMA_DIRECTION_MEMORY_TO_PERIPH);
    LL_DMA_SetDataLength(DMA1, LL_DMA_CHANNEL_2, len);
    LL_DMA_EnableChannel(DMA1, LL_DMA_CHANNEL_2);
    LL_SPI_EnableDMAReq_TX(SPI1);

    guard = 200000UL;
    while ((LL_DMA_IsActiveFlag_TC2(DMA1) == 0U) && (guard > 0UL)) {
        guard--;
    }

    guard = 20000UL;
    while (((LL_SPI_IsActiveFlag_TXE(SPI1) == 0U) || (LL_SPI_IsActiveFlag_BSY(SPI1) != 0U)) &&
           (guard > 0UL)) {
        guard--;
    }

    LL_SPI_DisableDMAReq_TX(SPI1);
    LL_DMA_DisableChannel(DMA1, LL_DMA_CHANNEL_2);
    LL_DMA_ClearFlag_GI2(DMA1);
}
