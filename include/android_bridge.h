/* Мост движок <-> Android (TTS, читы). На не-Android платформах
 * собирается заглушкой с отладочным выводом в stdout (XS4_BRIDGE_DEBUG=1). */
#ifndef SYSTEM4_ANDROID_BRIDGE_H
#define SYSTEM4_ANDROID_BRIDGE_H

#include <stdbool.h>

struct string;

// ADV-текст (вызывается из AnteaterADVEngine, поток VM)
void bridge_adv_add_text(struct string *sjis_text);
void bridge_adv_line_break(void);
void bridge_adv_page_break(void);
void bridge_adv_add_voice(int voice_no);

// Управление (вызывается из JNI / заглушки)
void bridge_set_tts_enabled(bool on);
void bridge_set_voice_muted(bool on);
// Перелистнуть сообщение (синтетический Enter; для авто-листания после TTS)
void bridge_advance_message(void);
// Приглушить музыку до percent% исходной громкости (on) / восстановить (off)
void bridge_duck_music(bool on, int percent);

// --- Читы: доступ к переменным VM ---
// Локация: слот heap-страницы + номер переменной. Слот 0 = глобальная страница.
struct bridge_var {
	int page_slot;
	int varno;
	char *name_utf8;   // malloc; освобождать bridge_cheat_free
	int value;
};

// Глобалы верхнего уровня (int/bool/lint) с фильтром-подстрокой по имени (UTF-8).
// Возвращает количество, пишет malloc-массив в *out.
int bridge_cheat_list(const char *filter_utf8, struct bridge_var **out, int max);
// Чтение значения; *ok=0 если локация невалидна/не-int.
int bridge_cheat_read(int page_slot, int varno, int *ok);
bool bridge_cheat_write(int page_slot, int varno, int value);
// Скан по значению: narrow=false — новый обход от глобальной страницы вглубь
// (структуры/массивы), narrow=true — сужение прежних кандидатов.
int bridge_cheat_scan(int value, bool narrow, struct bridge_var **out, int max);
void bridge_cheat_free(struct bridge_var *arr, int n);

#endif /* SYSTEM4_ANDROID_BRIDGE_H */
