// logdump.h — записи регистратора прибора (SPEC_logger.md): разбор, интервальные спектры, выгрузка в
// папку, пик по интервалам (для термокомпенсации и кривой старения), сложение с выравниванием по пику.
// Формат записи — общий с прошивкой: firmware/gamma_fw/src/core/logrec.{h,c} (собирается здесь как C++).
#pragma once
#include "logrec.h"
#include <cstdint>
#include <string>
#include <vector>

namespace logdump {

struct Record {
    uint32_t sector = 0;
    logrec_hdr_t h{};
    std::vector<uint32_t> spec;             // накопленный спектр с начала сессии
    std::vector<uint8_t> raw;               // запись как пришла (заголовок + нагрузка)
    bool ok = false;
    std::string err;
};

// raw: [0, hdr_len) — заголовок, [LOGREC_HDR_PAGE, …) — нагрузка
bool parse(const std::vector<uint8_t>& raw, uint32_t sector, Record& r);

// записи одной сессии по возрастанию seq
std::vector<const Record*> session(const std::vector<Record>& all, uint32_t sess);
std::vector<uint32_t> sessions(const std::vector<Record>& all);

// интервальный спектр записи i сессии: spec_i − spec_{i−1}; после -rst хоста (LOGF_HOST_RST) — spec_i
std::vector<uint32_t> interval(const std::vector<const Record*>& s, size_t i);

double temp_c(int16_t c100);                // NaN — нет

struct PeakPt {
    uint32_t seq = 0, rec = 0, flags = 0;
    double t_mid_s = 0, temp = 0, temp_span = 0, live_s = 0;
    double peak = 0, sigma = 0, fwhm_pct = 0;
    uint64_t n = 0;
    bool ok = false;
};
// подгонка пика в окне [lo, hi) по каждому интервалу сессии (запись 0 сессии пропускается)
std::vector<PeakPt> peaks(const std::vector<const Record*>& s, int lo, int hi);

// сложение интервалов с приведением к масштабу по пику в [lo, hi): канал × ref/peak_i, линейная
// интерполяция. Интервалы без пика — без поправки. out — 8192 каналов (дробные отсчёты).
bool aligned_sum(const std::vector<const Record*>& s, int lo, int hi, std::vector<double>& out, std::string& msg);

// папка: records.csv, raw/<seq>.bin, spec/s<сессия>_<номер>.csv (интервал), sum_s<сессия>.csv;
// lo/hi > 0 — колонки пика в records.csv
bool write_dir(const std::string& dir, const std::vector<Record>& recs, int lo, int hi, std::string& err);

} // namespace logdump
