// adc_dnl.h — таблица поправки дифференциальной нелинейности (DNL) сигнального АЦП по сырым порциям.
//
// Метод плотности кодов: гистограмма кодов по сырым отсчётам (широкий спектр: смесь, фон), ширина кода
// w = n / N, где N — локальная норма (медиана соседей в ±half_win кодов без «выбросов»). Код — «выброс»,
// если отклонение от медианы соседей больше max(sig_k·√N, rel_k·N). Центр кода считается по накопленной
// сумме (w − 1) от начала группы выбросов (соседние выбросы через ≤ 1 код — одна группа): нижняя граница
// кода = сумма по предыдущим кодам группы, центр = граница + (w − 1)/2 (сдвиг относительно номинала).
// Вне групп сдвиг 0 (INL между дефектами считается нулевым — шум суммы не копится).
// Отсев мест, где норма не годится: кривизна нормы |N(c−W) + N(c+W) − 2N(c)| > curv·N(c), перепад
// |N(c+W) − N(c−W)| > slope·N(c) или больше
// max_pos «выбросов вверх» в окне (горб базы около 620 кодов — там плотность меняется быстрее окна);
// группа с больше чем max_narrow узкими встречающимися или max_wide широкими кодами — не DNL (впадина/горб).
// В таблицу — коды, которые встречаются (w ≥ min_width), с нормой ≥ min_norm и |сдвиг| ≥ min_off LSB;
// больше MAX_PAIRS — берутся с наибольшим |сдвигом|. Прибор хранит сдвиг в 1/16 LSB (команда -dnl,
// SPEC §9; спек docs/superpowers/specs/2026-11-14-adc-dnl-design.md).
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace adc_dnl {

constexpr int CODES = 4096;
constexpr int MAX_PAIRS = 16;       // DET_DNL_MAX прошивки
constexpr int OFF_MAX = 1024;       // DET_DNL_OFF_MAX: |сдвиг| ≤ 64 LSB, в 1/16 LSB

struct Params {
    int half_win = 16;          // полуширина окна нормы, кодов
    double min_norm = 50;       // норма, отсчётов на код: меньше — статистики мало, код не оценивается
    double sig_k = 5;           // порог выброса: отклонение > sig_k·√N …
    double rel_k = 0.3;         // … и > rel_k·N
    double curv = 0.3;          // допустимая кривизна нормы на ±half_win (доля N)
    double slope = 1.0;         // допустимый перепад нормы N(c+W) − N(c−W) (доля N): склон горба базы — мимо
    int max_pos = 3;            // «выбросов вверх» в окне ±half_win не больше
    int max_narrow = 3;         // в группе выбросов узких встречающихся кодов (min_width ≤ w < 1) не больше …
    int max_wide = 3;           // … и широких (w > 1) не больше, иначе группа — не DNL
    double min_width = 0.1;     // ширина кода (LSB) не меньше — реже встречающиеся коды не берутся
    double min_off = 0.5;       // |сдвиг| центра, LSB, не меньше
};

struct Entry {
    int code = 0;
    int off16 = 0;              // сдвиг центра, 1/16 LSB (как в приборе)
    double off = 0;             // то же, LSB
    double width = 1;           // ширина кода, LSB
    double norm = 0;            // локальная норма, отсчётов на код
    uint64_t count = 0;         // отсчётов с этим кодом
};

struct Result {
    bool ok = false;
    std::string msg;
    uint64_t samples = 0;           // отсчётов в гистограмме
    std::vector<Entry> table;       // ≤ MAX_PAIRS, по возрастанию кода
    std::vector<Entry> candidates;  // все прошедшие отбор, по убыванию |сдвига|
    std::vector<int> outliers;      // все коды-выбросы с годной нормой (для отчёта)
};

// гистограмма кодов: hist.size() == CODES; коды ≥ CODES — в *over (если не NULL)
void add_samples(std::vector<uint64_t>& hist, const uint16_t* s, size_t n, uint64_t* over = nullptr);
Result compute(const std::vector<uint64_t>& hist, const Params& p = Params());

// файл таблицы: «код сдвиг_1/16» построчно, # — комментарий (его же читает tools/replay)
std::string format_table(const Result& r, const std::string& comment);
// разбор файла таблицы; false — ошибка (err)
bool parse_table(const std::string& text, std::vector<std::pair<int, int>>& out, std::string& err);
// команды прибору: "-dnl clr", затем пары одной командой (≤ 16 пар — ≤ 181 символа), [-save]
std::vector<std::string> load_commands(const std::vector<std::pair<int, int>>& t, bool save);

}  // namespace adc_dnl
