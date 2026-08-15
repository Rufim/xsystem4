/*
 * Проигрыватель роликов APEG (AliceSoft, System 4) поверх декодера src/apeg.c.
 *
 * Отдельно от movie_ffmpeg.c/movie_plmpeg.c: у тех интерфейс `struct movie_context`
 * под SACT-спрайт и путь `DrawMovie`, а APEG нужен movie-ЧАСТЯМ (PartsEngine
 * CreatePartsMovie/PlayPartsMovie/…), где кадр ложится в текстуру части.
 *
 * Звук: чанк `SOND` — целый файл Ogg Vorbis, отдаётся микшеру как есть.
 * Время: если звук открылся, видео синхронизируется ПО НЕМУ (как в оригинале и
 * как в movie_plmpeg.c), иначе — по стенным часам.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <SDL.h>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "system4.h"
#include "system4/archive.h"
#include "system4/file.h"

#include "apeg.h"
#include "apeg_movie.h"
#include "mixer.h"
#include "xsystem4.h"

struct apeg_movie {
	uint8_t *data;          /* весь файл: mmap или прочитанный кусок */
	size_t size;
	bool mmapped;

	struct apeg *dec;
	int width, height;

	uint8_t *pixels;        /* кадр в RGBA для текстуры части */
	const struct apeg_frame *pending;   /* декодированный, но ещё не показанный */

	struct channel *sound;
	struct archive_data *sound_data;

	bool playing, ended;
	uint32_t start_ticks;   /* SDL_GetTicks в момент старта */
	int start_ms;           /* смещение внутри ролика в момент старта */
	int last_pts;           /* метка показанного кадра */
};

/* Звук лежит внутри нашего буфера, поэтому микшеру подсовывается описатель с
 * архивом-пустышкой: освобождать надо только сам описатель. */
static void sound_free_data(struct archive_data *data)
{
	free(data);
}

static struct archive_ops sound_ops = { .free_data = sound_free_data };
static struct archive sound_archive = { .ops = &sound_ops };

static uint8_t *map_file(const char *path, size_t *size, bool *mmapped)
{
#ifndef _WIN32
	int fd = open(path, O_RDONLY);
	if (fd >= 0) {
		struct stat st;
		if (!fstat(fd, &st) && st.st_size > 0) {
			void *p = mmap(0, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
			close(fd);
			if (p != MAP_FAILED) {
				*size = st.st_size;
				*mmapped = true;
				return p;
			}
		} else {
			close(fd);
		}
	}
#endif
	/* запасной путь — прочитать целиком (ролики бывают по 200 МБ) */
	FILE *fp = file_open_utf8(path, "rb");
	if (!fp)
		return NULL;
	fseek(fp, 0, SEEK_END);
	long n = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (n <= 0) {
		fclose(fp);
		return NULL;
	}
	uint8_t *buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, fp) != (size_t)n) {
		free(buf);
		fclose(fp);
		return NULL;
	}
	fclose(fp);
	*size = (size_t)n;
	*mmapped = false;
	return buf;
}

/* Фон части задаётся В YCbCr: игра сама переводит RGB в YCbCr перед вызовом
 * CreatePartsMovie (movie::detail::CDrawMovie@SetBackColor). */
static void fill_background(struct apeg_movie *m, int y, int cb, int cr)
{
	int r = y + ((91881 * (cr - 128)) >> 16);
	int g = y - ((22554 * (cb - 128) + 46802 * (cr - 128)) >> 16);
	int b = y + ((116130 * (cb - 128)) >> 16);
	uint8_t px[4] = {
		(uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r)),
		(uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g)),
		(uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b)),
		255,
	};
	for (int i = 0; i < m->width * m->height; i++)
		memcpy(m->pixels + i * 4, px, 4);
}

/*
 * YCbCr → RGB. Матрица — BT.601 в ПОЛНОМ диапазоне (0…255), а не студийном:
 * это доказано кодом самой игры, которая обратным преобразованием переводит фон
 * (0.29891/0.58661/0.11448 — коэффициенты без масштаба 16…235), и замером потока
 * (яркость доходит до 254 и опускается ниже нуля).
 * ★Клип к 0…255 стоит только здесь: реконструкция в оригинале идёт по int16 без
 * насыщения, поэтому плоскости могут выходить за диапазон на единицы.
 */
static void frame_to_rgba(struct apeg_movie *m, const struct apeg_frame *f)
{
	for (int y = 0; y < m->height; y++) {
		const int16_t *ly = f->y + (size_t)y * f->y_stride;
		const int16_t *lcb = f->cb + (size_t)(y / 2) * f->c_stride;
		const int16_t *lcr = f->cr + (size_t)(y / 2) * f->c_stride;
		uint8_t *out = m->pixels + (size_t)y * m->width * 4;
		for (int x = 0; x < m->width; x++) {
			int yy = ly[x];
			int cb = lcb[x / 2] - 128;
			int cr = lcr[x / 2] - 128;
			int r = yy + ((91881 * cr) >> 16);
			int g = yy - ((22554 * cb + 46802 * cr) >> 16);
			int b = yy + ((116130 * cb) >> 16);
			out[0] = (uint8_t)(r < 0 ? 0 : (r > 255 ? 255 : r));
			out[1] = (uint8_t)(g < 0 ? 0 : (g > 255 ? 255 : g));
			out[2] = (uint8_t)(b < 0 ? 0 : (b > 255 ? 255 : b));
			out[3] = 255;
			out += 4;
		}
	}
}

struct apeg_movie *apeg_movie_open(const char *filename, int sound_group,
				   int back_y, int back_cb, int back_cr)
{
	char *path = gamedir_path_icase(filename);
	if (!path) {
		WARNING("%s: ролика нет", filename);
		return NULL;
	}
	struct apeg_movie *m = xcalloc(1, sizeof(struct apeg_movie));
	m->data = map_file(path, &m->size, &m->mmapped);
	if (!m->data) {
		WARNING("%s: %s", path, strerror(errno));
		free(path);
		free(m);
		return NULL;
	}
	free(path);

	m->dec = apeg_open(m->data, m->size);
	if (!m->dec) {
		WARNING("%s: не APEG или незнакомая версия", filename);
		apeg_movie_close(m);
		return NULL;
	}
	m->width = apeg_width(m->dec);
	m->height = apeg_height(m->dec);
	m->pixels = xmalloc((size_t)m->width * m->height * 4);
	fill_background(m, back_y, back_cb, back_cr);

	size_t snd_size = 0;
	const uint8_t *snd = apeg_sound(m->dec, &snd_size);
	if (snd && snd_size) {
		struct archive_data *d = xcalloc(1, sizeof(struct archive_data));
		d->size = snd_size;
		d->data = (uint8_t *)snd;
		d->no = -1;
		d->archive = &sound_archive;
		/* канал забирает описатель себе (и освобождает при ошибке) */
		m->sound = channel_open_archive_data(d);
		if (!m->sound)
			WARNING("%s: звук ролика не открылся", filename);
		else if (sound_group > 0)
			channel_set_mixer(m->sound, sound_group);
		if (m->sound && getenv("XSYS4_PARTS_TRACE"))
			NOTICE("APEG звук: %zu байт, длина %d мс, микшер %d", snd_size,
			       channel_get_time_length(m->sound), sound_group);
	} else {
		WARNING("%s: чанка SOND нет", filename);
	}
	m->last_pts = -1;
	return m;
}

void apeg_movie_close(struct apeg_movie *m)
{
	if (!m)
		return;
	if (m->sound)
		channel_close(m->sound);
	if (m->dec)
		apeg_close(m->dec);
	if (m->data) {
#ifndef _WIN32
		if (m->mmapped)
			munmap(m->data, m->size);
		else
#endif
			free(m->data);
	}
	free(m->pixels);
	free(m);
}

int apeg_movie_width(struct apeg_movie *m) { return m->width; }
int apeg_movie_height(struct apeg_movie *m) { return m->height; }
const uint8_t *apeg_movie_pixels(struct apeg_movie *m) { return m->pixels; }
int apeg_movie_end_time(struct apeg_movie *m) { return (int)apeg_duration_ms(m->dec); }

static int movie_now(struct apeg_movie *m)
{
	if (m->sound && channel_is_playing(m->sound))
		return channel_get_pos(m->sound);
	return m->start_ms + (int)(SDL_GetTicks() - m->start_ticks);
}

int apeg_movie_current_time(struct apeg_movie *m)
{
	return m->playing ? movie_now(m) : m->start_ms;
}

bool apeg_movie_play(struct apeg_movie *m, int start_ms)
{
	/* ★Перематываем ВСЕГДА, в том числе на ноль: повторный Play (игра проиграла
	 * ролик и зовёт его снова) заставал бы декодер на конце потока, и ролик
	 * «кончался» бы мгновенно. */
	if (start_ms < 0)
		start_ms = 0;
	apeg_seek(m->dec, (uint32_t)start_ms);
	if (m->sound)
		channel_seek(m->sound, start_ms);
	m->start_ms = start_ms;
	m->start_ticks = SDL_GetTicks();
	m->pending = NULL;
	m->last_pts = -1;
	m->ended = false;
	m->playing = true;
	if (m->sound) {
		channel_play(m->sound);
		if (getenv("XSYS4_PARTS_TRACE"))
			NOTICE("APEG старт с %d мс: канал играет=%d, позиция %d мс", start_ms,
			       channel_is_playing(m->sound), channel_get_pos(m->sound));
	}
	return true;
}

void apeg_movie_set_time(struct apeg_movie *m, int ms)
{
	if (ms < 0)
		ms = 0;
	apeg_seek(m->dec, (uint32_t)ms);
	m->pending = NULL;
	m->last_pts = -1;
	m->start_ms = ms;
	m->start_ticks = SDL_GetTicks();
	if (m->sound)
		channel_seek(m->sound, ms);
}

bool apeg_movie_is_end(struct apeg_movie *m)
{
	/* ★Конец считаем и здесь, а не только в apeg_movie_update: кадры тикают лишь у
	 * ТЕКУЩЕГО состояния части, и если игра переключит состояние на середине ролика,
	 * обновления перестанут приходить. Без этой проверки ожидание конца ролика
	 * повисло бы навсегда. */
	if (!m->ended && m->playing && movie_now(m) >= apeg_movie_end_time(m))
		m->ended = true;
	return m->ended;
}

bool apeg_movie_update(struct apeg_movie *m)
{
	if (!m->playing || m->ended)
		return false;

	int now = movie_now(m);
	const struct apeg_frame *show = NULL;

	for (;;) {
		const struct apeg_frame *f = m->pending;
		m->pending = NULL;
		if (!f)
			f = apeg_next_frame(m->dec);
		if (!f) {
			/* картинки кончились — ждём, пока доиграет звук */
			if (now >= apeg_movie_end_time(m) ||
			    !(m->sound && channel_is_playing(m->sound)))
				m->ended = true;
			break;
		}
		if ((int)f->pts_ms > now) {
			m->pending = f;
			break;
		}
		show = f;
		/* если декодер отстал, промежуточные кадры не переводим в RGBA */
	}

	if (!show || (int)show->pts_ms == m->last_pts)
		return false;
	m->last_pts = (int)show->pts_ms;
	frame_to_rgba(m, show);
	return true;
}
