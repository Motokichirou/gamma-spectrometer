// ui_home.h — главный экран пульта: три вида по состоянию прибора (ds::home_view) —
// «Подключите прибор», «Прибор не настроен», «Всё готово». Макет host/design/mockup.html, экраны 1, 2, 4.
#pragma once
#include "imgui.h"

// Рисует главный экран в области между верхней полосой и строкой состояния.
void DrawHome(ImVec2 pos, ImVec2 size);
