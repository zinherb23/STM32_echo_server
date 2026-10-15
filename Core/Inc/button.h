/*
 * button.h - user button B1 (PC13, active low, EXTI13 falling edge).
 */
#ifndef BUTTON_H
#define BUTTON_H

/* Creates the button task. */
void button_start(void);

/* From the EXTI15_10 ISR (via HAL_GPIO_EXTI_Callback): wake the button
 * task. Does nothing else - debouncing happens in the task. */
void button_exti_from_isr(void);

#endif /* BUTTON_H */
