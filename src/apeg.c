/* Декодер роликов APEG (AliceSoft, System 4) — см. include/apeg.h.
 *
 * Всё, что здесь написано, снято с dohnadohna.exe (спецификация — analysis/apeg/FORMAT.md
 * в проекте разбора движка). Места, где формат расходится с учебным MPEG-1, помечены ★.
 *
 * Файл намеренно не зависит ни от чего из xsystem4.
 */

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "apeg.h"
#include "apeg_tables.h"

#define APEG_NFRAMES 4   /* вперёдная опора, назадная опора, отданный кадр и рабочий */

/* флаги mb_type — те же, что в MPEG-1 */
#define MB_INTRA    0x01
#define MB_PATTERN  0x02
#define MB_BACKWARD 0x04
#define MB_FORWARD  0x08
#define MB_QUANT    0x10

static const uint8_t ZIGZAG[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

/* предмасштаб под целочисленный IDCT (таблица по адресу 0x80d408 в exe) */
static const int32_t PRESCALE[64] = {
	32, 44, 42, 38, 32, 25, 17, 9,
	44, 62, 58, 52, 44, 35, 24, 12,
	42, 58, 55, 49, 42, 33, 23, 12,
	38, 52, 49, 44, 38, 30, 20, 10,
	32, 44, 42, 38, 32, 25, 17, 9,
	25, 35, 33, 30, 25, 20, 14, 7,
	17, 24, 23, 20, 17, 14, 9, 5,
	9, 12, 12, 10, 9, 7, 5, 2,
};

struct vlc {
	int bits;
	uint32_t *lut;   /* (длина << 16) | (значение & 0xffff); длина 0 — кода нет */
};

struct bits {
	const uint8_t *d;
	size_t size;     /* байт */
	size_t pos;      /* бит */
	int ok;
};

struct frame {
	int16_t *plane;  /* одним куском: Y, затем Cb, Cr */
	int16_t *y, *cb, *cr;
	uint32_t pts;
};

struct apeg {
	const uint8_t *data;
	size_t size;

	int version;
	int width, height;
	uint32_t duration_ms;
	uint32_t n_index;
	const uint8_t *index;     /* n_index пар (метка, смещение) */
	uint8_t intra_q[64], inter_q[64];

	size_t data_start;
	size_t pos;               /* смещение следующего чанка */

	const uint8_t *sound;
	size_t sound_size;
	const uint8_t *thumb;
	size_t thumb_size;
	int thumb_w, thumb_h;

	int mbw, mbh, mb_count;
	int pw, ph;               /* размер плоскостей, кратный 16 */

	struct frame frames[APEG_NFRAMES];
	struct frame *fwd, *bwd, *last_out;
	struct apeg_frame out;

	struct vlc coeff, mbtype_p, mbtype_b, cbp, mba, mv, dc_luma, dc_chroma;

	int32_t coefs[64];        /* деквантованный блок */
	int16_t blocks[6][64];    /* макроблок после IDCT: 4×Y, Cb, Cr */
	int32_t tmp[16 * 16];     /* предсказание одного блока */

	int skip_to_key;
	int eof;
	char err[192];
};

static void fail(struct apeg *a, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(a->err, sizeof a->err, fmt, ap);
	va_end(ap);
}

/* ------------------------------------------------------------------ биты */

static uint32_t bits_peek(struct bits *b, int n)
{
	size_t byte = b->pos >> 3;
	int off = (int)(b->pos & 7);
	uint64_t w = 0;
	int i;
	for (i = 0; i < 5; i++)
		w = (w << 8) | (byte + i < b->size ? b->d[byte + i] : 0);
	return (uint32_t)((w >> (40 - off - n)) & ((1u << n) - 1));
}

static uint32_t bits_get(struct bits *b, int n)
{
	uint32_t v = bits_peek(b, n);
	b->pos += n;
	return v;
}

static int vlc_get(struct bits *b, const struct vlc *v)
{
	uint32_t e = v->lut[bits_peek(b, v->bits)];
	int len = (int)(e >> 16);
	if (!len) {
		b->ok = 0;
		return 0;
	}
	b->pos += len;
	return (int16_t)(e & 0xffff);
}

static int vlc_build(struct vlc *v, const struct apeg_vlc_entry *e, size_t n, int bits)
{
	size_t i, j;
	v->bits = bits;
	v->lut = calloc((size_t)1 << bits, sizeof(uint32_t));
	if (!v->lut)
		return 0;
	for (i = 0; i < n; i++) {
		size_t base = (size_t)e[i].code << (bits - e[i].len);
		size_t cnt = (size_t)1 << (bits - e[i].len);
		uint32_t val = ((uint32_t)e[i].len << 16) | (uint16_t)e[i].val;
		for (j = 0; j < cnt; j++)
			v->lut[base + j] = val;
	}
	return 1;
}

/* ------------------------------------------------------- блоки и обратное DCT */

/* клип ±2048 и «оддификация» — ровно как в exe (sub_44b330) */
static int32_t fixup(int32_t v)
{
	if (v > 2047)
		v = 2047;
	else if (v < -2048)
		v = -2048;
	if (!(v & 1))
		v += (v > 0) ? 1 : -1;
	return v;
}

/* целочисленный IDCT MPEG-1 (константы 473/196/362), sub_44afd0: вход int32, выход int16 */
static void idct(int32_t *b, int16_t *dst)
{
	int i;
	for (i = 0; i < 8; i++) {
		int32_t b1 = b[4 * 8 + i];
		int32_t b3 = b[2 * 8 + i] + b[6 * 8 + i];
		int32_t b4 = b[5 * 8 + i] - b[3 * 8 + i];
		int32_t t1 = b[1 * 8 + i] + b[7 * 8 + i];
		int32_t t2 = b[3 * 8 + i] + b[5 * 8 + i];
		int32_t b6 = b[1 * 8 + i] - b[7 * 8 + i];
		int32_t b7 = t1 + t2;
		int32_t m0 = b[0 * 8 + i];
		int32_t x4 = ((b6 * 473 - b4 * 196 + 128) >> 8) - b7;
		int32_t x0 = x4 - (((t1 - t2) * 362 + 128) >> 8);
		int32_t x1 = m0 - b1;
		int32_t x2 = (((b[2 * 8 + i] - b[6 * 8 + i]) * 362 + 128) >> 8) - b3;
		int32_t x3 = m0 + b1;
		int32_t y3 = x1 + x2;
		int32_t y4 = x3 + b3;
		int32_t y5 = x1 - x2;
		int32_t y6 = x3 - b3;
		int32_t y7 = -x0 - ((b4 * 473 + b6 * 196 + 128) >> 8);
		b[0 * 8 + i] = b7 + y4;
		b[1 * 8 + i] = x4 + y3;
		b[2 * 8 + i] = y5 - x0;
		b[3 * 8 + i] = y6 - y7;
		b[4 * 8 + i] = y6 + y7;
		b[5 * 8 + i] = x0 + y5;
		b[6 * 8 + i] = y3 - x4;
		b[7 * 8 + i] = y4 - b7;
	}
	for (i = 0; i < 64; i += 8) {
		int32_t b1 = b[4 + i];
		int32_t b3 = b[2 + i] + b[6 + i];
		int32_t b4 = b[5 + i] - b[3 + i];
		int32_t t1 = b[1 + i] + b[7 + i];
		int32_t t2 = b[3 + i] + b[5 + i];
		int32_t b6 = b[1 + i] - b[7 + i];
		int32_t b7 = t1 + t2;
		int32_t m0 = b[0 + i];
		int32_t x4 = ((b6 * 473 - b4 * 196 + 128) >> 8) - b7;
		int32_t x0 = x4 - (((t1 - t2) * 362 + 128) >> 8);
		int32_t x1 = m0 - b1;
		int32_t x2 = (((b[2 + i] - b[6 + i]) * 362 + 128) >> 8) - b3;
		int32_t x3 = m0 + b1;
		int32_t y3 = x1 + x2;
		int32_t y4 = x3 + b3;
		int32_t y5 = x1 - x2;
		int32_t y6 = x3 - b3;
		int32_t y7 = -x0 - ((b4 * 473 + b6 * 196 + 128) >> 8);
		dst[0 + i] = (int16_t)((b7 + y4 + 128) >> 8);
		dst[1 + i] = (int16_t)((x4 + y3 + 128) >> 8);
		dst[2 + i] = (int16_t)((y5 - x0 + 128) >> 8);
		dst[3 + i] = (int16_t)((y6 - y7 + 128) >> 8);
		dst[4 + i] = (int16_t)((y6 + y7 + 128) >> 8);
		dst[5 + i] = (int16_t)((x0 + y5 + 128) >> 8);
		dst[6 + i] = (int16_t)((y3 - x4 + 128) >> 8);
		dst[7 + i] = (int16_t)((y4 - b7 + 128) >> 8);
	}
}

/* Разбор и деквантование одного блока. Возвращает натуральный индекс последнего
 * записанного коэффициента (для сокращения «в блоке только DC»). */
static int decode_coefs(struct apeg *a, struct bits *b, int intra, int qscale,
			const uint8_t *qm)
{
	int n = intra ? 1 : 0;
	int last = 0;

	for (;;) {
		int run, level, idx;
		int32_t v;

		if (bits_peek(b, 1) == 0) {
			b->pos++;
			if (n == 0) {
				/* ★первый коэффициент не-intra блока: один бит знака */
				level = bits_get(b, 1) ? -1 : 1;
				run = 0;
			} else {
				/* «00» — конец блока, «01»+знак — (run 0, level ±1) */
				if (bits_get(b, 1) == 0)
					break;
				level = bits_get(b, 1) ? -1 : 1;
				run = 0;
			}
		} else {
			int code = vlc_get(b, &a->coeff);
			if (!b->ok)
				return -1;
			if (code == -2) {
				/* escape: 6 бит run, 8 бит level (ещё 8 при 0 и −128) */
				run = (int)bits_get(b, 6);
				level = (int)bits_get(b, 8);
				if (level == 0)
					level = (int)bits_get(b, 8);
				else if (level == 0x80)
					level = (int)bits_get(b, 8) - 256;
				else if (level > 0x80)
					level -= 256;
			} else {
				int mag = code & 0xff;
				run = (code >> 8) & 0xff;
				level = bits_get(b, 1) ? -mag : mag;
			}
		}

		n += run;
		if (n >= 64) {
			b->ok = 0;
			return -1;
		}
		idx = ZIGZAG[n];
		if (intra)
			v = 2 * level * qm[idx] * qscale;
		else
			v = (2 * level + (level > 0 ? 1 : -1)) * qm[idx] * qscale;
		v /= 16;   /* деление к нулю */
		a->coefs[idx] = fixup(v) * PRESCALE[idx];
		last = idx;
		n++;
	}
	return last;
}

/* Один блок целиком: коэффициенты, деквантование, обратное преобразование. */
static int decode_block(struct apeg *a, struct bits *b, int16_t *dst, int intra,
			int dc_pred, int qscale, const uint8_t *qm)
{
	int last;

	memset(a->coefs, 0, sizeof a->coefs);
	if (intra)
		a->coefs[0] = fixup(dc_pred * 8) * PRESCALE[0];
	last = decode_coefs(a, b, intra, qscale, qm);
	if (last < 0)
		return 0;
	if (last == 0) {
		/* ★в блоке только DC — IDCT не зовут, блок заливают coefs[0]/256 (к нулю) */
		int16_t fill = (int16_t)(a->coefs[0] / 256);
		int i;
		for (i = 0; i < 64; i++)
			dst[i] = fill;
	} else {
		idct(a->coefs, dst);
	}
	return 1;
}

/* -------------------------------------------------------- компенсация движения */

/* Предсказание квадрата size×size из опорной плоскости в a->tmp.
 * Вектор — в полупикселях; целая часть — арифметический сдвиг, полупиксель — младший бит. */
static void predict(struct apeg *a, const int16_t *ref, int stride, int w, int h,
		    int x0, int y0, int size, int mvx, int mvy)
{
	int sx = x0 + (mvx >> 1), sy = y0 + (mvy >> 1);
	int hx = mvx & 1, hy = mvy & 1;
	int32_t *out = a->tmp;
	int i, j;

	if (sx >= 0 && sy >= 0 && sx + size + hx <= w && sy + size + hy <= h) {
		const int16_t *p = ref + (size_t)sy * stride + sx;
		for (j = 0; j < size; j++, p += stride) {
			for (i = 0; i < size; i++) {
				int v;
				if (hx && hy)
					v = (p[i] + p[i + 1] + p[i + stride] + p[i + stride + 1] + 2) / 4;
				else if (hx)
					v = (p[i] + p[i + 1] + 1) / 2;
				else if (hy)
					v = (p[i] + p[i + stride] + 1) / 2;
				else
					v = p[i];
				*out++ = v;
			}
		}
		return;
	}

	/* Край кадра. ★В оригинале координаты не проверяются вовсе — MPEG-1 обещает,
	 * что вектор не выводит за кадр; мы зажимаем, чтобы битый файл не уронил движок. */
	for (j = 0; j < size; j++) {
		int y = sy + j, y1 = y + hy;
		if (y < 0) y = 0; else if (y >= h) y = h - 1;
		if (y1 < 0) y1 = 0; else if (y1 >= h) y1 = h - 1;
		for (i = 0; i < size; i++) {
			int x = sx + i, x1 = x + hx, v;
			if (x < 0) x = 0; else if (x >= w) x = w - 1;
			if (x1 < 0) x1 = 0; else if (x1 >= w) x1 = w - 1;
			if (hx && hy)
				v = (ref[(size_t)y * stride + x] + ref[(size_t)y * stride + x1] +
				     ref[(size_t)y1 * stride + x] + ref[(size_t)y1 * stride + x1] + 2) / 4;
			else if (hx)
				v = (ref[(size_t)y * stride + x] + ref[(size_t)y * stride + x1] + 1) / 2;
			else if (hy)
				v = (ref[(size_t)y * stride + x] + ref[(size_t)y1 * stride + x] + 1) / 2;
			else
				v = ref[(size_t)y * stride + x];
			*out++ = v;
		}
	}
}

/* dst += a->tmp (16-битное сложение с заворотом, как в exe — без насыщения) */
static void add_pred(int16_t *dst, int stride, int x0, int y0, int size, const int32_t *pred)
{
	int i, j;
	for (j = 0; j < size; j++) {
		int16_t *row = dst + (size_t)(y0 + j) * stride + x0;
		for (i = 0; i < size; i++)
			row[i] = (int16_t)(row[i] + pred[j * size + i]);
	}
}

/* dst += (первое предсказание + второе + 1) / 2, деление к нулю (sub_4506b0) */
static void add_pred_avg(int16_t *dst, int stride, int x0, int y0, int size,
			 const int32_t *p1, const int32_t *p2)
{
	int i, j;
	for (j = 0; j < size; j++) {
		int16_t *row = dst + (size_t)(y0 + j) * stride + x0;
		for (i = 0; i < size; i++) {
			int s = p1[j * size + i] + 1 + p2[j * size + i];
			row[i] = (int16_t)(row[i] + s / 2);
		}
	}
}

/* Выложить шесть блоков макроблока в кадр (sub_44b850): простая копия, без клипа. */
static void put_blocks(struct apeg *a, struct frame *f, int mb)
{
	int mx = (mb % a->mbw) * 16, my = (mb / a->mbw) * 16;
	int i, j, k;

	for (k = 0; k < 4; k++) {
		int16_t *dst = f->y + (size_t)(my + (k >> 1) * 8) * a->pw + mx + (k & 1) * 8;
		for (j = 0; j < 8; j++)
			for (i = 0; i < 8; i++)
				dst[(size_t)j * a->pw + i] = a->blocks[k][j * 8 + i];
	}
	for (k = 4; k < 6; k++) {
		int16_t *dst = (k == 4 ? f->cb : f->cr) + (size_t)(my / 2) * (a->pw / 2) + mx / 2;
		for (j = 0; j < 8; j++)
			for (i = 0; i < 8; i++)
				dst[(size_t)j * (a->pw / 2) + i] = a->blocks[k][j * 8 + i];
	}
}

/* Плоская копия макроблока из опорной картинки (sub_44bb10) — пропуск в P-картинке. */
static void copy_mb(struct apeg *a, struct frame *dst, const struct frame *ref, int mb)
{
	int mx = (mb % a->mbw) * 16, my = (mb / a->mbw) * 16;
	int cw = a->pw / 2;
	int j;

	for (j = 0; j < 16; j++)
		memcpy(dst->y + (size_t)(my + j) * a->pw + mx,
		       ref->y + (size_t)(my + j) * a->pw + mx, 16 * sizeof(int16_t));
	for (j = 0; j < 8; j++) {
		size_t off = (size_t)(my / 2 + j) * cw + mx / 2;
		memcpy(dst->cb + off, ref->cb + off, 8 * sizeof(int16_t));
		memcpy(dst->cr + off, ref->cr + off, 8 * sizeof(int16_t));
	}
}

/* Прибавить предсказание к уже лежащему в кадре остатку. */
static void compensate(struct apeg *a, struct frame *cur, int mb, int mbt,
		       int fx, int fy, int bx, int by)
{
	int mx = (mb % a->mbw) * 16, my = (mb / a->mbw) * 16;
	int cw = a->pw / 2, ch = a->ph / 2;
	int32_t second[16 * 16];
	int bidir = (mbt & MB_FORWARD) && (mbt & MB_BACKWARD);
	/* вектор цветности — половина с усечением К НУЛЮ */
	int cfx = fx / 2, cfy = fy / 2, cbx = bx / 2, cby = by / 2;

	if (bidir) {
		predict(a, a->bwd->y, a->pw, a->pw, a->ph, mx, my, 16, bx, by);
		memcpy(second, a->tmp, 16 * 16 * sizeof(int32_t));
		predict(a, a->fwd->y, a->pw, a->pw, a->ph, mx, my, 16, fx, fy);
		add_pred_avg(cur->y, a->pw, mx, my, 16, a->tmp, second);

		predict(a, a->bwd->cb, cw, cw, ch, mx / 2, my / 2, 8, cbx, cby);
		memcpy(second, a->tmp, 8 * 8 * sizeof(int32_t));
		predict(a, a->fwd->cb, cw, cw, ch, mx / 2, my / 2, 8, cfx, cfy);
		add_pred_avg(cur->cb, cw, mx / 2, my / 2, 8, a->tmp, second);

		predict(a, a->bwd->cr, cw, cw, ch, mx / 2, my / 2, 8, cbx, cby);
		memcpy(second, a->tmp, 8 * 8 * sizeof(int32_t));
		predict(a, a->fwd->cr, cw, cw, ch, mx / 2, my / 2, 8, cfx, cfy);
		add_pred_avg(cur->cr, cw, mx / 2, my / 2, 8, a->tmp, second);
		return;
	}

	{
		struct frame *ref = (mbt & MB_BACKWARD) ? a->bwd : a->fwd;
		int vx = (mbt & MB_BACKWARD) ? bx : fx;
		int vy = (mbt & MB_BACKWARD) ? by : fy;
		int cvx = vx / 2, cvy = vy / 2;

		predict(a, ref->y, a->pw, a->pw, a->ph, mx, my, 16, vx, vy);
		add_pred(cur->y, a->pw, mx, my, 16, a->tmp);
		predict(a, ref->cb, cw, cw, ch, mx / 2, my / 2, 8, cvx, cvy);
		add_pred(cur->cb, cw, mx / 2, my / 2, 8, a->tmp);
		predict(a, ref->cr, cw, cw, ch, mx / 2, my / 2, 8, cvx, cvy);
		add_pred(cur->cr, cw, mx / 2, my / 2, 8, a->tmp);
	}
}

/* -------------------------------------------------------------- уровень картинки */

static int read_mv(struct apeg *a, struct bits *b, int pred, int r)
{
	int code = vlc_get(b, &a->mv);
	int delta = 0, f, mv;

	if (!b->ok)
		return pred;
	if (code != 0) {
		int sign = (int)bits_get(b, 1);
		if (r == 0)
			delta = code;
		else
			delta = (code - 1) * (1 << r) + (int)bits_get(b, r) + 1;
		if (sign)
			delta = -delta;
	}
	f = 1 << r;
	mv = pred + delta;
	if (mv >= 16 * f)
		mv -= 32 * f;
	else if (mv < -16 * f)
		mv += 32 * f;
	return mv;
}

/* ★приращение адреса макроблока лежит в КОНЦЕ макроблока, escape прибавляет 35 */
static int read_mba(struct apeg *a, struct bits *b)
{
	int total = 0;
	for (;;) {
		int v = vlc_get(b, &a->mba);
		if (!b->ok)
			return 0;
		if (v == -1) {
			total += 35;
			continue;
		}
		return total + v;
	}
}

static int decode_picture(struct apeg *a, int kind, const uint8_t *p, size_t n,
			  struct frame *cur)
{
	struct bits b = { p, n, 32, 1 };
	int r_f = 0, r_b = 0, full_pel_f = 0, full_pel_b = 0;
	int qscale = 1;
	int pred_y = 128, pred_cb = 128, pred_cr = 128;
	int fx = 0, fy = 0, bx = 0, by = 0;
	int mb = 0;

	cur->pts = (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;

	if (kind != 'I') {
		full_pel_f = (int)bits_get(&b, 1);
		r_f = (int)bits_get(&b, 3);
		if (kind == 'B') {
			full_pel_b = (int)bits_get(&b, 1);
			r_b = (int)bits_get(&b, 3);
		} else {
			bits_get(&b, 4);   /* выравнивание до байта */
		}
	}

	while (mb < a->mb_count) {
		int mbt, cbp, skip = 0, i, k;
		int vfx, vfy, vbx, vby;

		if (kind == 'I') {
			mbt = MB_INTRA | (bits_get(&b, 1) ? MB_QUANT : 0);
			cbp = 0x3f;
		} else {
			mbt = vlc_get(&b, kind == 'P' ? &a->mbtype_p : &a->mbtype_b);
			if (!b.ok)
				break;
			if (mbt & MB_FORWARD) {
				fx = read_mv(a, &b, fx, r_f);
				fy = read_mv(a, &b, fy, r_f);
			}
			if (mbt & MB_BACKWARD) {
				bx = read_mv(a, &b, bx, r_b);
				by = read_mv(a, &b, by, r_b);
			}
			/* ★нет ни одного направления (то есть intra) — все четыре
			 * предсказателя вектора обнуляются */
			if (!(mbt & (MB_FORWARD | MB_BACKWARD)))
				fx = fy = bx = by = 0;
			cbp = (mbt & MB_PATTERN) ? vlc_get(&b, &a->cbp)
						 : ((mbt & MB_INTRA) ? 0x3f : 0);
		}
		if (mbt & MB_QUANT)
			qscale = (int)bits_get(&b, 5);
		if (kind != 'I')
			skip = read_mba(a, &b);
		if (!b.ok)
			break;
		/* ★Приращение адреса ничем не ограничено (escape прибавляет по 35 сколько
		 * угодно раз), а дальше по нему пишутся макроблоки. У целого файла оно
		 * всегда укладывается в картинку; у битого — выводило бы за плоскости,
		 * поэтому это не «на всякий случай», а признак негодного потока. */
		if (skip > a->mb_count - 1 - mb) {
			b.ok = 0;
			break;
		}
		if (!(mbt & MB_INTRA))
			pred_y = pred_cb = pred_cr = 128;

		memset(a->blocks, 0, sizeof a->blocks);
		for (i = 0; i < 6; i++) {
			int dc = 0, ok;
			if (!(cbp & (1 << (5 - i))))
				continue;
			if (mbt & MB_INTRA) {
				int size = vlc_get(&b, i < 4 ? &a->dc_luma : &a->dc_chroma);
				int diff = 0;
				if (!b.ok)
					break;
				if (size) {
					int v = (int)bits_get(&b, size);
					diff = (v < (1 << (size - 1))) ? v - ((1 << size) - 1) : v;
				}
				if (i < 4)
					dc = (pred_y += diff);
				else if (i == 4)
					dc = (pred_cb += diff);
				else
					dc = (pred_cr += diff);
			}
			ok = decode_block(a, &b, a->blocks[i], (mbt & MB_INTRA) != 0, dc, qscale,
					  (mbt & MB_INTRA) ? a->intra_q : a->inter_q);
			if (!ok)
				break;
		}
		if (!b.ok)
			break;

		vfx = full_pel_f ? fx * 2 : fx;
		vfy = full_pel_f ? fy * 2 : fy;
		vbx = full_pel_b ? bx * 2 : bx;
		vby = full_pel_b ? by * 2 : by;

		/* ★сначала в кадр ложится остаток, потом на него прибавляется предсказание */
		put_blocks(a, cur, mb);
		if (!(mbt & MB_INTRA))
			compensate(a, cur, mb, mbt, vfx, vfy, vbx, vby);

		if (skip) {
			pred_y = pred_cb = pred_cr = 128;
			if (kind == 'P') {
				/* пропуск в P: копия из опорной картинки, векторы сброшены */
				fx = fy = bx = by = 0;
				for (k = 0; k < skip; k++)
					copy_mb(a, cur, a->fwd, mb + 1 + k);
			} else {
				/* ★пропуск в B: то же предсказание, что у текущего макроблока,
				 * остаток нулевой; векторы НЕ сбрасываются */
				memset(a->blocks, 0, sizeof a->blocks);
				for (k = 0; k < skip; k++) {
					put_blocks(a, cur, mb + 1 + k);
					if (!(mbt & MB_INTRA))
						compensate(a, cur, mb + 1 + k, mbt,
							   vfx, vfy, vbx, vby);
				}
			}
		}
		mb += 1 + skip;
	}

	if (!b.ok) {
		fail(a, "битый поток: макроблок %d из %d, бит %zu из %zu",
		     mb, a->mb_count, b.pos, n * 8);
		return 0;
	}
	return 1;
}

/* ------------------------------------------------------------------ контейнер */

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)(p[0] | p[1] << 8);
}

static int frame_alloc(struct apeg *a, struct frame *f)
{
	size_t ny = (size_t)a->pw * a->ph;
	size_t nc = ny / 4;
	f->plane = calloc(ny + 2 * nc, sizeof(int16_t));
	if (!f->plane)
		return 0;
	f->y = f->plane;
	f->cb = f->plane + ny;
	f->cr = f->plane + ny + nc;
	return 1;
}

struct apeg *apeg_open(const uint8_t *data, size_t size)
{
	struct apeg *a;
	int i;

	if (!data || size < 0x98 || memcmp(data, "APEG", 4))
		return NULL;
	a = calloc(1, sizeof *a);
	if (!a)
		return NULL;
	a->data = data;
	a->size = size;
	a->version = (int)rd32(data + 8);
	if (a->version != 3 && a->version != 5) {
		free(a);
		return NULL;
	}
	a->width = rd16(data + 0xc);
	a->height = rd16(data + 0xe);
	/* Заведомо негодные размеры отсекаем здесь: дальше идут умножения на размер
	 * плоскости, и на 32-битной сборке они могут завернуться. */
	if (a->width > 8192 || a->height > 8192) {
		free(a);
		return NULL;
	}
	memcpy(a->intra_q, data + 0x10, 64);
	memcpy(a->inter_q, data + 0x50, 64);
	a->duration_ms = rd32(data + 0x90);
	a->n_index = rd32(data + 0x94);
	a->index = data + 0x98;
	/* сравнение ДО умножения: иначе на 32-битном size_t произведение заворачивается
	 * и негодный индекс проходит проверку, а apeg_seek читает мимо буфера */
	if (a->n_index > (size - 0x98) / 8) {
		free(a);
		return NULL;
	}
	a->data_start = 0x98 + 8 * (size_t)a->n_index;
	if (a->width <= 0 || a->height <= 0 || a->data_start > size) {
		free(a);
		return NULL;
	}
	a->mbw = (a->width + 15) / 16;
	a->mbh = (a->height + 15) / 16;
	a->mb_count = a->mbw * a->mbh;
	a->pw = a->mbw * 16;
	a->ph = a->mbh * 16;
	a->pos = a->data_start;

	if (!vlc_build(&a->coeff, apeg_coeff, sizeof apeg_coeff / sizeof *apeg_coeff, APEG_COEFF_BITS) ||
	    !vlc_build(&a->mbtype_p, apeg_mbtype_p, sizeof apeg_mbtype_p / sizeof *apeg_mbtype_p, APEG_MBTYPE_P_BITS) ||
	    !vlc_build(&a->mbtype_b, apeg_mbtype_b, sizeof apeg_mbtype_b / sizeof *apeg_mbtype_b, APEG_MBTYPE_B_BITS) ||
	    !vlc_build(&a->cbp, apeg_cbp, sizeof apeg_cbp / sizeof *apeg_cbp, APEG_CBP_BITS) ||
	    !vlc_build(&a->mba, apeg_mba, sizeof apeg_mba / sizeof *apeg_mba, APEG_MBA_BITS) ||
	    !vlc_build(&a->mv, apeg_mv, sizeof apeg_mv / sizeof *apeg_mv, APEG_MV_BITS) ||
	    !vlc_build(&a->dc_luma, apeg_dc_luma, sizeof apeg_dc_luma / sizeof *apeg_dc_luma, APEG_DC_LUMA_BITS) ||
	    !vlc_build(&a->dc_chroma, apeg_dc_chroma, sizeof apeg_dc_chroma / sizeof *apeg_dc_chroma, APEG_DC_CHROMA_BITS)) {
		apeg_close(a);
		return NULL;
	}
	for (i = 0; i < APEG_NFRAMES; i++) {
		if (!frame_alloc(a, &a->frames[i])) {
			apeg_close(a);
			return NULL;
		}
	}
	return a;
}

void apeg_close(struct apeg *a)
{
	int i;
	if (!a)
		return;
	free(a->coeff.lut);
	free(a->mbtype_p.lut);
	free(a->mbtype_b.lut);
	free(a->cbp.lut);
	free(a->mba.lut);
	free(a->mv.lut);
	free(a->dc_luma.lut);
	free(a->dc_chroma.lut);
	for (i = 0; i < APEG_NFRAMES; i++)
		free(a->frames[i].plane);
	free(a);
}

const char *apeg_error(struct apeg *a) { return a ? a->err : "нет декодера"; }
int apeg_width(struct apeg *a) { return a->width; }
int apeg_height(struct apeg *a) { return a->height; }
uint32_t apeg_duration_ms(struct apeg *a) { return a->duration_ms; }

/* Найти чанк по тегу, не сдвигая текущую позицию разбора. */
static const uint8_t *find_chunk(struct apeg *a, const char *tag, size_t *out_size)
{
	size_t off = a->data_start;
	while (off + 8 <= a->size) {
		uint32_t sz = rd32(a->data + off + 4);
		if (off + 8 + sz > a->size)
			break;
		if (!memcmp(a->data + off, tag, 4)) {
			*out_size = sz;
			return a->data + off + 8;
		}
		/* картинки идут после служебных чанков — дальше искать незачем */
		if (!memcmp(a->data + off, "IPIC", 4))
			break;
		off += 8 + sz;
	}
	return NULL;
}

const uint8_t *apeg_sound(struct apeg *a, size_t *size)
{
	if (!a->sound)
		a->sound = find_chunk(a, "SOND", &a->sound_size);
	*size = a->sound_size;
	return a->sound;
}

const uint8_t *apeg_thumbnail(struct apeg *a, size_t *size, int *width, int *height)
{
	if (!a->thumb) {
		size_t sz;
		const uint8_t *p = find_chunk(a, "TMNL", &sz);
		if (!p || sz < 4)
			return NULL;
		a->thumb_w = rd16(p);
		a->thumb_h = rd16(p + 2);
		a->thumb = p + 4;
		a->thumb_size = sz - 4;
	}
	*size = a->thumb_size;
	*width = a->thumb_w;
	*height = a->thumb_h;
	return a->thumb;
}

static struct frame *free_frame(struct apeg *a)
{
	int i;
	for (i = 0; i < APEG_NFRAMES; i++) {
		struct frame *f = &a->frames[i];
		if (f != a->fwd && f != a->bwd && f != a->last_out)
			return f;
	}
	return &a->frames[0];   /* недостижимо: буферов на один больше, чем занятых */
}

static const struct apeg_frame *publish(struct apeg *a, struct frame *f)
{
	a->last_out = f;
	a->out.width = a->width;
	a->out.height = a->height;
	a->out.y_stride = a->pw;
	a->out.c_stride = a->pw / 2;
	a->out.y = f->y;
	a->out.cb = f->cb;
	a->out.cr = f->cr;
	a->out.pts_ms = f->pts;
	return &a->out;
}

const struct apeg_frame *apeg_next_frame(struct apeg *a)
{
	if (!a)
		return NULL;
	for (;;) {
		const uint8_t *tag, *payload;
		uint32_t sz;
		int kind;
		struct frame *cur;

		if (a->pos + 8 > a->size || a->eof) {
			/* конец потока: остался неотданным последний опорный кадр */
			struct frame *f = a->bwd;
			a->eof = 1;
			if (f) {
				a->bwd = NULL;
				return publish(a, f);
			}
			return NULL;
		}
		tag = a->data + a->pos;
		sz = rd32(tag + 4);
		if (a->pos + 8 + sz > a->size) {
			a->eof = 1;
			continue;
		}
		payload = tag + 8;
		a->pos += 8 + sz;

		if (!memcmp(tag, "IPIC", 4))
			kind = 'I';
		else if (!memcmp(tag, "PPIC", 4))
			kind = 'P';
		else if (!memcmp(tag, "BPIC", 4))
			kind = 'B';
		else
			continue;   /* TMNL, SOND и всё незнакомое */

		if (sz < 4) {
			fail(a, "картинка %c короче метки времени", kind);
			return NULL;
		}
		/* после перемотки ждём ближайшую ключевую картинку */
		if (a->skip_to_key) {
			if (kind != 'I')
				continue;
			a->skip_to_key = 0;
		}
		if (kind != 'I' && !a->bwd)
			continue;               /* опоры ещё нет — показывать нечего */
		if (kind == 'B' && !a->fwd)
			continue;

		cur = free_frame(a);
		if (kind == 'P' || kind == 'I') {
			/* ★ротация опор: назадная становится вперёдной и уходит на показ,
			 * а новая картинка занимает место назадной. У P сдвиг обязан
			 * произойти ДО разбора — она предсказывает уже из новой вперёдной. */
			struct frame *show = a->bwd;
			if (kind == 'P')
				a->fwd = a->bwd;
			if (!decode_picture(a, kind, payload, sz, cur))
				return NULL;
			a->fwd = show;
			a->bwd = cur;
			if (show)
				return publish(a, show);
			continue;
		}
		if (!decode_picture(a, kind, payload, sz, cur))
			return NULL;
		return publish(a, cur);
	}
}

uint32_t apeg_seek(struct apeg *a, uint32_t ms)
{
	uint32_t i, best_ts = 0;
	size_t best = a->data_start;

	for (i = 0; i < a->n_index; i++) {
		uint32_t ts = rd32(a->index + 8 * i);
		uint32_t off = rd32(a->index + 8 * i + 4);
		if (ts > ms)
			break;
		best_ts = ts;
		best = off;
	}
	a->pos = best;
	a->fwd = a->bwd = NULL;
	a->last_out = NULL;
	a->eof = 0;
	a->skip_to_key = 1;
	a->err[0] = '\0';
	return best_ts;
}
