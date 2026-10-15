/*
 * app.c - task setup
 */
#include "app.h"
#include "app_config.h"
#include "app_log.h"
#include "net.h"
#include "echo.h"
#include "button.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdio.h>

#if !defined(ENC_INT_Pin)
#error "Set CubeMX user label ENC_INT on PA0 (GPIO_EXTI0, falling edge, pull-up)"
#endif
#if !defined(LD2_Pin) || !defined(LD2_GPIO_Port)
#error "Set CubeMX user label LD2 on PA5 (GPIO_Output)"
#endif

static volatile bool st_link;
static volatile bool st_client;

static void led_update(void)
{
    HAL_GPIO_WritePin(LD2_GPIO_Port, LD2_Pin,
                      (st_link && st_client) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void app_status_link(bool up)
{
    st_link = up;
    led_update();
}

void app_status_client(bool connected)
{
    st_client = connected;
    led_update();
}

/* Called by HAL_GPIO_EXTI_IRQHandler() from the CubeMX-generated
 * EXTIx_IRQHandler (NVIC priority >= configMAX_SYSCALL_INTERRUPT_PRIORITY).
 * Only notifies tasks; no printf, no SPI. */
void HAL_GPIO_EXTI_Callback(uint16_t pin)
{
    if (pin == ENC_INT_Pin) {
        net_enc_int_from_isr();
    } else if (pin == B1_Pin) {
        button_exti_from_isr();
    }
}

void app_start(void)
{
    app_log_init();
    printf("\n[boot] battle01: FreeRTOS %s\n", tskKERNEL_VERSION_NUMBER);
    printf("[boot] SYSCLK %lu Hz, HCLK %lu, PCLK1 %lu, PCLK2 %lu\n",
           (unsigned long)HAL_RCC_GetSysClockFreq(),
           (unsigned long)HAL_RCC_GetHCLKFreq(),
           (unsigned long)HAL_RCC_GetPCLK1Freq(),
           (unsigned long)HAL_RCC_GetPCLK2Freq());

    net_start();
    echo_start();
    button_start();

    vTaskStartScheduler();
    app_panic("vTaskStartScheduler returned", NULL, 0);
}
