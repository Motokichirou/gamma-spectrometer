// ui_adv.h — расширенный режим: навигация по задачам слева, страница посередине,
// справа телеметрия и короткий журнал команд. Макет host/design/mockup.html, экран 5.
// Страница — g_ui.adv_page (enum AdvPage в ui_shell.h).
#pragma once
#include "imgui.h"

// Рисует расширенный режим в области между верхней полосой и строкой состояния.
void DrawAdvanced(ImVec2 pos, ImVec2 size);
