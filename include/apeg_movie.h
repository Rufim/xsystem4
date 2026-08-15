/*
 * Проигрыватель роликов APEG для movie-ЧАСТЕЙ (см. src/movie_apeg.c).
 * Кадр отдаётся готовыми пикселями RGBA — их кладёт в свою текстуру часть.
 */

#ifndef SYSTEM4_APEG_MOVIE_H
#define SYSTEM4_APEG_MOVIE_H

#include <stdbool.h>
#include <stdint.h>

struct apeg_movie;

/* Фон задаётся В YCbCr — так его передаёт игра (она сама переводит RGB).
 * sound_group — номер микшера, в который уходит звук ролика. */
struct apeg_movie *apeg_movie_open(const char *filename, int sound_group,
				   int back_y, int back_cb, int back_cr);
void apeg_movie_close(struct apeg_movie *m);

int apeg_movie_width(struct apeg_movie *m);
int apeg_movie_height(struct apeg_movie *m);
const uint8_t *apeg_movie_pixels(struct apeg_movie *m);

bool apeg_movie_play(struct apeg_movie *m, int start_ms);
void apeg_movie_set_time(struct apeg_movie *m, int ms);
bool apeg_movie_is_end(struct apeg_movie *m);
int apeg_movie_current_time(struct apeg_movie *m);
int apeg_movie_end_time(struct apeg_movie *m);

/* Догнать кадр до текущего времени; true — картинка сменилась. */
bool apeg_movie_update(struct apeg_movie *m);

#endif /* SYSTEM4_APEG_MOVIE_H */
