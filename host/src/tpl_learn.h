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

// длинное окно для таблицы хвоста: одиночный с тишиной до +LPOST после максимума
constexpr int LPRE = 8, LPOST = 70, LW = LPRE + LPOST + 1;
using LWin = std::array<float, LW>;

struct Singles {
    std::vector<Win> wins;                    // одиночные импульсы без базы, окно −PRE…+POST от максимума
    std::vector<LWin> lwins;                  // из них с тишиной до +LPOST: окно −LPRE…+LPOST, база — среднее спокойных отсчётов порции
    double sigma = 0;                         // средняя σ шума порций, коды
    int blocks = 0, cross = 0, single = 0, amp = 0;
};

// одиночные импульсы: порог thr·σ, одиночность ±ISO, амплитуда amin…amax, без упора в clip
Singles find_singles(const std::vector<std::vector<uint16_t>>& blocks,
                     double thr = 10.0, double amin = 100.0, double amax = 1200.0, int clip = 4090);

struct Result {
    bool ok = false;
    // причина, если не построен (текст — result_text, через T() в потоке UI; рабочий поток T() не зовёт)
    enum Err { None = 0, Few, Shape } err = None;
    int n_found = 0;                          // Few: сколько одиночных нашлось
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

std::string result_text(const Result& r);   // итог или причина отказа на языке интерфейса (ключи tpl.* в i18n.cpp)
std::string header(const Result& r, const char* name = "tpl_pmt", const char* note = "эталон с реального тракта (gammapult)");
std::vector<std::string> commands(const Result& r);   // 64 × "-tplw p v0..v6" (без -tplok)

// ---- таблица хвоста для разбора наложений (прошивка: det_tail_t, команды -tlw/-tlm/-tlok) ----
// хвост импульса с фазой эталона p, амплитудой A и максимумом на imax на отсчёте imax + j (j = 1…64) =
// A · s[row[p]][j − 1] / 32768 (Q15, 32767 ≡ 1.0 = максимум формы)
constexpr int TL_ROWS = 16, TL_LEN = 64;

struct Tail {
    bool ok = false;
    int16_t s[TL_ROWS][TL_LEN] = {};
    uint8_t row[NPH] = {};
    double rms_pct = 0;                       // систематика: МНК-средний остаток одиночных по (строке, j), rms, % A
    // причина, если не построена (текст — tail_text, через T()): 0 — построена
    enum Err { None = 0, NoTpl, Few, NoTime, Cleaned, Shape } err = None;
    int n_all = 0;                            // одиночных с тишиной до +LPOST (Singles::lwins)
    double smax = 0;                          // максимум формы S (Shape: не ≈ 1)
    int jneg = 0; double smin_pct = 0;        // средняя строка: первый j ниже нуля, минимум, % A
    // для сверки: время импульса (≈ 50 % фронта) от максимума у фаз эталона, отсчёты: диапазон (фазы делятся
    // на строки 16 равными шагами по нему) и время каждой строки
    double off_lo = 0, off_hi = 0;
    double t_row[TL_ROWS] = {};
    double k_row[TL_ROWS] = {};               // масштаб строки (смещение A эталона у её фаз): s = κ·S(j − t_row)
    int n = 0;                                // одиночных в форме (после чистки)
    double noise_rms = 0;                     // rms остатка одиночных на +1…+64, коды
    double max_pct = 0;                       // то же, наибольший по (строке, j) у строк с ≥ 20 импульсами, % A
};

// форма по одиночным с тишиной до +70 (Singles::lwins), A и фаза — подгонкой эталона tpl как в детекторе;
// время импульса по 50 % фронта, смещение по фазе — медиана; строки — 16 равных шагов по диапазону смещений
Tail build_tail(const Singles& s, const Result& tpl);
std::vector<std::string> tail_commands(const Tail& t);   // 64 × -tlw, 4 × -tlm, -tlok
bool save_tail_bin(const Tail& t, const std::string& path);   // s (int16 LE) подряд, затем row: 2112 байт
std::string tail_text(const Tail& t);       // итог или причина отказа на языке интерфейса (ключи tail.* в i18n.cpp)

// Запись эталона и таблицы хвоста в прибор по шагам. Очередь Device после "-err" идёт дальше, поэтому всё
// сразу слать нельзя: при ошибке -tplok в приборе остаётся старый эталон, и -tlok включил бы хвост под чужой
// эталон (а -save записал бы это во флеш). Шаги: [-tplw ×64, -tplok] → только если без ошибок: таблица
// (-tlw ×64, -tlm ×4, -tlok), если построена → -save, если просили (и после ошибки таблицы: эталон верный).
// Ошибка таблицы (в т. ч. старая прошивка без -tlw: -err unknown) эталон не отменяет.
// Использование: v = start(...); queue(v); когда очередь пуста — v = next(новых ошибок, последняя ошибка), queue(v);
// пусто и done() — конец.
class TplLoad {
public:
    enum Stage { Idle, Tpl, TailSt, Save, Done };
    std::vector<std::string> start(const Result& r, const Tail& t, bool save);
    std::vector<std::string> next(int errors, const std::string& last_err);
    // связь пропала посреди записи: конец, ничего не принято (что дошло до прибора — неизвестно)
    void abort(const std::string& err);
    bool busy() const { return stage != Idle && stage != Done; }
    bool done() const { return stage == Done; }
    Stage stage = Idle;
    bool tpl_ok = false;                      // -tplw/-tplok без ошибок
    bool tail_sent = false, tail_ok = false;  // таблица отправлена / принята (-tlok без ошибок)
    bool save_sent = false, save_ok = false;
    std::string tpl_err, tail_err, save_err;  // последняя ошибка шага
private:
    std::vector<std::string> after_tail();
    std::vector<std::string> tail_cmds_;
    bool save_ = false;
};

// дописать порции в открытый файл (uint16 LE × BLK подряд, как raw.bin) и выбросить их из памяти:
// сбор сотен тысяч порций не копится в ОЗУ и при сбое не теряется. Порции не длины BLK — пропускаются.
// Возврат — сколько записано; false в ok — ошибка записи.
size_t append_blocks(FILE* f, std::vector<std::vector<uint16_t>>& blocks, bool& ok);

} // namespace tpl
