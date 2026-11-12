// tpl_learn.h — обучение эталона формы импульса по сырым порциям АЦП (порт tools/tpl_pmt.py build).
// Без железа и UI: на входе порции по 8192 кода (ответы "-osc raw"), на выходе 64 строки × 7 отсчётов
// в Q14 (максимум строки на позиции 1) — как src/core/tpl_pmt.h, для -tplw/-tplok.
#pragma once
#include <vector>
#include <array>
#include <string>
#include <cstdint>

namespace tpl {

constexpr int NPH = 64, NS = 7, IPK = 1;      // как в прошивке
constexpr int PRE = 4, POST = 24, W = PRE + POST + 1, ISO = 40, BLK = 8192;
constexpr int QMAX = 16384;

using Win = std::array<double, W>;

struct Singles {
    std::vector<Win> wins;                    // одиночные импульсы без базы, окно −PRE…+POST от максимума
    double sigma = 0;                         // средняя σ шума порций, коды
    int blocks = 0, cross = 0, single = 0, amp = 0;
};

// одиночные импульсы: порог thr·σ, одиночность ±ISO, амплитуда amin…amax, без упора в clip
Singles find_singles(const std::vector<std::vector<uint16_t>>& blocks,
                     double thr = 10.0, double amin = 100.0, double amax = 1200.0, int clip = 4090);

struct Result {
    bool ok = false;
    std::string msg;
    int q[NPH][NS] = {};                      // строки эталона Q14
    int n = 0;                                // импульсов в эталоне
    double sigma = 0;
    double resid_med = 0;                     // медиана rms(w − A·h)/A, %
    double peak = 0, peak_sd = 0;             // фотопик по новому эталону, коды
    double spread_before = 0, spread_after = 0;   // размах фотопика по 8 фазовым группам, %
    double fwhm50 = 0;                        // ширина формы на половине высоты, отсчётов
};

// число одиночных импульсов (как Singles::amp) в порциях [from, to) — для живого счёта при сборе;
// порции считаются независимо, поэтому сумма по кускам равна счёту по всем сразу
int count_singles(const std::vector<std::vector<uint16_t>>& blocks, size_t from, size_t to,
                  double thr = 10.0, double amin = 100.0, double amax = 1200.0, int clip = 4090);

Result build(const Singles& s, int refine = 3, bool pp = true, int pp_iters = 4);

std::string header(const Result& r, const char* name = "tpl_pmt", const char* note = "эталон с реального тракта (gammapult)");
std::vector<std::string> commands(const Result& r);   // 64 × "-tplw p v0..v6" (без -tplok)

} // namespace tpl
