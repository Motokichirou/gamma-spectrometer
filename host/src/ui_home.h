// ui_home.h — главный экран пульта: три вида по состоянию прибора (ds::home_view) —
// «Подключите прибор», «Прибор не настроен», «Всё готово». Макет host/design/mockup.html, экраны 1, 2, 4.
#pragma once
#include "imgui.h"
#include "devstate.h"
#include <string>

// Рисует главный экран в области между верхней полосой и строкой состояния.
void DrawHome(ImVec2 pos, ImVec2 size);

// короткий результат пройденного шага по данным прибора («дата», «755 В, пик на канале 1190», «6.99 %»);
// общий для списка шагов главного экрана и рейки мастера
std::string HomeStepResult(const ds::Snapshot& s, ds::Step st);
