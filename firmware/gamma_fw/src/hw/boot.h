/* boot.h — переход в системный загрузчик и аварийный сброс (SPEC §4). */
#ifndef BOOT_H
#define BOOT_H

/* Переход в системный загрузчик STM32 (USART1 115200 8E1, AN2606): ВВ выкл (PB10 = 0, PB6 = 0,
   PA8 из AF в выход 0, TIM1 сброс), дослать UART (≤ 50 мс), SysTick стоп, NVIC выкл/очистить,
   сбросить DMA/DMAMUX/USART1/ADC/DAC/TIM1/SPI1 и ОУ, SYSCLK → HSI, PLL и HSE выкл, регулятор
   в Range 1 normal, PA9/PA10 в аналог, SYSCFG->MEMRMP = 1, MSP/PC с 0x1FFF0000. Не возвращается.
   Звать из главного цикла (не из прерывания). */
void boot_enter_system(void) __attribute__((noreturn));

/* Обработчик сбоя (HardFault, MemManage, BusFault, UsageFault, NMI, неожиданное прерывание —
   startup: Default_Handler → Fault_Handler): ВВ выкл, ≈1 с, NVIC_SystemReset. Кирпича нет:
   после сброса прошивка снова слушает UART. Причина сброса видна в стартовом сообщении (sw). */
void Fault_Handler(void) __attribute__((noreturn));

#endif
