// tpl_learn.h — обучение эталона формы импульса по сырым порциям АЦП (порт tools/tpl_pmt.py build).
// Без железа и UI: на входе порции по 8192 кода (ответы "-osc raw"), на выходе 64 строки × 7 отсчётов
// в Q14 (максимум строки на позиции 1) — как src/core/tpl_pmt.h, для -tplw/-tplok.
#pragma once
#include <vector>
#include <array>
#include <string>
#include <cstdint>
#include <cstdio>

namespace tpl {

constexpr int NPH = 64, NS = 7, IPK = 1;      // как в прошивке
constexpr int PRE = 4, POST = 24, W = PRE + POST + 1, ISO = 40, BLK = 8192;
constexpr int RISE_MAX = 8;   // от пересечения порога до максимума, отсчётов: больше — наложение на фронте
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
    // общая непрерывная форма (до поправки строк по фазам): x — отсчёты от максимума, y — доля максимума
    std::vector<float> shape_x, shape_y;
};

// ---- вид одиночных импульсов для экрана (шаг «Форма импульса») ----
constexpr int VPRE = 8, VPOST = 32, VW = VPRE + VPOST + 1;   // окно вида: −VPRE…+VPOST от максимума
using VWin = std::array<float, VW>;

struct BlockView {
    std::vector<VWin> pulses;                 // одиночные: база вычтена, максимум = 1 на отсчёте VPRE
    std::vector<int> ok;                      // отсчёт максимума каждого одиночного (критерии find_singles)
    std::vector<int> bad;                     // отбракованные: наложение, шум рядом, вне окна амплитуд, упор
    double base = 0, sigma = 0;               // медиана порции и σ шума, коды
};

// одна порция "-osc raw" (BLK кодов) → одиночные для наложения и метки для осциллограммы
BlockView view_block(const std::vector<uint16_t>& blk,
                     double thr = 10.0, double amin = 100.0, double amax = 1200.0, int clip = 4090);

// кольцо последних cap нормированных импульсов; at(0) — самый старый
class PulseRing {
public:
    explicit PulseRing(size_t cap = 30) : cap_(cap ? cap : 1) {}
    void push(const VWin& w) { if (v_.size() < cap_) v_.push_back(w); else { v_[head_] = w; head_ = (head_ + 1) % cap_; } }
    void clear() { v_.clear(); head_ = 0; }
    size_t size() const { return v_.size(); }
    const VWin& at(size_t i) const { return v_[(head_ + i) % v_.size()]; }
private:
    std::vector<VWin> v_;
    size_t head_ = 0, cap_;
};

// число одиночных импульсов (как Singles::amp) в порциях [from, to) — для живого счёта при сборе;
// порции считаются независимо, поэтому сумма по кускам равна счёту по всем сразу
int count_singles(const std::vector<std::vector<uint16_t>>& blocks, size_t from, size_t to,
                  double thr = 10.0, double amin = 100.0, double amax = 1200.0, int clip = 4090);

Result build(const Singles& s, int refine = 3, bool pp = true, int pp_iters = 4);

std::string header(const Result& r, const char* name = "tpl_pmt", const char* note = "эталон с реального тракта (gammapult)");
std::vector<std::string> commands(const Result& r);   // 64 × "-tplw p v0..v6" (без -tplok)

// дописать порции в открытый файл (uint16 LE × BLK подряд, как raw.bin) и выбросить их из памяти:
// сбор сотен тысяч порций не копится в ОЗУ и при сбое не теряется. Порции не длины BLK — пропускаются.
// Возврат — сколько записано; false в ok — ошибка записи.
size_t append_blocks(FILE* f, std::vector<std::vector<uint16_t>>& blocks, bool& ok);

} // namespace tpl
