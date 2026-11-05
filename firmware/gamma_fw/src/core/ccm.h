/* Размещение горячего кода в CCM SRAM (0x10000000, без ожиданий флеша).
   На ПК (тесты) атрибут пустой. */
#ifndef CCM_H
#define CCM_H

#if defined(__GNUC__) && defined(__arm__)
#define CCM_FUNC __attribute__((section(".ccmfunc"), noinline))
#define CCM_DATA __attribute__((section(".ccmdata")))
#else
#define CCM_FUNC
#define CCM_DATA
#endif

#endif
