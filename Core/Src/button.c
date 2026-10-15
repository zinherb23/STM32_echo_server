/*
 * button.c - user button B1 on PC13 (Nucleo: external pull-up, pressed = low).
 *
 * The EXTI13 interrupt (EXTI15_10_IRQHandler, falling edge) only gives a
 * task notification. The task debounces:
 *   - an edge less than BUTTON_LOCKOUT_MS after the previous edge is
 *     ignored (contact bounce);
 *   - otherwise the pin is sampled BUTTON_CONFIRM_MS later and the press
 *     only counts if it is still low. Bounce on RELEASE also produces
 *     falling edges, but the pin has settled high by then.
 * Each accepted press increments a counter and posts it to the echo
 * task's queue; the echo task (the only owner of the socket) sends
 * "button pressed <count>\r\n" to the connected client.
 */
#include "button.h"
#include "app.h"
#include "app_config.h"
#include "app_log.h"
#include "echo.h"
#include "main.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdbool.h>
#include <stdio.h>

#if !defined(B1_Pin) || !defined(B1_GPIO_Port)
#error "Set CubeMX user label B1 on PC13 (GPIO_EXTI13, falling edge, no pull)"
#endif

#define BUTTON_LOCKOUT_MS   50U   /* ignore edges this close to the previous one */
#define BUTTON_CONFIRM_MS   10U   /* then require the pin to be still low */

static TaskHandle_t button_task_handle;

void button_exti_from_isr(void)
{
    BaseType_t woken = pdFALSE;
    if (button_task_handle != NULL) {
        vTaskNotifyGiveFromISR(button_task_handle, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void button_task(void *arg)
{
    TickType_t last_edge = 0;
    uint32_t count = 0;
    bool first = true;
    (void)arg;

    for (;;) {
        TickType_t now;
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);   /* one or more falling edges */
        now = xTaskGetTickCount();
        if (!first && (TickType_t)(now - last_edge) < pdMS_TO_TICKS(BUTTON_LOCKOUT_MS)) {
            last_edge = now;
            continue;                                    /* bounce */
        }
        first = false;
        last_edge = now;
        vTaskDelay(pdMS_TO_TICKS(BUTTON_CONFIRM_MS));
        if (HAL_GPIO_ReadPin(B1_GPIO_Port, B1_Pin) != GPIO_PIN_RESET) {
            continue;                                    /* glitch or release bounce */
        }
        count++;
        if (echo_post_button(count)) {
            printf("[btn] press %lu\n", (unsigned long)count);
        } else {
            printf("[btn] press %lu (queue full, message dropped)\n", (unsigned long)count);
        }
    }
}

void button_start(void)
{
    if (xTaskCreate(button_task, "button", APP_STACK_BUTTON, NULL,
                    APP_PRIO_BUTTON, &button_task_handle) != pdPASS) {
        app_panic("xTaskCreate", "button", 0);
    }
}
