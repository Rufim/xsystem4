/* Декодер роликов APEG (AliceSoft, System 4).
 *
 * Формат — MPEG-1 в своей обёртке: свой контейнер и свои коды VLC, всё остальное
 * (макроблоки, деквантование, IDCT, компенсация движения) как в MPEG-1.
 * Спецификация и эталонный декодер на питоне — в проекте разбора движка,
 * analysis/apeg/FORMAT.md.
 *
 * Пара apeg.c + apeg.h НИЧЕГО не включает из xsystem4 — только стандартную
 * библиотеку, вход — указатель на буфер с файлом. Поэтому один и тот же код
 * собирается и в тестовую утилиту, и в движок:
 *
 *     gcc -O2 -Iinclude -o /tmp/apegdec src/apeg.c tools/apegdec.c
 */

#ifndef SYSTEM4_APEG_H
#define SYSTEM4_APEG_H

#include <stdint.h>
#include <stddef.h>

struct apeg;

/* Кадр: три плоскости 4:2:0.
 *
 * ★Отсчёты знаковые 16-битные и НЕ ОГРАНИЧЕНЫ диапазоном 0…255 — так же, как в
 * оригинальном движке, где реконструкция идёт без насыщения, а клип происходит
 * только при выводе. Значит потребитель обязан сам зажимать значения в 0…255.
 */
struct apeg_frame {
	int width, height;         /* видимый размер (может быть не кратен 16) */
	int y_stride, c_stride;    /* шаг плоскостей в отсчётах (кратен 16) */
	const int16_t *y, *cb, *cr;
	uint32_t pts_ms;           /* метка времени показа */
};

/* Открыть ролик, лежащий в памяти. Буфер должен пережить декодер (он не копируется).
 * Возвращает NULL, если это не APEG или версия незнакомая. */
struct apeg *apeg_open(const uint8_t *data, size_t size);
void apeg_close(struct apeg *a);

/* Описание последней ошибки (пустая строка — ошибок не было). */
const char *apeg_error(struct apeg *a);

int apeg_width(struct apeg *a);
int apeg_height(struct apeg *a);
uint32_t apeg_duration_ms(struct apeg *a);

/* Звук: чанк SOND — целый файл Ogg Vorbis, отдаётся как есть, декодировать нечего.
 * Указатель — внутрь исходного буфера; NULL, если звука нет. */
const uint8_t *apeg_sound(struct apeg *a, size_t *size);

/* Эскиз из чанка TMNL: сжатые zlib данные BGRA8888. Распаковка — на потребителе
 * (декодер не тянет за собой zlib). NULL, если эскиза нет. */
const uint8_t *apeg_thumbnail(struct apeg *a, size_t *size, int *width, int *height);

/* Следующий кадр В ПОРЯДКЕ ПОКАЗА (B-картинки переставляются). NULL — конец ролика
 * или ошибка (см. apeg_error). Кадр действителен до следующего вызова apeg_next_frame
 * или apeg_seek. */
const struct apeg_frame *apeg_next_frame(struct apeg *a);

/* Перемотка на последний ключевой кадр с меткой не позже ms; следующий apeg_next_frame
 * вернёт кадр начиная с него. Возвращает метку времени этого ключевого кадра. */
uint32_t apeg_seek(struct apeg *a, uint32_t ms);

#endif /* SYSTEM4_APEG_H */
