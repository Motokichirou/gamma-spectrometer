// devstate.h — чистая логика состояния прибора для пульта: разбор записи о настройке (-setup),
// вид главного экрана, шаги мастера и их предпосылки, верхняя полоса, подсказка при ошибке порта.
// Без Win32, COM-порта и ImGui — покрыто консольными тестами (tests/test_host.cpp).
#pragma once
#include <string>
#include <cstdint>

namespace ds {

// запись о настройке из прибора (ответ "-ok setup …", settings_t v2 setup_rec_t)
struct SetupRec {
    bool tpl = false, cal = false;               // эталон и шкала BecqMoni есть (кем бы ни записаны)
    uint32_t chk_unix = 0; bool chk_ok = false;  // «Проверка электроники»
    uint32_t zero_unix = 0;                      // «Ноль монитора ВВ»
    uint32_t hv_unix = 0; double hv_v = 0, hv_peak_ch = 0;   // последний подбор ВВ
    uint32_t tpl_unix = 0;                       // эталон загружен мастером
    uint32_t res_unix = 0; double res_fwhm = 0, res_peak_ch = 0;
    uint32_t done_unix = 0;                      // «Сохранение»; 1 — настроен до учёта, дата неизвестна
};

// разбор строки "-ok setup tpl=… cal=… chk=…/… zero=… hv=…/…/… tplu=… res=…/…/… done=…".
// Ключи сравниваются целиком (tpl и tplu — разные). false — не запись о настройке или она испорчена.
bool parse_setup(const std::string& reply, SetupRec& out);

enum class SetupKnown { Unknown, Yes, OldFirmware };   // OldFirmware — на -setup ответили -err

struct Snapshot {
    bool connected = false;
    bool fw_ok = false;                          // прибор назвался прошивкой gamma_fw (ответ -inf)
    SetupKnown known = SetupKnown::Unknown;
    SetupRec s;
    int saved = -1;                              // ключ saved в -gst: 1/0, −1 — нет или «-»
    bool hv_on = false;
    double hv_v = 0;
};

enum class Home { NotConnected, NotConfigured, Ready };
enum class Step { Flash, Check, Zero, Hv, Tpl, Hv2, Res, Scale, Save, N };
enum class Mode { Home, Wizard, Advanced };

Home home_view(const Snapshot&);
bool step_done(const Snapshot&, Step);
Step first_open_step(const Snapshot&);           // первый несделанный; все сделаны → Save
int  step_prereq(const Snapshot&, Step);         // −1 — можно; иначе (int)Step, который нужен раньше

struct TopBar { bool modes_enabled; bool wiz_on, adv_on; bool hv_chip_on; bool kill_visible; };
TopBar top_bar(const Snapshot&, Mode);

// ---- главный экран ----
int  steps_done_count(const Snapshot&);          // сколько из 9 шагов сделано (для «Пройдено N из 9»)
bool step_needs_cs(Step);                        // шаг требует источник Cs-137

// ответ -cal (40 строк hex, конвенция BecqMoni): первые 10 слов — c0…c4 как double (старшее/младшее слово).
// false — не ответ -cal или шкалы нет (все нули, не числа). E(ch) = c0 + c1·ch + …; кэВ/канал ≈ c1.
bool parse_becq_cal(const std::string& reply, double c[5]);

// "-ok tc <t0> <k1> <k2>"; false — не тот ответ
bool parse_tc(const std::string& reply, double& t0, double& k1, double& k2);

// "-ok log on|off period … active 0|1 … records N …" (ответ "-log" без аргументов)
struct LogState { bool enabled = false, active = false; long records = 0; };
bool parse_log_status(const std::string& reply, LogState& out);

// ключ строки i18n по коду GetLastError() открытия порта
const char* connect_hint_key(unsigned long win_error);

// серийник из -cal для показа: "0474NNNN" (8 hex) → "GS474-NNNN", иначе как есть
std::string display_serial(const std::string& cal_last);

} // namespace ds
