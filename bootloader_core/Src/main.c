#include "stm32f446xx.h"
#include "stdint.h"
#include "stdlib.h"
#include "string.h"
#include "init.h"
#define APP_START_ADDRESS 0x8008000
#define START_SECTOR 2
#define END_SECTOR 7
#define CHUNK_SIZE 256
#define CRC_STORE *((volatile uint32_t *)(0x0807FFFC))	  // stores crc value
#define APP_END_ADDR *((volatile uint32_t *)(0x0807FFF4)) // stores app_end_address
#define FIRST_KEY 0x45670123
#define SECOND_KEY 0xCDEF89AB

typedef enum
{
	STATE_MAIN_MENU = (1 << 0),
	STATE_CRC = (1 << 1),
	STATE_CHECKSUM = (1 << 2),
	STATE_JMP_TO_MAIN_APP = (1 << 3),
	STATE_GOT_FILE_SIZE = (1 << 4),
	STATE_SEND_MENU = (1 << 5)
} state_t;

static const uint32_t sector_end_addr[6] = {0x0800BFFF, 0x0800FFFF, 0x08017FFF,
											0x0803FFFF, 0x0805FFFF, 0x0807FFFF};
static uint16_t packet_size;
static uint32_t file_size = 0;
volatile uint8_t state = 0;
volatile uint8_t ptr = 0;
volatile uint8_t reflect = 0;
volatile uint32_t curr_address = APP_START_ADDRESS;
uint8_t buffer[CHUNK_SIZE + 1] = {0};
char incoming[32];
volatile uint8_t checksum_rx;
volatile uint8_t checksum_fin = 0;
uint32_t final_crc_val = 0;

static char *menu = "=======================================\r\n"
					"        STM32 CUSTOM BOOTLOADER     \r\n"
					"=======================================\r\n"
					"     Press 'u' to UPDATE firmware.\r\n"
					"    Press 'b' to go to application.\r\n";

void uart_send(char *str)
{
	while (*str)
	{
		while (!(USART2->SR & USART_SR_TXE))
			;
		USART2->DR = *str++;
	}
}

void uart_receive(void)
{
	while (!(USART2->SR & USART_SR_RXNE))
		;
	char rx_char = USART2->DR;
	if (ptr >= sizeof(incoming) - 1)
	{
		incoming[ptr] = '\0';
		reflect = 0;
	}
	else
	{
		if (rx_char == '\r' || rx_char == '\n')
		{
			incoming[ptr] = '\0';
			reflect = 0;
		}
		else
		{
			incoming[ptr++] = rx_char;
		}
	}
}
void get_crc_val_and_check()
{
	uart_send("Checking for CRC value from script\r\n");
	ptr = 0;
	reflect = 1;
	final_crc_val = CRC->DR;
	uint32_t final_crc_val_from_script = 0;
	for (int i = 0; i < 4; i++)
	{
		while (!(USART2->SR & USART_SR_RXNE))
			; // Wait for byte to arrive

		final_crc_val_from_script = (final_crc_val_from_script << 8) | USART2->DR;
	}

	if (final_crc_val_from_script != final_crc_val)
	{
		FLASH->CR |= FLASH_CR_LOCK;
		state |= STATE_CRC;
	}
	else
	{
		while (FLASH->SR & FLASH_SR_BSY)
			;
		FLASH->CR |= FLASH_CR_PG;
		APP_END_ADDR = curr_address;
		while (FLASH->SR & FLASH_SR_BSY)
			;
		CRC_STORE = final_crc_val;
		while (FLASH->SR & FLASH_SR_BSY)
			;
		FLASH->CR &= ~(FLASH_CR_PG);
		FLASH->CR |= FLASH_CR_LOCK;
		state &= ~STATE_CRC;
	}
}

void jump_to_main_app(void)
{
	CRC->CR |= CRC_CR_RESET;
	volatile uint32_t start_crc = APP_START_ADDRESS;
	if (APP_END_ADDR <= 0x08000000 || APP_END_ADDR >= 0x0807FFFF)
	{
		FLASH->CR |= FLASH_CR_LOCK;
		state |= STATE_JMP_TO_MAIN_APP;
		return;
	}
	while (start_crc < APP_END_ADDR)
	{
		CRC->DR = *(volatile uint32_t *)start_crc;
		start_crc += 4;
	}
	if (CRC->DR != CRC_STORE)
	{
		FLASH->CR |= FLASH_CR_LOCK;
		state |= STATE_JMP_TO_MAIN_APP;
		return;
	}

	volatile uint32_t app_estack = *(volatile uint32_t *)APP_START_ADDRESS;
	volatile uint32_t app_reset_handler =
		*(volatile uint32_t *)(APP_START_ADDRESS + 0x4);
	if ((app_estack >= 0x20000000) && (app_estack <= 0x20020000))
	{
		uart_send("Moving to application....\r\n");
		TIM6->CR1 &= ~TIM_CR1_CEN;
		GPIOC->MODER &= ~(GPIO_MODER_MODE13);
		GPIOA->ODR &= ~(GPIO_ODR_OD5);
		GPIOA->MODER &= ~(GPIO_MODER_MODE5);
		USART2->CR1 &= ~(USART_CR1_RE | USART_CR1_TE | USART_CR1_UE);
		state = 0;
		// jmp to main app
		__disable_irq();
		__DSB();
		__ISB();
		SCB->VTOR = APP_START_ADDRESS;
		__set_MSP(app_estack);
		__asm __volatile("BX %[reset_handler]\n\t" ::[reset_handler] "r"(app_reset_handler) : "memory");
	}

	uart_send("Invalid Stack Pointer...\r\n");
	FLASH->CR |= FLASH_CR_LOCK;
	state |= STATE_JMP_TO_MAIN_APP;
	return;
}

void Take_data_from_UART(void)
{
	memset((char *)buffer, 0, sizeof(buffer));
	ptr = 0;
	checksum_fin = 0;
	volatile uint16_t chunk_size = CHUNK_SIZE + 1;
	while (chunk_size > 0)
	{
		if (chunk_size == 1)
		{
			while (!(USART2->SR & USART_SR_RXNE))
				;
			checksum_rx = USART2->DR;
		}
		else
		{
			while (!(USART2->SR & USART_SR_RXNE))
				;
			uint8_t rx_data = USART2->DR;
			checksum_fin ^= rx_data;
			buffer[ptr++] = rx_data;
		}
		chunk_size--;
	}
}

void flash_write(void)
{
	FLASH->CR |= (2U << FLASH_CR_PSIZE_Pos);

	for (volatile int i = 0; i < 256; i += 4)
	{
		uint32_t word_data;
		memcpy(&word_data, &buffer[i], 4);
		while (FLASH->SR & FLASH_SR_BSY)
			;
		FLASH->CR |= (FLASH_CR_PG);
		*(volatile uint32_t *)curr_address = word_data;
		while (FLASH->SR & FLASH_SR_BSY)
			;
		FLASH->CR &= ~(FLASH_CR_PG);
		CRC->DR = word_data;
		curr_address += 4;
	}
}
void flash_unlock_erase(void)
{
	FLASH->KEYR = FIRST_KEY;
	FLASH->KEYR = SECOND_KEY;
	if (!(FLASH->CR & FLASH_CR_LOCK))
	{
		FLASH->CR |= (2U << FLASH_CR_PSIZE_Pos);
		// erase sectors 2 to 7
		for (volatile uint8_t i = START_SECTOR; i <= END_SECTOR; ++i)
		{
			while (FLASH->SR & FLASH_SR_BSY)
				;
			FLASH->CR |= (FLASH_CR_SER);
			FLASH->CR &= ~(FLASH_CR_SNB);
			FLASH->CR |= (i << FLASH_CR_SNB_Pos);
			FLASH->CR |= (FLASH_CR_STRT);
			while (FLASH->SR & FLASH_SR_BSY)
				;
			if (sector_end_addr[i] >= (APP_START_ADDRESS + (CHUNK_SIZE * packet_size)))
			{
				if (i < 7)
				{
					while (FLASH->SR & FLASH_SR_BSY)
						;
					FLASH->CR |= (FLASH_CR_SER);
					FLASH->CR &= ~(FLASH_CR_SNB);
					FLASH->CR |= (7U << FLASH_CR_SNB_Pos);
					FLASH->CR |= (FLASH_CR_STRT);
					while (FLASH->SR & FLASH_SR_BSY)
						;
				}
				break;
			}
		}
		FLASH->CR &= ~(FLASH_CR_SER);
	}
	else
	{
		uart_send("Unable to unlock flash...");
	}
}

void get_file_size()
{
	uart_send("Getting file size...\r\n");
	for (int i = 0; i < 4; i++)
	{
		while (!(USART2->SR & USART_SR_RXNE))
			; // Wait for byte to arrive

		file_size = (file_size << 8) | USART2->DR;
	}
	state |= STATE_GOT_FILE_SIZE;
}

int main(void)
{

	clock_init();
	gpio_init();
	timer_init();
	usart_init();
	TIM6->CR1 |= TIM_CR1_CEN;
	GPIOA->ODR |= GPIO_ODR_OD5;
	for (;;)
	{
		while ((TIM6->SR & TIM_SR_UIF) == 0)
		{
			if (!(GPIOC->IDR & GPIO_IDR_ID13))
			{
				while (!(GPIOC->IDR & GPIO_IDR_ID13))
					;
				GPIOC->MODER &= ~(GPIO_MODER_MODE13);
				TIM6->CR1 &= ~TIM_CR1_CEN;
				state |= STATE_SEND_MENU;
				state |= STATE_MAIN_MENU;
				break;
			}
		}
		if (!(state & STATE_MAIN_MENU))
		{
			jump_to_main_app();
			if (state & STATE_JMP_TO_MAIN_APP)
			{
				state |= (STATE_MAIN_MENU | STATE_SEND_MENU);
				state &= ~STATE_JMP_TO_MAIN_APP;
			}
		}
		while (state & STATE_MAIN_MENU)
		{
			if (state & STATE_SEND_MENU)
			{
				state &= ~STATE_SEND_MENU;
				uart_send((char *)menu);
				reflect = 1;
			}
			while (reflect)
			{
				uart_receive();
			}
			if (!strcmp((char *)incoming, "u") && ptr < 3)
			{
				curr_address = APP_START_ADDRESS;
				// get file size from uart
				if (!(state & STATE_GOT_FILE_SIZE))
				{
					state &= ~(STATE_GOT_FILE_SIZE);
					get_file_size();
					packet_size = (file_size + CHUNK_SIZE - 1) / CHUNK_SIZE;
				}

				// erase flash
				uart_send("Erasing starting...\r\n");
				flash_unlock_erase();
				uart_send("Successfully erased...\r\n");

				// start writing to flash 256 byte at a time
				CRC->CR |= CRC_CR_RESET;
				for (volatile int i = 0; i < (packet_size); i++)
				{
					Take_data_from_UART();
					if (checksum_fin == checksum_rx)
					{
						flash_write();
						uart_send("A");
					}
					else
					{
						uart_send(
							(char *)"\r\nData corrupted\r\nPlease restart again...\r\n");
						state |= STATE_CHECKSUM;
						break;
					}
				}
				if (state & STATE_CHECKSUM)
				{
					state &= ~STATE_CHECKSUM;
					state |= STATE_SEND_MENU;
					continue;
				}
				get_crc_val_and_check();
				if (state & STATE_CRC)
				{
					state &= ~STATE_CRC;
					state |= STATE_SEND_MENU;
					uart_send("Corrupted CRC! Retry again\r\n");
					continue;
				}
				uart_send("Successfully modified application...\r\n");
				jump_to_main_app();
				if (state & STATE_JMP_TO_MAIN_APP)
				{
					state &= ~STATE_JMP_TO_MAIN_APP;
					state |= STATE_SEND_MENU;
					continue;
				}
			}
			else if (!strcmp((char *)incoming, "b") && ptr < 3)
			{
				jump_to_main_app();
				if (state & STATE_JMP_TO_MAIN_APP)
				{
					uart_send("Invalid app... \r\n");
					state &= ~STATE_JMP_TO_MAIN_APP;
					state |= STATE_SEND_MENU;
				}
			}
			else
			{
				uart_send("Kindly enter correct command...\r\n");
			}
			ptr = 0;
		}
	}
}
