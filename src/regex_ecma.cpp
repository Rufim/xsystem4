/* Copyright (C) 2026 xsystem4 contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://gnu.org/licenses/>.
 */

/*
 * Регулярные выражения для Ixseal-библиотеки `String`
 * (Search/SearchAll/Match/ReplaceRegex).
 *
 * ПОЧЕМУ C++: игра компилировалась поверх `std::regex` — синтаксис в её
 * шаблонах ECMAScript (напр. `Motion::Parser@SplitParams` передаёт
 * `([^\[\]]+?)+|\[[^\]]+?\]` с ЛЕНИВЫМИ квантификаторами, которых нет в
 * POSIX ERE). Тот же `std::regex` даёт совпадающую семантику без внешних
 * зависимостей; наружу отдаётся чистый C-API.
 *
 * КОДИРОВКА: вызывающая сторона (`src/hll/String.c`) переводит и строку, и
 * шаблон из SJIS в UTF-8, а совпадения — обратно. Сопоставление идёт по
 * БАЙТАМ, но в UTF-8 это безопасно: продолжающие байты (>= 0x80) никогда не
 * совпадают с ASCII-метасимволами, тогда как у SJIS второй байт вполне может
 * быть, например, `[` (0x5B). Побайтовость видна лишь там, где шаблон считает
 * СИМВОЛЫ (`.` или `{n}` по многобайтному тексту).
 */

#include <regex>
#include <string>

extern "C" {

// Все функции возвращают -1, если шаблон некорректен (std::regex_error), —
// вызывающая сторона логирует это и ведёт себя как «совпадений нет».
typedef void (*xs4_regex_sink)(void *user, const char *utf8, size_t len);

static bool make_regex(const char *pattern, std::regex &out)
{
	try {
		out.assign(pattern, std::regex::ECMAScript);
	} catch (const std::regex_error &) {
		return false;
	}
	return true;
}

// Есть ли совпадение где-либо в строке (std::regex_search).
int xs4_regex_search(const char *subject, const char *pattern)
{
	std::regex re;
	if (!make_regex(pattern, re))
		return -1;
	return std::regex_search(std::string(subject), re) ? 1 : 0;
}

// Совпадает ли строка ЦЕЛИКОМ (std::regex_match).
int xs4_regex_match(const char *subject, const char *pattern)
{
	std::regex re;
	if (!make_regex(pattern, re))
		return -1;
	return std::regex_match(std::string(subject), re) ? 1 : 0;
}

/*
 * Первое совпадение: отдать в sink его группы — сначала совпадение целиком
 * (m[0]), затем подгруппы, как их нумерует std::smatch.
 * Возвращает число отданных элементов (0, если совпадения нет).
 */
int xs4_regex_search_groups(const char *subject, const char *pattern,
			    xs4_regex_sink sink, void *user)
{
	std::regex re;
	if (!make_regex(pattern, re))
		return -1;
	std::string s(subject);
	std::smatch m;
	if (!std::regex_search(s, m, re))
		return 0;
	for (size_t i = 0; i < m.size(); i++) {
		std::string g = m[i].matched ? m[i].str() : std::string();
		sink(user, g.c_str(), g.size());
	}
	return (int)m.size();
}

/*
 * Все совпадения: отдать в sink каждое совпадение ЦЕЛИКОМ (m[0]).
 * Именно этого ждёт единственный сайт SearchAll — `Motion::Parser@SplitParams`
 * режет строку вида `abc[def]ghi` на токены `abc`, `[def]`, `ghi`; подгруппы
 * там служебные (внешний `+` оставил бы в группе лишь последнюю итерацию).
 * Возвращает число совпадений.
 */
int xs4_regex_search_all(const char *subject, const char *pattern,
			 xs4_regex_sink sink, void *user)
{
	std::regex re;
	if (!make_regex(pattern, re))
		return -1;
	std::string s(subject);
	int n = 0;
	for (auto it = std::sregex_iterator(s.begin(), s.end(), re);
	     it != std::sregex_iterator(); ++it) {
		std::string whole = it->str();
		// Пустое совпадение не двигает итератор осмысленно — пропускаем его,
		// чтобы шаблон вида `x*` не наплодил пустых токенов.
		if (whole.empty())
			continue;
		sink(user, whole.c_str(), whole.size());
		n++;
	}
	return n;
}

// Заменить ВСЕ совпадения (std::regex_replace); результат уходит в sink один раз.
int xs4_regex_replace(const char *subject, const char *pattern, const char *repl,
		      xs4_regex_sink sink, void *user)
{
	std::regex re;
	if (!make_regex(pattern, re))
		return -1;
	std::string out = std::regex_replace(std::string(subject), re, std::string(repl));
	sink(user, out.c_str(), out.size());
	return 0;
}

} // extern "C"
