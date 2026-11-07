// peakfit.h — подгонка фотопика гауссом с линейным фоном (как D:/gamma_data/hs_test/spec.py analyze):
// окно ±2.2σ вокруг пика, 3 итерации, веса 1/√N. Для мастера ввода в строй и CLI.
#pragma once
#include <cstdint>

namespace peakfit {

struct Result {
    bool ok = false;
    double peak = 0, sigma = 0, fwhm_pct = 0, fwhm_err = 0, height = 0;
};

// спектр 8192 каналов; поиск максимума сглаженного (9 кан.) в [lo, hi)
Result fit(const uint32_t* spec, int n, int lo, int hi);

// E(ch) = c0 + c1·ch + … c4·ch⁴ → 11 команд "-cal i HEX" (формат BecqMoni: double hi/lo + CRC-32)
void becq_cal_commands(const double c[5], char out[11][40]);

} // namespace peakfit
