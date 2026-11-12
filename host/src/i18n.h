// i18n.h — строки интерфейса пульта по ключам, русский и английский. Без ImGui.
// Ключи: <экран>.<что> (top.*, status.*, home.*, conn.*, wiz.*, wiz.step.<шаг>.*, adv.nav.*, adv.*).
#pragma once
#include <cstddef>
#include <string>
#include <vector>

enum class Lang { Ru, En };

struct I18nEntry { const char* key; const char* ru; const char* en; };

void i18n_set(Lang l);
Lang i18n_get();

// Строка на текущем языке. Нет ключа: возвращает сам ключ и запоминает его в i18n_missing().
const char* T(const char* key);

// Ключи, которых не нашлось (каждый один раз) — для журнала.
const std::vector<std::string>& i18n_missing();

// Число записей в таблице.
size_t i18n_count();

// Вычитка таблицы: ключи уникальны, ru и en непустые, нет «—» (U+2014) и «–» (U+2013),
// кроме «–» между цифрами (диапазон «3–5»). false — нарушения перечислены в report.
bool i18n_check(std::string& report);
bool i18n_check_entries(const I18nEntry* e, size_t n, std::string& report);
