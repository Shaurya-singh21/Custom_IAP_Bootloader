#include "stm32f446xx.h"
#include "stdint.h"
#include "init.h"

void clock_init(void) {
	RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOCEN | RCC_AHB1ENR_CRCEN;
	RCC->APB1ENR |= RCC_APB1ENR_USART2EN | RCC_APB1ENR_TIM6EN;
}

void gpio_init(void) {
	//pc13 as button
	GPIOC->MODER &= ~GPIO_MODER_MODE13;

	//pa5 led
	GPIOA->MODER |= (1U << GPIO_MODER_MODE5_Pos);
	GPIOA->OTYPER &= ~(GPIO_OTYPER_OT5);
	GPIOA->ODR &= (~GPIO_ODR_OD5);
}

void usart_init(void) {
	//pa2 for tx and pa3 for rx
	GPIOA->MODER |= (2U << GPIO_MODER_MODE2_Pos) | (2U << GPIO_MODER_MODE3_Pos);
	GPIOA->AFR[0] |= (7U << GPIO_AFRL_AFSEL2_Pos) | (7U << GPIO_AFRL_AFSEL3_Pos);
	USART2->BRR = baud_rate;
	USART2->CR1 |= (USART_CR1_RE) | (USART_CR1_TE) | (USART_CR1_UE);
}

void timer_init(void) {
	//tim6 for manual count
	TIM6->ARR = 2999;
	TIM6->PSC = 15999;
	TIM6->EGR = TIM_EGR_UG;
	TIM6->SR = ~TIM_SR_UIF;
	TIM6->CNT = 0;
}
