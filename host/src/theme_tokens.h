// theme_tokens.h — семантические цвета темы (0xRRGGBB, sRGB) и WCAG-контраст.
// Без ImGui: проверяется консольными тестами (host/tests/test_host.cpp, test_contrast).
// Значения — из макета host/design/mockup.html.
#pragma once

namespace theme {

struct Tok {
    unsigned bg;        // фон окна
    unsigned panel;     // панели и карточки
    unsigned panel2;    // поле ввода, выделенная строка
    unsigned border;    // рамки (декор, не текст)
    unsigned text;      // основной текст (≥ 7:1 ко всем фонам)
    unsigned text2;     // вторичный текст (≥ 4.5:1)
    unsigned accent;    // главная кнопка, выбранное
    unsigned on_accent; // текст на главной кнопке
    unsigned sel_bg;    // подложка выбранного пункта
    unsigned ok, warn, danger;
    unsigned hv;        // высокое напряжение (оранжевый)
    unsigned hv_bg;     // подложка чипа «ВВ включено»
    unsigned warn_bg;   // подложка предупреждения
};
extern const Tok tok;

// Контраст по WCAG 2.x: (L1 + 0.05) / (L2 + 0.05), L — относительная яркость sRGB. 1…21.
double contrast(unsigned rgb_a, unsigned rgb_b);

} // namespace theme
