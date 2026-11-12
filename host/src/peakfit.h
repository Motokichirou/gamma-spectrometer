// peakfit.h — подгонка фотопика гауссом с линейным фоном (как D:/gamma_data/hs_test/spec.py analyze):
// окно ±2.2σ вокруг пика, 3 итерации, веса 1/√N. Для мастера ввода в строй и CLI.
#pragma once
#include <cstdint>

namespace peakfit {

struct Result {
    bool ok = false;
    double peak = 0, sigma = 0, fwhm_pct = 0, fwhm_err = 0, height = 0;
    double bg0 = 0, bg1 = 0, x0 = 0;          // линейный фон под пиком: bg0 + bg1·(x − x0)
    // подогнанная модель (колокол + фон) — для графика
    double model(double x) const;
};

// спектр 8192 каналов; поиск максимума сглаженного (9 кан.) в [lo, hi)
Result fit(const uint32_t* spec, int n, int lo, int hi);

// ---- набор до N импульсов в фотопике ----
// окно фотопика: центр ± k·FWHM по подгонке fit() живого спектра; каналы [lo, hi).
// ok = false — пик не найден уверенно (подгонка не сошлась, ширина вне 1…30 %, площадь < 30,
// погрешность ширины больше половины её), счёт в пике не идёт.
struct PeakWin {
    bool ok = false;
    double center = 0, fwhm = 0;              // каналы
    int lo = 0, hi = 0;
    Result fit;
};
PeakWin peak_window(const uint32_t* spec, int n, int search_lo, int search_hi, double k = 1.5);

// сумма каналов [lo, hi), границы обрезаются по [0, n)
uint64_t window_sum(const uint32_t* spec, int n, int lo, int hi);

// когда остановить набор: набралось N в пике (Count) раньше страховки по времени (Timeout)
enum class Stop { Run, Count, Timeout };
Stop count_stop(uint64_t in_peak, uint64_t target, double elapsed_s, double timeout_s);

// E(ch) = c0 + c1·ch + … c4·ch⁴ → 11 команд "-cal i HEX" (формат BecqMoni: double hi/lo + CRC-32)
void becq_cal_commands(const double c[5], char out[11][40]);

} // namespace peakfit
