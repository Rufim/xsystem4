/* Тестовая утилита к декодеру APEG: гоняет src/apeg.c без всякого движка.
 *
 *     gcc -O2 -Iinclude -o /tmp/apegdec src/apeg.c tools/apegdec.c
 *     /tmp/apegdec Movie/Op.apeg 150 /tmp/out.raw
 *
 * Аргументы: файл, сколько кадров декодировать (по умолчанию 1), куда записать
 * последний кадр. Расширение решает формат: .ppm — картинка RGB (BT.601, полный
 * диапазон), иначе — сырые плоскости int16 (Y, Cb, Cr) для ПОБАЙТНОЙ сверки с
 * питоновским эталоном. По каждому кадру печатается метка времени и контрольная
 * сумма плоскостей — этого хватает, чтобы найти первый разошедшийся кадр.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apeg.h"

static uint8_t *slurp(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	uint8_t *buf;
	long n;

	if (!f) {
		perror(path);
		return NULL;
	}
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc((size_t)n);
	if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
		fprintf(stderr, "не прочитан %s\n", path);
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	*size = (size_t)n;
	return buf;
}

static uint64_t plane_sum(const int16_t *p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	size_t i;
	for (i = 0; i < n; i++) {
		h ^= (uint16_t)p[i];
		h *= 1099511628211ull;
	}
	return h;
}

static int clamp8(int v)
{
	return v < 0 ? 0 : (v > 255 ? 255 : v);
}

static void write_ppm(const char *path, const struct apeg_frame *f)
{
	FILE *o = fopen(path, "wb");
	int x, y;

	if (!o) {
		perror(path);
		return;
	}
	fprintf(o, "P6\n%d %d\n255\n", f->width, f->height);
	for (y = 0; y < f->height; y++) {
		for (x = 0; x < f->width; x++) {
			int yy = f->y[(size_t)y * f->y_stride + x];
			int cb = f->cb[(size_t)(y / 2) * f->c_stride + x / 2] - 128;
			int cr = f->cr[(size_t)(y / 2) * f->c_stride + x / 2] - 128;
			uint8_t px[3];
			px[0] = (uint8_t)clamp8(yy + ((91881 * cr) >> 16));
			px[1] = (uint8_t)clamp8(yy - ((22554 * cb + 46802 * cr) >> 16));
			px[2] = (uint8_t)clamp8(yy + ((116130 * cb) >> 16));
			fwrite(px, 1, 3, o);
		}
	}
	fclose(o);
}

static void write_raw(const char *path, const struct apeg_frame *f)
{
	FILE *o = fopen(path, "wb");
	int y;

	if (!o) {
		perror(path);
		return;
	}
	for (y = 0; y < f->height; y++)
		fwrite(f->y + (size_t)y * f->y_stride, sizeof(int16_t), (size_t)f->width, o);
	for (y = 0; y < f->height / 2; y++)
		fwrite(f->cb + (size_t)y * f->c_stride, sizeof(int16_t), (size_t)f->width / 2, o);
	for (y = 0; y < f->height / 2; y++)
		fwrite(f->cr + (size_t)y * f->c_stride, sizeof(int16_t), (size_t)f->width / 2, o);
	fclose(o);
}

int main(int argc, char *argv[])
{
	const char *path, *out = NULL;
	int count = 1, n = 0;
	long until_pts = -1;
	size_t size, snd_size = 0;
	uint8_t *data;
	struct apeg *a;
	const struct apeg_frame *f = NULL, *last = NULL;

	if (argc < 2) {
		fprintf(stderr, "использование: apegdec <файл.apeg> [кадров] [выход.ppm|.raw]\n");
		return 2;
	}
	path = argv[1];
	if (argc > 2) {
		/* «@3300» — гнать до кадра с этой меткой времени: так кадр в порядке
		 * показа сводится с картинкой, которую эталон декодирует в порядке
		 * кодирования (метка уникальна). */
		if (argv[2][0] == '@')
			until_pts = (long)atoi(argv[2] + 1);
		else
			count = atoi(argv[2]);
	}
	if (argc > 3)
		out = argv[3];

	data = slurp(path, &size);
	if (!data)
		return 1;
	a = apeg_open(data, size);
	if (!a) {
		fprintf(stderr, "%s: не APEG или незнакомая версия\n", path);
		free(data);
		return 1;
	}
	apeg_sound(a, &snd_size);
	printf("%s: %dx%d, %u мс, звука %zu байт\n", path, apeg_width(a), apeg_height(a),
	       apeg_duration_ms(a), snd_size);

	while ((until_pts >= 0 || n < count) && (f = apeg_next_frame(a))) {
		size_t ny = (size_t)f->y_stride * ((f->height + 15) / 16 * 16);
		n++;
		printf("  кадр %3d  t=%6u мс  Y=%016llx Cb=%016llx Cr=%016llx\n", n, f->pts_ms,
		       (unsigned long long)plane_sum(f->y, ny),
		       (unsigned long long)plane_sum(f->cb, ny / 4),
		       (unsigned long long)plane_sum(f->cr, ny / 4));
		last = f;
		if (until_pts >= 0 && (long)f->pts_ms >= until_pts)
			break;
	}
	if (*apeg_error(a))
		fprintf(stderr, "ошибка: %s\n", apeg_error(a));
	if (!last) {
		fprintf(stderr, "ни одного кадра\n");
		apeg_close(a);
		free(data);
		return 1;
	}
	if (out) {
		size_t len = strlen(out);
		if (len > 4 && !strcmp(out + len - 4, ".ppm"))
			write_ppm(out, last);
		else
			write_raw(out, last);
		printf("записано %s (кадр %d, t=%u мс)\n", out, n, last->pts_ms);
	}
	apeg_close(a);
	free(data);
	return 0;
}
