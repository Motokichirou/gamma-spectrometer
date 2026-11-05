/* gamma_fw: каркас. Пустой цикл; драйверы и логика добавляются следующими задачами. */
#include "stm32g474xx.h"
#include "ccm.h"
#include <stdint.h>

/* Вызывается из Reset_Handler первым делом. Таблица векторов — наша:
   после перехода из системного загрузчика VTOR может смотреть в его ПЗУ. */
void SystemInit(void)
{
    SCB->VTOR = 0x08000000u;
    /* FPU: сборка hard-float, без доступа к CP10/CP11 первая же VFP-инструкция
       даёт UsageFault. Обязательно. Глобальные переменные здесь не трогать:
       .data/.bss ещё не инициализированы. */
    SCB->CPACR |= (3u << 20) | (3u << 22);
    __DSB();
    __ISB();
}

/* Проверка размещения кода в CCM SRAM (на ПК не применяется). */
static CCM_FUNC uint32_t ccm_probe(uint32_t x)
{
    return x * 3u + 1u;
}

int main(void)
{
    volatile uint32_t sink = 0;
    for (;;) {
        sink = ccm_probe(sink);
    }
}
