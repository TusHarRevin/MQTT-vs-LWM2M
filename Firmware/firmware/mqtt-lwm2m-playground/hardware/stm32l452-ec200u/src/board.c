#include "board.h"

#include <string.h>

#include "hw_config.h"

static UART_HandleTypeDef huart1; /* EC200U */
static UART_HandleTypeDef huart2; /* ST-LINK VCP */
static TIM_HandleTypeDef htim2;
static RNG_HandleTypeDef hrng;

/* ---- ring buffers ------------------------------------------------------------ */

#define MODEM_RX_SIZE 8192u
#define VCP_TX_SIZE 8192u
#define VCP_LINES 4u
#define VCP_LINE_MAX 640u

static volatile uint8_t modem_rx[MODEM_RX_SIZE];
static volatile uint32_t modem_rx_head, modem_rx_tail, modem_rx_lost;

static volatile uint8_t vcp_tx[VCP_TX_SIZE];
static volatile uint32_t vcp_tx_head, vcp_tx_tail;

/* Complete lines received from the PC; the ISR fills, the main loop pops. */
static char vcp_lines[VCP_LINES][VCP_LINE_MAX];
static volatile uint32_t vcp_line_head, vcp_line_tail;
static char vcp_cur[VCP_LINE_MAX];
static volatile uint32_t vcp_cur_len;

static volatile uint32_t tim2_wraps;

/* ---- clocks ------------------------------------------------------------------ */

static void fatal(void)
{
    __disable_irq();
    for (;;) {
    }
}

static void clock_init(void)
{
    RCC_OscInitTypeDef osc = { 0 };
    RCC_ClkInitTypeDef clk = { 0 };
    RCC_PeriphCLKInitTypeDef per = { 0 };

    __HAL_RCC_PWR_CLK_ENABLE();
    if (HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1) != HAL_OK) {
        fatal();
    }
    /* SYSCLK 80 MHz = MSI 4 MHz * 40 / 2 (as the Cube template), plus HSI48
     * for the RNG. */
    osc.OscillatorType = RCC_OSCILLATORTYPE_MSI | RCC_OSCILLATORTYPE_HSI48;
    osc.MSIState = RCC_MSI_ON;
    osc.MSICalibrationValue = RCC_MSICALIBRATION_DEFAULT;
    osc.MSIClockRange = RCC_MSIRANGE_6;
    osc.HSI48State = RCC_HSI48_ON;
    osc.PLL.PLLState = RCC_PLL_ON;
    osc.PLL.PLLSource = RCC_PLLSOURCE_MSI;
    osc.PLL.PLLM = 1;
    osc.PLL.PLLN = 40;
    osc.PLL.PLLR = 2;
    osc.PLL.PLLP = 7;
    osc.PLL.PLLQ = 4;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) {
        fatal();
    }
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK) {
        fatal();
    }
    per.PeriphClockSelection = RCC_PERIPHCLK_USART1 | RCC_PERIPHCLK_USART2 | RCC_PERIPHCLK_RNG;
    per.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
    per.Usart2ClockSelection = RCC_USART2CLKSOURCE_PCLK1;
    per.RngClockSelection = RCC_RNGCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&per) != HAL_OK) {
        fatal();
    }
}

/* ---- peripherals --------------------------------------------------------------- */

void HAL_UART_MspInit(UART_HandleTypeDef *h)
{
    GPIO_InitTypeDef g = { 0 };

    g.Mode = GPIO_MODE_AF_PP;
    g.Pull = GPIO_PULLUP;
    g.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF7_USART1; /* == GPIO_AF7_USART2 */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    if (h->Instance == USART1) {
        __HAL_RCC_USART1_CLK_ENABLE();
        g.Pin = GPIO_PIN_9 | GPIO_PIN_10;
        HAL_GPIO_Init(GPIOA, &g);
        HAL_NVIC_SetPriority(USART1_IRQn, 0, 0); /* highest: never lose modem bytes */
        HAL_NVIC_EnableIRQ(USART1_IRQn);
    } else if (h->Instance == USART2) {
        __HAL_RCC_USART2_CLK_ENABLE();
        g.Pin = GPIO_PIN_2 | GPIO_PIN_3;
        HAL_GPIO_Init(GPIOA, &g);
        HAL_NVIC_SetPriority(USART2_IRQn, 2, 0);
        HAL_NVIC_EnableIRQ(USART2_IRQn);
    }
}

static void uart_init(UART_HandleTypeDef *h, USART_TypeDef *inst, uint32_t baud)
{
    h->Instance = inst;
    h->Init.BaudRate = baud;
    h->Init.WordLength = UART_WORDLENGTH_8B;
    h->Init.StopBits = UART_STOPBITS_1;
    h->Init.Parity = UART_PARITY_NONE;
    h->Init.Mode = UART_MODE_TX_RX;
    h->Init.HwFlowCtl = UART_HWCONTROL_NONE;
    h->Init.OverSampling = UART_OVERSAMPLING_16;
    h->Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    h->AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(h) != HAL_OK) {
        fatal();
    }
    /* Our own interrupt handlers below take it from here. */
    SET_BIT(inst->CR1, USART_CR1_RXNEIE);
}

static void timer_init(void)
{
    __HAL_RCC_TIM2_CLK_ENABLE();
    htim2.Instance = TIM2;
    htim2.Init.Prescaler = (SystemCoreClock / 1000000u) - 1u; /* 1 MHz */
    htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim2.Init.Period = 0xFFFFFFFFu;
    htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim2) != HAL_OK) {
        fatal();
    }
    /* HAL_TIM_Base_Init's update event sets UIF: clear it, or the first
     * interrupt counts a wrap that never happened (time would start at 4295 s). */
    __HAL_TIM_CLEAR_FLAG(&htim2, TIM_FLAG_UPDATE);
    HAL_NVIC_SetPriority(TIM2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
    HAL_TIM_Base_Start_IT(&htim2);
}

static void gpio_init(void)
{
    GPIO_InitTypeDef g = { 0 };

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    /* LD4 (green) on PB13 */
    g.Pin = GPIO_PIN_13;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &g);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET);
    /* PWRKEY stays an input (released) except while it is pulsed. */
}

void board_init(void)
{
    HAL_Init();
    clock_init();
    gpio_init();
    timer_init();
    uart_init(&huart2, USART2, VCP_BAUD);
    uart_init(&huart1, USART1, MODEM_BAUD);
    __HAL_RCC_RNG_CLK_ENABLE();
    hrng.Instance = RNG;
    if (HAL_RNG_Init(&hrng) != HAL_OK) {
        fatal();
    }
}

void board_led(bool on)
{
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void board_reset(void)
{
    vcp_flush();
    NVIC_SystemReset();
    for (;;) {
    }
}

/* ---- time ---------------------------------------------------------------------- */

uint64_t board_us(void)
{
    uint32_t primask = __get_PRIMASK();
    uint32_t hi, lo;

    __disable_irq();
    hi = tim2_wraps;
    lo = TIM2->CNT;
    /* wrapped but the interrupt hasn't run yet */
    if ((TIM2->SR & TIM_SR_UIF) && lo < 0x80000000u) {
        hi++;
    }
    __set_PRIMASK(primask);
    return ((uint64_t)hi << 32) | lo;
}

void TIM2_IRQHandler(void)
{
    if (TIM2->SR & TIM_SR_UIF) {
        TIM2->SR = ~TIM_SR_UIF;
        tim2_wraps++;
    }
}

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ---- RNG ------------------------------------------------------------------------- */

int board_rng(uint8_t *out, size_t len)
{
    while (len > 0) {
        uint32_t w;
        size_t n = len < 4 ? len : 4;
        if (HAL_RNG_GenerateRandomNumber(&hrng, &w) != HAL_OK) {
            return -1;
        }
        memcpy(out, &w, n);
        out += n;
        len -= n;
    }
    return 0;
}

/* ---- VCP (USART2) ------------------------------------------------------------------ */

void vcp_write(const void *data, size_t len)
{
    const uint8_t *p = data;

    while (len > 0) {
        uint32_t next = (vcp_tx_head + 1u) % VCP_TX_SIZE;
        if (next == vcp_tx_tail) {
            /* full: let the interrupt drain it (or drain it ourselves if
             * interrupts are off, e.g. from a fault handler) */
            if (__get_PRIMASK()) {
                while (!(USART2->ISR & USART_ISR_TXE)) {
                }
                USART2->TDR = vcp_tx[vcp_tx_tail];
                vcp_tx_tail = (vcp_tx_tail + 1u) % VCP_TX_SIZE;
            }
            continue;
        }
        vcp_tx[vcp_tx_head] = *p++;
        vcp_tx_head = next;
        len--;
        SET_BIT(USART2->CR1, USART_CR1_TXEIE);
    }
}

void vcp_flush(void)
{
    uint32_t start = HAL_GetTick();
    while ((vcp_tx_head != vcp_tx_tail || !(USART2->ISR & USART_ISR_TC)) && HAL_GetTick() - start < 2000u) {
    }
}

bool vcp_read_line(char *out, size_t cap)
{
    if (vcp_line_tail == vcp_line_head) {
        return false;
    }
    strncpy(out, vcp_lines[vcp_line_tail], cap - 1);
    out[cap - 1] = '\0';
    vcp_line_tail = (vcp_line_tail + 1u) % VCP_LINES;
    return true;
}

void USART2_IRQHandler(void)
{
    uint32_t isr = USART2->ISR;

    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        USART2->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    }
    if (isr & USART_ISR_RXNE) {
        char c = (char)USART2->RDR;
        if (c == '\r' || c == '\n') {
            if (vcp_cur_len > 0) {
                uint32_t next = (vcp_line_head + 1u) % VCP_LINES;
                if (next != vcp_line_tail) { /* else: drop the line */
                    memcpy(vcp_lines[vcp_line_head], vcp_cur, vcp_cur_len);
                    vcp_lines[vcp_line_head][vcp_cur_len] = '\0';
                    vcp_line_head = next;
                }
                vcp_cur_len = 0;
            }
        } else if (vcp_cur_len < VCP_LINE_MAX - 1u) {
            vcp_cur[vcp_cur_len++] = c;
        }
    }
    if ((USART2->CR1 & USART_CR1_TXEIE) && (isr & USART_ISR_TXE)) {
        if (vcp_tx_tail != vcp_tx_head) {
            USART2->TDR = vcp_tx[vcp_tx_tail];
            vcp_tx_tail = (vcp_tx_tail + 1u) % VCP_TX_SIZE;
        } else {
            CLEAR_BIT(USART2->CR1, USART_CR1_TXEIE);
        }
    }
}

/* ---- EC200U (USART1) ----------------------------------------------------------------- */

void modem_uart_write(const void *data, size_t len)
{
    const uint8_t *p = data;
    for (size_t i = 0; i < len; i++) {
        while (!(USART1->ISR & USART_ISR_TXE)) {
        }
        USART1->TDR = p[i];
    }
    while (!(USART1->ISR & USART_ISR_TC)) {
    }
}

int modem_uart_getc(void)
{
    uint8_t c;
    if (modem_rx_tail == modem_rx_head) {
        return -1;
    }
    c = modem_rx[modem_rx_tail];
    modem_rx_tail = (modem_rx_tail + 1u) % MODEM_RX_SIZE;
    return c;
}

void modem_uart_drop_rx(void)
{
    modem_rx_tail = modem_rx_head;
}

uint32_t modem_uart_overruns(void)
{
    return modem_rx_lost;
}

void USART1_IRQHandler(void)
{
    uint32_t isr = USART1->ISR;

    if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) {
        if (isr & USART_ISR_ORE) {
            modem_rx_lost++;
        }
        USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
    }
    if (isr & USART_ISR_RXNE) {
        uint8_t c = (uint8_t)USART1->RDR;
        uint32_t next = (modem_rx_head + 1u) % MODEM_RX_SIZE;
        if (next != modem_rx_tail) {
            modem_rx[modem_rx_head] = c;
            modem_rx_head = next;
        } else {
            modem_rx_lost++;
        }
    }
}

bool modem_pwrkey_pulse(void)
{
#ifdef MODEM_PWRKEY_PORT
    GPIO_InitTypeDef g = { 0 };

    g.Pin = MODEM_PWRKEY_PIN;
    g.Mode = GPIO_MODE_OUTPUT_PP;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_WritePin(MODEM_PWRKEY_PORT, MODEM_PWRKEY_PIN, MODEM_PWRKEY_ACTIVE);
    HAL_GPIO_Init(MODEM_PWRKEY_PORT, &g);
    HAL_Delay(MODEM_PWRKEY_PULSE_MS);
    /* release: back to input */
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(MODEM_PWRKEY_PORT, &g);
    return true;
#else
    return false;
#endif
}

/* ---- faults ------------------------------------------------------------------------------- */

static void fault_print(const char *s)
{
    while (*s) {
        while (!(USART2->ISR & USART_ISR_TXE)) {
        }
        USART2->TDR = (uint8_t)*s++;
    }
}

void HardFault_Handler(void)
{
    __disable_irq();
    fault_print("\r\n{\"k\":\"hw\",\"state\":\"fault\",\"msg\":\"HardFault - resetting\"}\r\n");
    for (volatile uint32_t i = 0; i < 4000000u; i++) {
    }
    NVIC_SystemReset();
}

void MemManage_Handler(void) { HardFault_Handler(); }
void BusFault_Handler(void) { HardFault_Handler(); }
void UsageFault_Handler(void) { HardFault_Handler(); }
void NMI_Handler(void) { }
void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }
