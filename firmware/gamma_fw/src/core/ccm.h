/* Размещение горячего кода в CCM SRAM (0x10000000, без ожиданий флеша).
   CCM_FUNC — код, CCM_DATA — инициализированные данные (образ во флеш, копируется при старте),
   CCM_BSS — обнуляемые данные. ⚠ DMA к CCM SRAM на G4 доступа не имеет: буферы DMA — только в ОЗУ.
   На ПК (тесты) атрибуты пустые. */
#ifndef CCM_H
#define CCM_H

#if defined(__GNUC__) && defined(__arm__)
#define CCM_FUNC __attribute__((section(".ccmfunc"), noinline))
#define CCM_DATA __attribute__((section(".ccmdata")))
#define CCM_BSS  __attribute__((section(".ccmbss")))
#else
#define CCM_FUNC
#define CCM_DATA
#define CCM_BSS
#endif

#endif
