/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
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

#ifndef SYSTEM4_PARTS_INTERNAL_H
#define SYSTEM4_PARTS_INTERNAL_H

#include <cglm/types.h>
#include "gfx/gfx.h"
#include "gfx/font.h"
#include "queue.h"
#include "scene.h"
#include "swf.h"

typedef struct cJSON cJSON;
struct string;
struct hash_table;

// NOTE: actual value is +1
enum parts_state_type {
	PARTS_STATE_DEFAULT = 0,
	PARTS_STATE_HOVERED = 1,
	PARTS_STATE_CLICKED = 2,
#define PARTS_NR_STATES 3
};

enum parts_motion_type {
	PARTS_MOTION_POS,
	PARTS_MOTION_ALPHA,
	PARTS_MOTION_CG,
	PARTS_MOTION_HGAUGE_RATE,
	PARTS_MOTION_VGAUGE_RATE,
	PARTS_MOTION_NUMERAL_NUMBER,
	PARTS_MOTION_MAG_X,
	PARTS_MOTION_MAG_Y,
	PARTS_MOTION_ROTATE_X,
	PARTS_MOTION_ROTATE_Y,
	PARTS_MOTION_ROTATE_Z,
	PARTS_MOTION_VIBRATION_SIZE
#define PARTS_NR_MOTION_TYPES (PARTS_MOTION_VIBRATION_SIZE+1)
};

union parts_motion_param {
	int i;
	float f;
	struct {
		int x;
		int y;
	};
};

struct parts_motion {
	TAILQ_ENTRY(parts_motion) entry;
	enum parts_motion_type type;
	union parts_motion_param begin;
	union parts_motion_param end;
	int begin_time;
	int end_time;
};

TAILQ_HEAD(parts_motion_list, parts_motion);

struct sound_motion {
	TAILQ_ENTRY(sound_motion) entry;
	int begin_time;
	int sound_no;
	bool played;
};

struct parts_text_char {
	Texture t;
	char ch[4];
	float advance;
	Point off;
};

struct parts_text_line {
	struct parts_text_char *chars;
	int nr_chars;
	unsigned height;
	float width;
};

/*
 * РУБИ (фуригана) — чтение мелким кеглем над прогоном базовых символов. В репликах
 * Haha Ranman это ЕДИНСТВЕННАЯ живая разметка: во всех 34406 сообщениях .ain
 * встречается только `${ruby text=<чтение>}<база>${/ruby}` (482 раза), а формы
 * `${font …}`/`${time …}` лежат лишь ШАБЛОНАМИ в строковой секции — их собирает
 * `CMessageTextModel@AddMessageText` из глобалов `_Ｍフォント`, `_Ｍサイズ`, …,
 * которым игра ни разу не присваивает значение.
 *
 * Чтение хранится СВОИМИ текстурами глифов, а не строкой: у него свой кегль,
 * интервал и обводка (узел `ルビ` раскладки), то есть в общий поток символов строки
 * оно не встаёт. Индекс базы — СКВОЗНОЙ по рисуемым символам части, как у счётчика
 * посимвольного проявления.
 */
struct parts_text_ruby {
	int first_char;
	int nr_chars;
	int nr_glyphs;
	struct parts_text_char *glyphs;
	float width;
};

// То, что подаёт разборщик разметки: чтение над прогоном базовых символов.
struct parts_ruby_spec {
	int first_char;
	int nr_chars;
	struct string *text;
};

enum parts_type {
	PARTS_UNINITIALIZED,
	PARTS_CG,
	PARTS_TEXT,
	PARTS_ANIMATION,
	PARTS_NUMERAL,
	PARTS_HGAUGE,
	PARTS_VGAUGE,
	PARTS_CONSTRUCTION_PROCESS,
	PARTS_FLASH,
	PARTS_FLAT,
	PARTS_MOVIE,
	PARTS_RECT_DETECTION,
	PARTS_LAYOUT_BOX,
	PARTS_3DLAYER,
	// System 4 v14 (Ixseal): сплошной цветной прямоугольник (`パネル`).
	PARTS_PANEL,
	/*
	 * `ＣＧ判定パーツ` (классический id 19) — область попадания, заданная
	 * НЕПРОЗРАЧНЫМИ ПИКСЕЛЯМИ картинки. Родня `PARTS_RECT_DETECTION`: сама
	 * часть НЕ рисуется, но её текстура задаёт форму hit-области. Хранится в
	 * том же `struct parts_cg`, что и обычная картинка (имя + текстура), —
	 * различие только в типе состояния и в том, что рендер её пропускает.
	 */
	PARTS_CG_DETECTION,
#define PARTS_NR_TYPES (PARTS_CG_DETECTION+1)
};

struct parts_common {
	Texture texture;
	int w, h;
	Point origin_offset;
	Rectangle hitbox;
	Rectangle surface_area;
	// Кэш маски попиксельного hit-теста (см. parts->pixel_hittest): 1 байт на
	// пиксель, !=0 = непрозрачный. Строится ЛЕНИВО из текстуры при первом
	// попадании в hitbox и живёт до parts_state_free — читать альфу с GPU каждый
	// кадр нельзя (glReadPixels синхронизирует конвейер).
	uint8_t *hit_mask;
	int hit_mask_w, hit_mask_h;
};

struct parts_cg {
	struct parts_common common;
	int no;
	struct string *name;
};

struct parts_text {
	struct parts_common common;
	unsigned nr_lines;
	struct parts_text_line *lines;
	int line_space;
	struct { float x; int y; } cursor;
	struct text_style ts;
	// `Parts_SetTextEnableTag`: разрешена ли РАЗМЕТКА в тексте этой части. Игра
	// ставит флаг и читает обратно (`Parts_IsTextEnableTag`), поэтому хранить его
	// обязательно. Сам разбор тегов движком пока не делается — см. PE_SetTextEnableTag.
	bool enable_tag;
	/*
	 * ПОСИМВОЛЬНОЕ ПРОЯВЛЕНИЕ (`字速度` окна реплик, см. message_window.c).
	 * Отрисовка обрезается по счётчику, а не перекладывается текст: у части и так
	 * каждый символ — своя текстура, поэтому «показать первые N» ничего не
	 * перерисовывает. Счёт идёт по РИСУЕМЫМ символам в порядке отрисовки
	 * (переводы строки в него не входят — они не глифы).
	 *
	 * `reveal_active` отдельным флагом, а не «-1 = всё видно» в счётчике: часть
	 * приходит из xcalloc нулями, и любое значение-в-счётчике по умолчанию
	 * означало бы «спрятать текст» у ВСЕХ обычных текстовых частей.
	 */
	bool reveal_active;
	int reveal_shown;         // сколько символов нарисовано целиком
	float reveal_head_alpha;  // альфа символа под номером reveal_shown (0…1)
	// Руби: свой стиль (узел `ルビ` раскладки) и прогоны чтения.
	struct text_style ruby_ts;
	int ruby_line_space;
	struct parts_text_ruby *ruby;
	int nr_ruby;
};

struct parts_animation {
	struct parts_common common;
	struct string *cg_name;
	unsigned start_no;
	unsigned nr_frames;
	Texture *frames;
	unsigned frame_time;
	unsigned elapsed;
	unsigned current_frame;
};

enum parts_numeral_font_type {
	// each digit has a separate CG (only first CG number stored in `cg_no` member)
	PARTS_NUMERAL_FONT_SEPARATE = 0,
	// digits packed into single CG
	PARTS_NUMERAL_FONT_COMBINED = 1,
	// each digit has a separate CG (CG numbers stored in `width` member)
	PARTS_NUMERAL_FONT_SEPARATE2 = 2,
};

struct parts_numeral_font {
	enum parts_numeral_font_type type;
	int cg_no;
	int width[12];
	Texture cg[12];
};

struct parts_numeral {
	struct parts_common common;
	bool have_num;
	int num;
	int space;
	int show_comma;
	int length;
	int font_no;
	/*
	 * Второй способ рисовать число — ШРИФТОМ, а не набором CG (`表示タイプ = 2`
	 * в раскладке, `ＣＧ名` при этом пуст, а рядом лежат `フォントタイプ/
	 * フォントサイズ/フォント色/フォント縁取り*`). Так у Dohna сделаны все
	 * счётчики интерфейса — «TALENT 3/3», «Client 1/4» и т.п.; в CG-варианте
	 * (Tsumamigui 3) эти поля не используются.
	 */
	bool use_font;
	struct text_style ts;
};

struct parts_gauge {
	struct parts_common common;
	Texture cg;
	int cg_no;
	float rate;
	/*
	 * Числитель и знаменатель хранятся ОТДЕЛЬНО от готового отношения: игра
	 * читает их по-одиночке — `CHGaugeParts@Denominator::set` (@0x32b78c у Dohna)
	 * сначала зовёт `Numerator::get`, чтобы пересчитать заполнение под новый
	 * знаменатель, а `Parts_GetHGaugeNumerator`/`Denominator` зовёт экран
	 * `行動選択` у Haha Ranman. Из одного `rate` их не восстановить.
	 * В сейв частей не пишутся (формат не меняем): при загрузке восстанавливаются
	 * как (rate, 1) — отношение то же.
	 */
	float numerator;
	float denominator;
	/*
	 * `Reverse` (Parts_SetHGaugeReverse/SetVGaugeReverse): шкала заполняется от
	 * ПРОТИВОПОЛОЖНОГО края — горизонтальная справа налево, вертикальная снизу
	 * вверх. Без этого экран Squad у Dohna ронял движок на первой же полосе
	 * (`CHGaugeParts@Reverse::set` ← `PlayerSummaryView@SetParam`), потому что
	 * незнакомая HLL-функция у нас фатальна. В сейв не пишется (формат не
	 * меняем): игра выставляет флаг заново при построении экрана.
	 */
	bool reverse;
};

// Serialized in save data. Do not reorder.
enum parts_cp_op_type {
	PARTS_CP_CREATE,
	PARTS_CP_CREATE_PIXEL_ONLY,
	PARTS_CP_CG,
	PARTS_CP_FILL,
	PARTS_CP_FILL_ALPHA_COLOR,
	PARTS_CP_FILL_AMAP,
	PARTS_CP_DRAW_RECT,
	PARTS_CP_DRAW_CUT_CG,
	PARTS_CP_COPY_CUT_CG,
	PARTS_CP_DRAW_TEXT,
	PARTS_CP_COPY_TEXT,
	PARTS_CP_GRAY_FILTER,
	PARTS_CP_FILL_WITH_ALPHA,
	// ★Новое дописывается ТОЛЬКО В КОНЕЦ: значения уходят числом в сейв
	// (`save_parts_cp_op` пишет `op->type`), вставка в середину переименует
	// все последующие операции в уже записанных сейвах.
	PARTS_CP_FILL_GRADATION_HORIZON,
	PARTS_CP_MUL_FILTER,
	// v14 (Ixseal): векторные фигуры В АЛЬФА-КАРТУ. Из них собраны скруглённые
	// подложки интерфейса Dohna: прямоугольник + четыре сектора по углам
	// (см. «Round128x40» в PlayerShopView.pactex).
	PARTS_CP_FILL_PIE_AMAP,
	// v14: заливка КРУГА/сектора ЦВЕТОМ с альфа-блендом (жёлтая метка
	// следующего узла в данже Dohna).
	PARTS_CP_FILL_PIE_BLEND,
	// v14: заливка МНОГОУГОЛЬНИКА цветом с альфа-блендом (`SetFillPolygonAlphaBlend`,
	// команда 97) — «труба» маршрута между узлами данжа Dohna.
	PARTS_CP_FILL_POLYGON_BLEND,
	// v14 (Ixseal): размытие поверхности по одной оси (команды 27/28 раскладки,
	// `CASConstructionProcess@Set{H,V}BlurFilter`). Игра ставит их парой сразу за
	// `SetCreateCG` — так собран размытый задник экранов (CONFIG, галерея, выбор
	// фазы): CG фона → HBlur → VBlur, сила в поле `ブラー`.
	PARTS_CP_BLUR_FILTER,
	// v14: аддитивная подкраска (команда 16, `SetAddFilter`) — красит только цвет.
	PARTS_CP_ADD_FILTER,
	// v14: ГРАДИЕНТ В АЛЬФА-КАРТУ (команды 25 и 26, вертикальный и горизонтальный).
	// Ими обводят поверхность мягкой каймой: слайд-шоу титула Haha Ranman кладёт
	// четыре полосы по 15 px и получает фотографию, растворяющуюся в бумаге.
	PARTS_CP_FILL_GRADATION_AMAP,
	/*
	 * v14: ЗАМОЩЕНИЕ CG (семейство `CASConstructionProcess::TileCG*`).
	 * 128 `TileCGBlend` — подмешать по альфе: у титула Haha Ranman так кладётся
	 * зерно плёнки `タイトル／フィルム／ノイズ` (32x32, чёрное, альфа 0…22) —
	 * ШАГ ИДЁТ ДО гашения краёв, поэтому зерно попадает и в кайму.
	 */
	PARTS_CP_TILE_CG_BLEND,
	/*
	 * v14: 129 `TileCGCopy` — скопировать плитку ВМЕСТЕ С АЛЬФОЙ: сетка-ромбы на
	 * фоне данжа Dohna (`Grid` в `SceneHitokari.x`: Create 2048x2048 → замостить
	 * `システム／マップ／グリッド`, дальше часть крутится на 330° с альфой 25).
	 * ★Отдельный тип, а не флаг в tile_cg: op-типы сериализуются в parts-блок
	 * сейва (src/parts/save.c), новое ПОЛЕ сломало бы чтение старых сейвов.
	 */
	PARTS_CP_TILE_CG_COPY,
	/*
	 * v14: ОСТАЛЬНЫЕ РЕЖИМЫ ЗАЛИВКИ ФИГУР. Номера команд 88…127 разложены по двум
	 * осям (см. таблицу в docs/FINDINGS.md): фигура (прямоугольник, многоугольник,
	 * круг, эллипс, сектор) и способ записи результата — ровно пятёрка
	 * `Fill / WithAlpha / AMap / Blend / AlphaBlend`, та же, что у прямоугольника
	 * (команды 3/6/5/4 старого набора). Геометрию считает общая маска покрытия,
	 * различие только в том, что делается с пикселем приёмника:
	 *   COLOR      — RGB подменяется, альфа не трогается;
	 *   WITH_ALPHA — RGB и альфа подменяются целиком (так узлы миникарты Dohna
	 *                рисуют белый круг d=24 на обнулённой по альфе поверхности);
	 *   AMAP       — меняется только альфа (уже было: PARTS_CP_FILL_PIE_AMAP);
	 *   COLOR_BLEND— цвет подмешивается по своей альфе, альфа приёмника неизменна;
	 *   ALPHA_BLEND— альфа поднимается по маске, затем цвет (PARTS_CP_FILL_*_BLEND).
	 */
	PARTS_CP_FILL_PIE_COLOR,
	PARTS_CP_FILL_PIE_WITH_ALPHA,
	PARTS_CP_FILL_PIE_COLOR_BLEND,
	PARTS_CP_FILL_POLYGON_COLOR,
	PARTS_CP_FILL_POLYGON_WITH_ALPHA,
	PARTS_CP_FILL_POLYGON_AMAP,
	PARTS_CP_FILL_POLYGON_COLOR_BLEND,
#define PARTS_NR_CP_TYPES (PARTS_CP_FILL_POLYGON_COLOR_BLEND+1)
};

struct parts_cp_create {
	int w;
	int h;
};

struct parts_cp_cg {
	int no;
};

struct parts_cp_fill {
	int x, y, w, h;
	int r, g, b, a;
};

/*
 * Сектор эллипса, заливаемый в АЛЬФА-КАРТУ (`SetFillPieAMap`, команда 122 — самая
 * частая в раскладках Dohna: 258 вхождений). Углы в градусах, 0° — вправо (+X),
 * растут ПО ЧАСОВОЙ стрелке (экранный Y вниз): проверено по четырём угловым
 * секторам «Round128x40», где 180°+90° даёт левый верхний угол, 270°+90° —
 * правый верхний, 0°+90° — правый нижний, 90°+90° — левый нижний.
 */
struct parts_cp_pie {
	int x, y;              // центр
	int rx, ry;            // радиусы
	int start_angle;       // начало дуги, град.
	int sweep_angle;       // длина дуги, град.
	int a;                 // заливаемая альфа
	// Цвет — для ЦВЕТНОЙ заливки фигуры (`SetFillCircleAlphaBlend`, команда
	// 106): та же геометрия, но результат кладётся не в альфа-карту, а
	// подмешивается цветом (у альфа-варианта поля просто не используются).
	int r, g, b;
};

struct parts_cp_polygon {
	int *pts;              // плоский список координат: x0,y0,x1,y1,…
	int nr_pts;            // число ТОЧЕК (координат вдвое больше)
	int r, g, b, a;
	int round_corner;      // скругление углов (поле раскладки RoundCorner)
	int angle;             // поле раскладки Angle
};

struct parts_cp_cut_cg {
	int cg_no;
	int dx, dy, dw, dh;
	int sx, sy, sw, sh;
	int interp_type;
};

struct parts_cp_text {
	struct string *text;
	int x, y;
	int line_space;
	struct text_style style;
};

struct parts_cp_filter {
	int x, y, w, h;
	bool full_size;
};

// Размытие по одной оси: `radius` — поле `ブラー` раскладки (у Dohna всегда 10),
// `vertical` различает команды 27 (по X) и 28 (по Y).
struct parts_cp_blur {
	int x, y, w, h;
	bool full_size;
	int radius;
	bool vertical;
};

// `Ｐ＿構築手順＿グラデーション横` — заливка прямоугольника градиентом СВЕРХУ ВНИЗ
// (полосы горизонтальные, цвет меняется по вертикали от верхнего к нижнему).
struct parts_cp_fill_gradation {
	int x, y, w, h;
	int top_r, top_g, top_b;
	int bot_r, bot_g, bot_b;
	// Прямоугольник не задан — заливать ВЕСЬ холст (как full_size у
	// parts_cp_color_filter). Так приходит из раскладки: там у команды 9
	// `元矩形`/`先矩形` нулевые, а цвета заданы (см. act_construction_run).
	bool full_size;
};

// Фильтр умножения: как gray_filter, но с цветом.
struct parts_cp_color_filter {
	int x, y, w, h;
	int r, g, b;
	bool full_size;
};

struct parts_cp_gradation_amap {
	int x, y, w, h;
	int a1, a2;      // альфа в начале и в конце полосы
	bool vertical;   // true — вдоль Y (команда 25), false — вдоль X (команда 26)
};

struct parts_cp_tile_cg {
	int cg_no;
	int x, y, w, h;  // область замощения; нули = вся поверхность (см. cp_fill_rect)
};

struct parts_cp_op {
	TAILQ_ENTRY(parts_cp_op) entry;
	enum parts_cp_op_type type;
	union {
		struct parts_cp_create create;
		struct parts_cp_cg cg;
		struct parts_cp_fill fill;
		struct parts_cp_pie pie;
		struct parts_cp_polygon polygon;
		struct parts_cp_cut_cg cut_cg;
		struct parts_cp_text text;
		struct parts_cp_filter filter;
		struct parts_cp_blur blur;
		struct parts_cp_fill_gradation gradation;
		struct parts_cp_color_filter color_filter;
		struct parts_cp_gradation_amap gradation_amap;
		struct parts_cp_tile_cg tile_cg;
	};
};

/*
 * Сырьё одной операции построения — ровно то, что игра подала в
 * `AddPartsConstructionProcess` (40 int / 2 float / 2 string / список точек).
 * Хранится, чтобы `GetPartsConstructionProcess` мог отдать процедуру ОБРАТНО:
 * восстанавливать эти массивы из уже разобранных `parts_cp_op` пришлось бы
 * обратным преобразованием на каждую из 25+ команд, а сохранённое сырьё делает
 * чтение тождественным записи.
 */
struct parts_cp_raw {
	int nr_ints;
	int ints[40];
	int nr_floats;
	float floats[2];
	int nr_strings;
	struct string *strings[2];
	int nr_pos;
	int *pos;
};

struct parts_construction_process {
	struct parts_common common;
	TAILQ_HEAD(, parts_cp_op) ops;
	// Сырьё операций в порядке добавления (см. struct parts_cp_raw).
	struct parts_cp_raw *raw;
	int nr_raw;
};

enum parts_flash_blend_mode {
	PARTS_FLASH_BLEND_NORMAL0    = 0,
	PARTS_FLASH_BLEND_NORMAL1    = 1,
	PARTS_FLASH_BLEND_LAYER      = 2,
	PARTS_FLASH_BLEND_MULTIPLY   = 3,
	PARTS_FLASH_BLEND_SCREEN     = 4,
	PARTS_FLASH_BLEND_LIGHTEN    = 5,
	PARTS_FLASH_BLEND_DARKEN     = 6,
	PARTS_FLASH_BLEND_DIFFERENCE = 7,
	PARTS_FLASH_BLEND_ADD        = 8,
	PARTS_FLASH_BLEND_SUBTRACT   = 9,
	PARTS_FLASH_BLEND_INVERT     = 10,
	PARTS_FLASH_BLEND_ALPHA      = 11,
	PARTS_FLASH_BLEND_ERASE      = 12,
	PARTS_FLASH_BLEND_OVERLAY    = 13,
	PARTS_FLASH_BLEND_HARDLIGHT  = 14,
};

enum parts_draw_filter {
	PARTS_DRAW_FILTER_NORMAL   = 0,
	PARTS_DRAW_FILTER_ADDITIVE = 1,
	PARTS_DRAW_FILTER_MULTIPLY = 2,
	PARTS_DRAW_FILTER_SCREEN   = 3,
};

struct parts_flash_object {
	TAILQ_ENTRY(parts_flash_object) entry;
	uint16_t depth;
	uint16_t character_id;
	mat4 matrix;
	struct swf_cxform_with_alpha color_transform;
	enum parts_flash_blend_mode blend_mode;
};

struct parts_flash {
	struct parts_common common;
	struct string *name;
	struct swf *swf;

	struct swf_tag *tag;
	bool has_ended;
	bool stopped;
	unsigned elapsed;
	int current_frame;
	struct hash_table *dictionary;
	struct hash_table *bitmaps;  // bitmap character id -> struct texture *
	struct hash_table *sprites;  // sprite character id -> struct parts_flash_object *
	TAILQ_HEAD(, parts_flash_object) display_list;
};

struct flat_layer_state {
	int current_frame;
	bool stopped;
	bool suppress_advance;
	int jump_target;  // pending jump frame, -1 = none
	// Per-timeline: last matched script key index for change detection
	// (-2 = uninitialized, -1 = no match, >= 0 = key index)
	int *last_script_keys;
	// Per-timeline: child layer state (for TIMELINE libs), NULL otherwise
	struct flat_layer_state **children;
	size_t nr_timelines;
};

// Per-frame CG list for a STOP_MOTION library. lib_indices[k] is
// the library index of the CG to display at frame k.
struct flat_stop_motion_frames {
	int *lib_indices;
	int count;
};

/*
 * ВИРТУАЛЬНАЯ CG-библиотека из секции TALT: у части флэтов LIBL пуст, а слои
 * таймлайна просят картинки по имени (`AUTO.png`, `緑丸.png`) — они лежат
 * ПОДКАРТИНКАМИ в одном атласе записи TALT. Такую библиотеку описывает пара
 * «текстура атласа + прямоугольник в ней».
 */
struct flat_atlas_lib {
	const char *name;   // из struct talt_metadata (владелец — struct flat)
	int tex;            // индекс в parts_flat.talt_textures
	Rectangle rect;     // подпрямоугольник в атласе
};

struct parts_flat {
	struct parts_common common;
	struct string *name;
	struct flat *flat;
	bool stopped;
	bool needs_advance;
	unsigned elapsed;
	int end_frame;
	int pending_seek_delta;
	struct flat_layer_state *root_state;
	size_t nr_libraries;
	Texture *textures;  // indexed by library index (only CG libs have valid textures)
	// Indexed by library index. Only entries for STOP_MOTION libraries have lib_indices populated.
	struct flat_stop_motion_frames *stop_motion_frames;
	// Атласы секции TALT (по записи) и собранные из них библиотеки-подкартинки.
	Texture *talt_textures;
	size_t nr_talt_textures;
	struct flat_atlas_lib *atlas_libs;
	size_t nr_atlas_libs;
};

struct parts_movie {
	struct parts_common common;
	int sprite_no;  // SACT sprite number used as movie render target
	// Новое API (CreatePartsMovie/PlayPartsMovie): ролик APEG принадлежит самой
	// части, кадр кладётся в её текстуру. У старого API (InitPartsMovie) поле
	// пустое, а целью служит спрайт SACT выше.
	struct apeg_movie *apeg;
};

enum parts_layout_type {
	PARTS_LAYOUT_FREE       = 0,  // no automatic layout
	PARTS_LAYOUT_VERTICAL   = 1,
	PARTS_LAYOUT_HORIZONTAL = 2,
};

// In AliceSoft's PartsEngine implementation, LayoutBox is a component type
// that is NOT per-state. We store it per-state for uniformity with other
// component types, but parts_get_layout_box() and all code in layoutbox.c
// operate on states[0] only. This is safe as long as game scripts never
// convert a LayoutBox parts to/from another component type.
struct parts_layout_box {
	struct parts_common common;
	enum parts_layout_type layout_type;
	bool wrap;
	int wrap_size;
	int align;
	int padding_top;
	int padding_bottom;
	int padding_left;
	int padding_right;
};

// Панель (Ixseal): размер + сплошной цвет; альфа-градиенты по краям хранятся,
// но не рисуются — см. src/parts/panel.c.
struct parts_panel {
	struct parts_common common;
	int w, h;
	SDL_Color color;
	int grad_top, grad_bottom, grad_left, grad_right;
};

struct parts_3dlayer {
	struct parts_common common;
	int plugin;    // ReignEngine plugin handle (-1 = none)
	int sprite_no; // SACT sprite used as render target
};

struct parts_state {
	enum parts_type type;
	union {
		struct parts_common common;
		struct parts_cg cg;
		struct parts_text text;
		struct parts_animation anim;
		struct parts_numeral num;
		struct parts_gauge gauge;
		struct parts_construction_process cproc;
		struct parts_flash flash;
		struct parts_flat flat;
		struct parts_movie movie;
		struct parts_layout_box layout_box;
		struct parts_3dlayer layer3d;
		struct parts_panel panel;
	};
};

TAILQ_HEAD(parts_list, parts);

struct parts_params {
	int z;
	Point pos;
	bool show;
	uint8_t alpha;
	struct { float x, y; } scale;
	struct { float x, y, z; } rotation;
	SDL_Color add_color;
	SDL_Color multiply_color;
	/*
	 * ★СИСТЕМА КООРДИНАТ поддерева, ЗЕРКАЛЬНАЯ ли она (только в `global`):
	 * накопленный XOR флагов `reverse_lr`/`reverse_tb` СТРОГИХ предков. Своего
	 * флага части здесь НЕТ — он складывается с этим уже в рендере.
	 *
	 * Зачем: `SetComponentReverseLR` игра ставит и на КОНТЕЙНЕР без картинки
	 * (у Dohna — рамка кадра бойца, `wh=0x0`, `tex=0`), и «зеркалить свой квад»
	 * для него не значит ничего. Разворот контейнера — это зеркало ВСЕГО
	 * ПОДДЕРЕВА: смещения потомков меняют знак, а их содержимое переворачивается.
	 */
	bool mirror_x, mirror_y;
};

struct parts_message_window;

struct parts {
	struct sprite sp;
	enum parts_state_type state;
	struct parts_state states[PARTS_NR_STATES];
	TAILQ_ENTRY(parts) parts_list_entry;
	TAILQ_ENTRY(parts) child_list_entry;
	TAILQ_ENTRY(parts) dirty_list_entry;
	struct parts_list children;
	struct parts_params local;
	struct parts_params global;
	struct parts *parent;
	int dirty;
	int no;
	int delegate_index;
	// `uniqueID` из SetEventID: идентификатор набора обработчиков, который игровой
	// CPartsMessageManager сверяет с сообщением (см. PE_SetEventID). Дефолт -1.
	int event_unique_id;
	int sprite_deform;
	// `SetComponentReverseLR/TB` (v14): НЕЗАВИСИМЫЕ флаги зеркалирования части по
	// горизонтали и вертикали. Отдельны от `sprite_deform` (0/1/2), потому что тот
	// одним числом обе оси выразить не может, а игра ставит их порознь
	// (`parts::detail::CParts@ReverseLR::set` ← `CSpriteParts` ← `AdvStandImage`),
	// и у обоих есть геттер. В рендере складываются с `sprite_deform`.
	bool reverse_lr;
	bool reverse_tb;
	/*
	 * Право на клик — ТРИ состояния, а не два: 0 — игра ничего не говорила,
	 * 1 — разрешила явно, −1 — ЗАПРЕТИЛА явно (`SetClickable(false)`, а через
	 * него и `SetButtonEnable(false)`). Двух значений не хватало: дефолт от
	 * `xcalloc` — ноль, и «выключено игрой» было неотличимо от «не задано», а
	 * новые игры `SetClickable` почти не зовут, поэтому клик разрешался по
	 * `!pass_cursor` (см. parts_update_mouse) и запрет молча терялся.
	 */
	int8_t clickable;
	// True for activity "button" parts (パーツタイプ=0) created by ReadActivityFile.
	// Such parts render as CG (hover/click state-switch) but must report component
	// type 0 to the game, which enables click handlers only on type-0 parts.
	bool is_button;
	// Часть, чьё состояние — `構築パーツ` (construction part): её содержимое
	// строит «процедура построения» (список операций из раскладки), которую
	// движок не выполняет. Загрузчик кладёт вместо результата НЕПРОЗРАЧНУЮ
	// заливку прямоугольника процедуры — она годится только как маска
	// альфа-клиппера (прямоугольного отсечения) для соседей, но не как то, что
	// видно на экране. Флаг убирает такую часть из отрисовки, оставляя текстуру
	// доступной клипперу (он читает её напрямую, независимо от show).
	bool construction_mask;
	// `マウスカーソルピクセル判定` из раскладки: курсор попадает в часть только там, где
	// её текстура НЕ прозрачна, а не по всему прямоугольнику. Без этого перекрывающиеся
	// части воруют клик друг у друга: меню титула Dohna — восемь ДИАГОНАЛЬНЫХ полос,
	// каждая лежит в текстуре 704x236 при своей видимой высоте ~91, и соседние боксы
	// перекрываются вчетверо, так что клик по «Start Game» доставался «Load Game»
	// (та выше по z). Гейт структурный и узкий: у Dohna флаг стоит у 13 частей из 2708
	// по всем 195 раскладкам (8 кнопок титула, 4 в SceneHome, 1 в DungeonSelector),
	// у Tsumamigui 3 — у 0 из 629, т.е. старые игры этим путём не идут вовсе.
	bool pixel_hittest;
	// Widget state for config-style screens (scrollbars/sliders and checkboxes).
	// Not yet rendered as interactive widgets, but the game reads these back, so
	// we store what it sets to keep the config UI logic consistent.
	float hscroll_rate;
	bool checkbox_checked;
	int checkbox_r, checkbox_g, checkbox_b;
	// Horizontal scrollbar (config slider, パーツタイプ=3). The part's CG is the
	// draggable knob; the track is a separate frame CG. Geometry comes from the
	// .pactex 種類別情報. The knob slides between sb_base_x .. sb_base_x+(len-knob_w).
	bool is_hscrollbar;
	int sb_length, sb_width;   // track length (x) and width (y)
	int sb_total, sb_view;     // scroll amounts (for pos<->rate conversion)
	int sb_base_x, sb_base_y;  // knob home position (track origin, from 座標)
	// Vertical scrollbar (backlog/backscene, パーツタイプ=2). The part's CG is the
	// draggable knob (＜base＞／バー／通常|オン|ダウン); geometry from .pactex 種類別情報.
	// Track runs top->bottom; up/down arrow buttons (前サイズ/次サイズ) reserve space
	// at the ends. rate is driven by the game via SetVScrollbarScrollPos (pos/total/view).
	bool is_vscrollbar;
	/*
	 * Полоса — СЛАЙДЕР (`横スライダーバー` = 13, `縦スライダーバー` = 12 в нумерации
	 * v14), а не полоса прокрутки (3/2). Механика та же (жёлоб, бегунок, доля), а
	 * различается ВИД КОМПОНЕНТА, который часть обязана отдавать игре: обёртку
	 * слайдера `CActivityWrap@GetHSliderBar` игра берёт через `CompParts(имя, 13, 1)`
	 * и при несовпадении получает null. Поэтому это отдельный признак, а не «алиас
	 * 13 → 3»: у слайдера и поля раскладки названы своими именами
	 * (`全体スライダー量`, `スライダーレート`).
	 */
	bool is_slider_bar;
	int sb_up_size, sb_down_size;  // heights of the ∧/∨ arrow buttons (前サイズ/次サイズ)
	// На сколько сдвигается позиция за одно нажатие кнопки-стрелки (.pactex
	// `ボタンクリック移動量`, у бэк-сцены = 1). Именно этим листаются сцены: обработчики
	// `CBackSceneView@PressNextButton/PressPrevButton` берут шаг через
	// GetVScrollbarMoveSizeByButton и прибавляют его к позиции. Пока геттер был
	// заглушкой-нулём, кнопки 前へ/次へ не делали НИЧЕГО (колесо работало — оно идёт
	// другим путём).
	int sb_move_by_button;
	float vscroll_rate;
	// Checkbox (パーツタイプ=1). The box CG has checked/unchecked variants
	// ("<base>[／チェック]／通常|オン|ダウン"); clicking toggles checkbox_checked.
	bool is_checkbox;
	struct string *checkbox_cg_base;
	/*
	 * ДОСТУПНОСТЬ чекбокса (`有効状態` в раскладке, `SetCheckBoxEnable` в рантайме).
	 * Выключенный не переключается кликом и рисуется отдельным файлом `／無効`
	 * (у Dohna есть и `／チェック／無効` — серая рамка с галочкой).
	 * Живой случай: System Menu в ADV разрешает ровно ЧЕТЫРЕ ярлыка, и когда их
	 * набрано четыре, `SceneAdvButtonMenu@UpdateCheckable` гасит остальные строки.
	 * Пока обе функции были заглушками, недоступных не было вовсе и отметить
	 * можно было сколько угодно.
	 */
	bool checkbox_enabled;
	/*
	 * `テキストボックス` (パーツタイプ=4) — ПОЛЕ ВВОДА. Само состояние поля (текст,
	 * шрифт, фокус) движок ведёт отдельной таблицей в src/hll/PartsEngine.c, но ВИД
	 * компонента обязан отдавать сам парт: игра берёт поле обёрткой
	 * `CActivityWrap@GetTextBox`, а та гейтится `CompParts(имя, 4, состояние)` и при
	 * несовпадении возвращает null — молча, без единого предупреждения.
	 * Так пропадала заметка к сейву: `SaveLoadConfirmScene` на «Yes» читает текст
	 * поля `テキストボックス：コメント編集`, а при null подставляет ПУСТУЮ строку —
	 * в сайдкаре `.vsf` поле `コメント` (индекс 8) оставалось пустым при заполненном
	 * `ゲーム時間文字列`. Замер: вид части 90000183 был 26/19, а нужен 4.
	 */
	bool is_textbox;
	/*
	 * `ラジオボタンボックス` (パーツタイプ=9) — ГРУППА радиокнопок. Своего вида у неё нет:
	 * в раскладке это пустая часть-СОСЕД рядом с самими кнопками (те — обычные чекбоксы,
	 * `パーツタイプ=1`), все под общим layout-боксом. Движку она нужна ровно затем, чтобы
	 * отдавать СВОЙ вид компонента: игра берёт группу обёрткой
	 * `CActivityWrap@GetRadioButtonBox` (гейт `CompParts(имя, 9, состояние)`) и вешает на
	 * неё `ChangedEvent`. Без этого обёртка пустая, обработчик не регистрируется — и
	 * выбор в группе не применяется вовсе (у Haha Ranman это `表示モード`,
	 * `フルスクリーン時の画面比率` и `ゲーム画面の拡大方法` на вкладке `ウィンドウ`).
	 */
	bool is_radio_box;
	/*
	 * Состав группы задаёт САМА РАСКЛАДКА — список `チェックボックスリスト` в
	 * `種類別情報` части-группы, ИМЕНАМИ кнопок (у Haha Ranman по две на группу:
	 * `SYS_拡大方法標準` / `SYS_拡大方法バイキュービック` и т.п.). Имена загрузчик
	 * разрешает в номера отложенно — кнопки могут быть построены позже группы.
	 * ★По дереву состав НЕ вывести: группа и кнопки — БРАТЬЯ под общим layout-боксом,
	 * и «все чекбоксы под тем же родителем» — правило заведомо неверное: у Dohna на
	 * экране Controls в одном контейнере с парой `Mouse Wheel` лежат ещё четыре
	 * независимых чекбокса `Confirmations`, и выбор в паре гасил их все.
	 */
	int *radio_children;
	int nr_radio_children;
	/*
	 * `ユーザコンポーネント` (тип компонента v14 = 17) — часть-место, куда игровой
	 * фреймворк подставляет ОТДЕЛЬНУЮ активность (шапка, футер, полоса фазы…).
	 * Своего рендера у неё нет: содержимое создаёт сама игра и вешает потомками.
	 * Движку нужно хранить ровно то, что у части спрашивают:
	 *   `ユーザコンポーネント名` → Get/SetUserComponentName (имя класса компонента),
	 *   `データ` (плоский список «ключ, значение») → Get/SetUserComponentData.
	 * Обе пары есть в библиотеке ЦЕЛИКОМ (сеттер+геттер), поэтому свойства
	 * обязаны храниться по-настоящему. У Tsumamigui 3 этих функций нет вовсе.
	 */
	bool is_user_component;
	/*
	 * КОРЕНЬ АКТИВНОСТИ (`ルートパーツ` раскладки): часть, с которой загрузчик
	 * начинает дерево, у неё нет родителя-части внутри своей активности.
	 * Нужен ровно для точки привязки: `原点座標モード` у корня ни в одной из 195
	 * раскладок Dohna не задан (всегда 1), его ставит КОД игры
	 * (`CustomerView@OriginPosMode::set` → `IActivity@Root::get` →
	 * `IParts@OriginPosMode::set` — единственный такой сеттер вне библиотеки),
	 * и тогда привязка относится к СОДЕРЖИМОМУ активности — как у
	 * `ユーザコンポーネント`. См. parts_anchor_shift и FINDINGS §5dz.
	 */
	bool is_activity_root;
	struct string *user_component_name;
	struct parts_uc_data {
		struct string *key;
		struct string *value;
	} *user_component_data;
	int nr_user_component_data;
	bool pass_cursor;
	// PartsEngine.SetEnableInputProcess/IsEnableInputProcess: участвует ли часть в
	// обработке ввода вообще. Отличается от clickable (право на клик) — это ГЛОБАЛЬНЫЙ
	// выключатель hit-теста и сообщений. Игра гасит им ввод на время анимации:
	// `Motion::Join → InputDisabler@SetPartsParam → CSpriteParts@EnableInputProcess::set`.
	// Дефолт — true (см. parts_init).
	bool enable_input_process;
	// PartsEngine.Parts_SetWheelable: принимает ли часть нотчи колеса. Геттера в
	// библиотеке НЕТ (только сеттер fn203), поэтому смысл взят из имени и сайта
	// `CParts@MouseWheelEvent::add` — подписка на колесо включает приём. Дефолт true =
	// прежнее поведение движка (Tsumamigui 3 и Escalayer этой функции не объявляют
	// вовсе — тул ainliball, — так что у них флаг всегда true и скролл BACK LOG цел).
	bool wheelable;
	// クリップ領域: SetComponentClipArea/GetComponentClipAreaPos{X,Y,Width,Height} +
	// SetComponentEnableClipArea/IsComponentEnableClipArea. Хранится и возвращается;
	// отсечение при рендере пока не реализовано (см. PE_SetComponentEnableClipArea).
	bool clip_area_enabled;
	Rectangle clip_area;
	bool lock_input_state;
	bool want_save;
	// Отдельный флаг для снимка «画面保管»/BACK SCENE (PartsEngine.SetWantSaveBackScene).
	// НЕ переиспользуем want_save: игра гасит им служебные оверлеи (системные кнопки,
	// mode-CG, отладочные тексты), и если исключить их ещё и из ИГРОВОГО сейва, после
	// resume-загрузки они не вернутся — конструкторы вьюх повторно не выполняются.
	bool want_save_back_scene;
	// Парт — КОПИЯ из снимка бэк-сцены (создан LoadBackScene в своём пространстве номеров).
	// По этому флагу ClearBackScene сносит снимок, не гадая по диапазону номеров.
	bool back_scene_copy;
	bool draggable;
	// «Свайп» (`スワイプ` в раскладке, Parts_Set/GetSwipeType): вид инерционного
	// перетаскивания списка. Значение ХРАНИТСЯ и отдаётся геттером — им игра
	// читает текущий режим (`ScrollBase@CreateSwipeTargetParts`), — а САМО
	// поведение свайпа НЕ РЕАЛИЗОВАНО: смысл конкретных значений не установлен,
	// а списки прокручиваются полосой и колесом.
	int swipe_type;
	int on_cursor_sound;
	int on_click_sound;
	int origin_mode;
	int pending_parent;
	int linked_to;
	int linked_from;
	// «Привязка прокрутки»: номер части, за которой следует позиция скролла этой.
	// Значение ХРАНИТСЯ, но САМО СЛЕДОВАНИЕ НЕ РЕАЛИЗОВАНО (на включённую привязку
	// печатается одноразовый WARNING) — семантику снять не удалось: в Tsumamigui 3
	// вызов идёт из `parts::detail::CPartsPanelList`, геттеры игрой не читаются, а
	// гадать направление и масштаб связи значит подменить громкое падение тихой
	// неверной прокруткой. Хранить всё равно нужно: без экспорта движок уходил в REPL.
	int scroll_pos_x_link;
	int scroll_pos_y_link;
	/*
	 * `オンカーソル表示連動` — «показ, связанный с наведением курсора»: номер части,
	 * при наведении на которую показывается ЭТА. В раскладке поле хранит ИМЯ соседней
	 * части, загрузчик разрешает его в номер отложенно (act_apply_pending_hover_links).
	 * Пока поле не читалось, все четыре подсказки страницы Window у Dohna были
	 * показаны разом и печатались одна поверх другой.
	 */
	int on_cursor_show_link;
	bool is_hovered;
	int hover_time;
	int draw_filter;
	/*
	 * `減算色モード` / `SetComponentSubColorMode` — добавочный цвет ВЫЧИТАЕТСЯ вместо
	 * прибавления (см. parts_render_cg/parts_render_text: шейдер считает
	 * `tex * multiply_color + add_color`, режим — смена знака).
	 *
	 * Функция была ДЫРОЙ: `SetComponentSubColorMode` не экспортировалась вовсе, а
	 * незнакомая HLL-функция фатальна — экран просмотра CG уходил в отладочный REPL,
	 * и «ничего не нажималось». Ставит её `CgThumbnailButton@RegisterStateEvent`
	 * (через `CLayoutBoxParts@SubColorMode::set`) при построении миниатюр.
	 *
	 * ★В образ (сейв) поле НЕ пишется — формат менять нельзя, а игра ставит режим
	 * заново при построении своих вьюх.
	 */
	bool sub_color_mode;
	bool message_window;
	int alpha_clipper_parts_no;
	/*
	 * Часть служит АЛЬФА-МАСКОЙ для кого-то (кто-то указал её в
	 * `alpha_clipper_parts_no`). Такую часть саму рисовать НЕЛЬЗЯ: её роль —
	 * задавать форму, а не картинку. У Dohna клиппер обучения
	 * (`Tutorial::CreateClipper`) — construction 1280×720, залитая ЧЁРНЫМ, и,
	 * будучи нарисованной, она накрывала экран.
	 */
	bool is_alpha_clipper;
	/*
	 * Сколько частей ссылаются на эту как на альфа-маску. Без счётчика метку
	 * `is_alpha_clipper` было не снять, и она оставалась навсегда: часть,
	 * побывавшая маской, больше не рисовалась НИКОГДА. Так пропадал фон страниц
	 * CONFIG у Dohna — построенная поверхность (размытый `背景／ナユタ` с чёрной
	 * заливкой по альфе 180) лежала готовой, а рендер её пропускал.
	 */
	int alpha_clipper_refs;
	int margin_top;
	int margin_bottom;
	int margin_left;
	int margin_right;
	struct parts_motion_list motion;
	int controller_no;
	// Окно реплик ADV (`パーツタイプ = メッセージウィンドウ`, тип компонента v14 = 10).
	// Не NULL только у частей, построенных из такого узла раскладки; см.
	// src/parts/message_window.c.
	struct parts_message_window *mw;
};

/*
 * Окно реплик ADV (`メッセージウィンドウ`) — тип части, которого у v6/v7 нет вовсе
 * (проверено тулом ainlibbyname: у Tsumamigui 3 из 42 функций `*MessageWindow*`
 * объявлены ровно две — `SetComponentMessageWindowShowLink`/`Is…`, то есть флаг
 * привязки чужой части к окну, а самой части нет; у Dohna объявлены все 42).
 *
 * СОБРАНО ИЗ ГОТОВЫХ КИРПИЧЕЙ, А НЕ НОВЫМ `enum parts_type`: сама часть держит
 * фон окна обычным состоянием `PARTS_CG`/`PARTS_FLAT`, а текст и «мигалку»
 * ожидания клика несут ДВЕ служебные части-потомка. Так подсистема бесплатно
 * получает всё, что уже работает у частей: якорь 原点座標モード, motion-анимации,
 * альфу и乗算色 (они наследуются потомками через parts_update_global_*),
 * альфа-клиппер, сохранение в сейв и метрики текста (те же, по которым выверено
 * окно сообщений Tsumamigui 3). Отдельный тип части пришлось бы протаскивать
 * через render/save/debug/hittest и дублировать там уже написанное.
 *
 * Раскладка узла (`種類別情報` в .pactex, напр. Scene/10_Adv/Main/
 * AdvMessageWindow_event.pactex) один-в-один ложится на 42 HLL-функции:
 * アクティブ→SetMessageWindowActive, ＣＧ名→…CGName, テキストエリア→…TextArea,
 * 字速度→…TextSpeed, ルビ→…Ruby*, и т.д.
 */
struct parts_message_window {
	bool active;
	SDL_Color inactive_multiply_color;  // 非アクティブ時の乗算カラー
	struct string *cg_name;             // ＣＧ名
	struct string *flat_name;           // フラット名
	int flat_show_wait_frames;          // フラット表示待ちフレーム数
	Rectangle text_area;                // テキストエリア
	int text_origin_pos_mode;           // テキスト位置
	int text_speed;                     // 字速度
	bool text_wrapping;                 // 折り返し
	bool text_fixed;                    // текст проявлен целиком
	/*
	 * ПОСИМВОЛЬНОЕ ПРОЯВЛЕНИЕ реплики. `char_ms` — время на каждый РИСУЕМЫЙ символ
	 * (уже с множителем настройки メッセージ表示速度), нулевое значение = мгновенно
	 * (`${time 0}`). `plain_text` нужен для сверки ПРЕФИКСА: игра пересобирает
	 * строку окна с нуля (Clear -> мгновенный повтор старого -> Fix -> новый хвост),
	 * и уже проявленные символы обязаны остаться проявленными.
	 */
	struct string *plain_text;
	/*
	 * Текст окна КАК ЕГО ПОДАЛА ИГРА, вместе с разметкой. Именно его обязан
	 * возвращать GetMessageWindowText: игра накапливает страницу, дописывая новый
	 * кусок к прочитанному обратно (`CMessageTextView@CreateDrawChar`), и если
	 * отдавать очищенный текст, то с каждым куском теряются И обёртки `${time 0}`
	 * уже показанного, И чтения `${ruby …}` — руби гасло на следующей же реплике
	 * страницы (замер XSYS4_RUBY_TRACE: чтение строилось и тут же пропадало).
	 */
	struct string *raw_text;
	int *char_ms;
	int nr_reveal_chars;
	int reveal_shown;
	int reveal_timer;                   // ms, накопленные для символа reveal_shown
	// Идентичность реплики из SetMessageWindowText(…, MsgNum, FuncName, Ver, Step) —
	// ею игра метит сообщение для 既読判定 (флага «прочитано»).
	int msg_num;
	struct string *msg_func_name;
	int msg_ver, msg_step;
	// Базовый цвет текста из раскладки (SetMessageWindowTextFont): к нему
	// возвращаемся после 既読-перекраски прочитанных реплик.
	SDL_Color text_color;
	// Шрифт руби: своего носителя у него нет (служебная часть текста хранит только
	// основной), поэтому лежит здесь.
	struct text_style ruby_ts;
	int ruby_char_space, ruby_line_space;
	// キー待ちマーク — значок «жду клика». Хранится здесь, рисуется служебной
	// частью-потомком; показом управляет игра (SetKeyWaitShow).
	struct string *mark_cg_name;
	struct string *mark_flat_name;
	int mark_start_no, mark_nr_cg, mark_time_per_cg;  // ループＣＧ開始番号/枚数/切り替え時間
	Point mark_pos;
	int mark_z;
	bool mark_show;
	// Служебные части-потомки: текст и キー待ちマーク. -1 = не создана.
	int text_parts_no;
	int mark_parts_no;
};

#define PARTS_LIST_FOREACH(iter) TAILQ_FOREACH(iter, &parts_list, parts_list_entry)
#define PARTS_LIST_FOREACH_REVERSE(iter) TAILQ_FOREACH_REVERSE(iter, &parts_list, parts_list, parts_list_entry)
#define PARTS_FOREACH_CHILD(iter, parent) TAILQ_FOREACH(iter, &parent->children, child_list_entry)

// parts.c
extern struct parts_list parts_list;

// Controllers are identified by their position in the stack (0 = bottom). The
// system overlay controller lives outside the stack.
#define PARTS_CONTROLLER_STACK_MAX 10000
#define PARTS_CONTROLLER_SYSTEM_OVERLAY PARTS_CONTROLLER_STACK_MAX

struct parts_controller_stack {
	int nr_controllers;
	int active;  // stack index or PARTS_CONTROLLER_SYSTEM_OVERLAY
	// Видимость СЛОЯ (см. parts_controller_set_show). Игра прячет и показывает целые
	// слои: `CBackSceneView@HideAllFrontScene` перебирает GetControllerID(i) и гасит
	// каждый видимый слой через SetComponentShow — так экран игры убирается на время
	// просмотра бэк-сцены. Флаг слоя ОТДЕЛЬНЫЙ от `show` партов, иначе обратный
	// ShowAllFrontScene засветил бы парты, спрятанные игрой поимённо.
	// Погашен ли слой — ПО ПОЗИЦИИ в стеке, а не по id: id теперь монотонный и
	// расти может сколь угодно долго, массивом по нему не проиндексируешься.
	// Сдвигается вместе со `stack` при снятии слоя.
	bool hidden[PARTS_CONTROLLER_STACK_MAX + 1];
	// Следующий свободный id. ★МОНОТОННЫЙ, номера НЕ переиспользуются: иначе парт,
	// который пересоздало покадровое событие уже МЁРТВОГО экрана, попадал в НОВЫЙ
	// слой с тем же номером и всплывал наверх (у Haha Ranman так плёночный шум
	// титула висел поверх ADV-сцены). С монотонным id номер мёртвого слоя больше
	// никому не достаётся, и такой парт уходит под всё (parts_get_sprite_z).
	int next_id;
	/*
	 * ID слоёв в порядке стека (низ → верх). ID УСТОЙЧИВ и НЕ равен позиции:
	 * игра сносит слой ПО ЕГО НОМЕРУ, в том числе из СЕРЕДИНЫ стека, и если
	 * приравнять id к позиции, то после такого удаления все номера выше
	 * съезжают — игра продолжает адресовать старыми, а движок понимает их
	 * как чужие. Живой случай: Haha Ranman, переход в ADV-сцену — игра просила
	 * снести слой 2, движок сносил активный 3, и парты слоя 2 (плёночный шум
	 * титула, рамка юнит-анимации `行動選択`) оставались на экране поверх сцены.
	 * Новый id — НАИМЕНЬШИЙ СВОБОДНЫЙ, поэтому при чисто стековом использовании
	 * (все прежние игры) нумерация ровно та же, что была: 0, 1, 2, …
	 */
	int stack[PARTS_CONTROLLER_STACK_MAX + 1];
};
extern struct parts_controller_stack ctrl_stack;

// hll/PartsEngine.c: реестр активностей в сейв-образе партов (XPE v5).
struct iarray_writer;
struct iarray_reader;
void pe_activities_save(struct iarray_writer *w);
// Сдвинуть счётчик номеров частей активностей выше восстановленного номера.
void pe_act_seq_reserve(int parts_no);
bool pe_activities_load(struct iarray_reader *r, bool apply, int version);
extern bool parts_multi_controller;

struct parts *parts_try_get(int parts_no);
struct parts *parts_get(int parts_no);
struct parts_cg *parts_get_cg(struct parts *parts, int state);
struct parts_text *parts_get_text(struct parts *parts, int state);
struct parts_animation *parts_get_animation(struct parts *parts, int state);
struct parts_numeral *parts_get_numeral(struct parts *parts, int state);
struct parts_gauge *parts_get_hgauge(struct parts *parts, int state);
struct parts_gauge *parts_get_vgauge(struct parts *parts, int state);
struct parts_construction_process *parts_get_construction_process(struct parts *parts, int state);
struct parts_flash *parts_get_flash(struct parts *parts, int state);
struct parts_flat *parts_get_flat(struct parts *parts, int state);
struct parts_movie *parts_get_movie(struct parts *parts, int state);
struct parts_layout_box *parts_get_layout_box(struct parts *parts);
struct parts_panel *parts_get_panel(struct parts *parts, int state);
struct parts_3dlayer *parts_get_3dlayer(struct parts *parts, int state);
void parts_set_pos(struct parts *parts, Point pos);
void parts_set_z(struct parts *parts, int z);
void parts_set_global_pos(Point pos);
void parts_set_dims(struct parts *parts, struct parts_common *common, int w, int h);
void parts_set_scale_x(struct parts *parts, float mag);
void parts_set_scale_y(struct parts *parts, float mag);
void parts_set_rotation_z(struct parts *parts, float rot);
void parts_set_alpha(struct parts *parts, int alpha);
void parts_set_show(struct parts *parts, bool show);
void parts_set_state(struct parts *parts, enum parts_state_type state);
void parts_release(int parts_no);
void parts_release_all(void);
bool parts_hidden_by_layer(struct parts *parts);
void parts_set_surface_area(struct parts *parts, struct parts_common *common, int x, int y, int w, int h);
extern bool parts_message_window_show;

extern struct parts_numeral_font *parts_numeral_fonts;
extern int parts_nr_numeral_fonts;

// for save.c
void parts_list_resort(struct parts *parts);
// Раз в кадр перед вводом: не разошёлся ли порядок списка с z (см. parts.c).
bool parts_list_order_check(void);
void parts_component_dirty(struct parts *parts);
void parts_recalculate_hitbox(struct parts *parts);
void parts_debug_dump(void);
bool parts_take_debug_dump_request(void);
void parts_state_reset(struct parts_state *state, enum parts_type type);
bool parts_cg_set(struct parts *parts, struct parts_cg *cg, struct string *cg_name);
bool parts_cg_set_by_index(struct parts *parts, struct parts_cg *cg, int cg_no);
void parts_text_append(struct parts *parts, struct parts_text *t, struct string *text);
bool parts_animation_set_cg_by_index(struct parts *parts, struct parts_animation *anim,
		int cg_no, int nr_frames, int frame_time);
bool parts_animation_set_cg(struct parts *parts, struct parts_animation *anim,
		struct string *cg_name, int start_no, int nr_frames, int frame_time);
void parts_numeral_font_init(struct parts_numeral_font *font);
bool parts_numeral_set_number(struct parts *parts, struct parts_numeral *num, int n);
bool parts_gauge_set_cg(struct parts *parts, struct parts_gauge *g, struct string *cg_name);
bool parts_gauge_set_cg_by_index(struct parts *parts, struct parts_gauge *g, int cg_no);
void parts_hgauge_set_rate(struct parts *parts, struct parts_gauge *g, float rate);
void parts_vgauge_set_rate(struct parts *parts, struct parts_gauge *g, float rate);

// text.c
void parts_text_free(struct parts_text *t);
struct string *parts_text_line_get(struct parts_text_line *line);
void parts_text_set_ruby(struct parts *parts, int state, const struct text_style *ts,
		int line_space, const struct parts_ruby_spec *specs, int nr_specs);
struct string *parts_text_get(struct parts_text *t);

/*
 * ЗЕРКАЛО ПОДДЕРЕВА (см. `parts_params::mirror_x`). Две величины нужны и рендеру,
 * и hit-тесту, поэтому живут здесь.
 *
 * `parts_effective_reverse_*` — переворачивать ли СОДЕРЖИМОЕ части: свой флаг
 * складывается (XOR) с зеркальностью системы координат предков.
 *
 * `parts_origin_offset_*` — где относительно точки привязки лежит КОРОБКА части.
 * В зеркальной системе коробка отражается вокруг точки привязки, поэтому сдвиг
 * становится `-(сдвиг + размер)`. Для привязки по центру (`原点座標モード` 2/5/8,
 * сдвиг −w/2) это то же число — так что боевые спрайты Dohna не двигаются, а
 * зеркалится только их картинка; для краевых привязок разница настоящая.
 */
static inline bool parts_effective_reverse_lr(struct parts *parts)
{
	return parts->global.mirror_x != parts->reverse_lr;
}

static inline bool parts_effective_reverse_tb(struct parts *parts)
{
	return parts->global.mirror_y != parts->reverse_tb;
}

/*
 * ИТОГОВОЕ отражение картинки части: `sprite_deform` (0/1/2, приходит четвёртым
 * аргументом `SetPartsCG`) плюс личные флаги плюс зеркальность системы координат.
 * Одно место на всех, потому что этой величиной обязаны пользоваться и рендер, и
 * ПОПИКСЕЛЬНЫЙ hit-тест, и маска альфа-клиппера: если они разойдутся, «клик по
 * спрайту» будет проверять не те тексели, что видны на экране.
 */
static inline bool parts_render_flip_h(struct parts *parts)
{
	bool flip = parts_effective_reverse_lr(parts);
	return parts->sprite_deform == 1 ? !flip : flip;
}

static inline bool parts_render_flip_v(struct parts *parts)
{
	bool flip = parts_effective_reverse_tb(parts);
	return parts->sprite_deform == 2 ? !flip : flip;
}

static inline int parts_origin_offset_x(struct parts *parts, struct parts_common *c)
{
	return parts->global.mirror_x ? -(c->origin_offset.x + c->w) : c->origin_offset.x;
}

static inline int parts_origin_offset_y(struct parts *parts, struct parts_common *c)
{
	return parts->global.mirror_y ? -(c->origin_offset.y + c->h) : c->origin_offset.y;
}

// render.c
void parts_render_init(void);
void parts_render_update(void);
void parts_engine_dirty(void);
void parts_engine_clean(void);
void parts_dirty(struct parts *parts);
void parts_sprite_render(struct sprite *sp);
void parts_render(struct parts *parts);
void parts_render_family(struct parts *parts);

// motion.c
void parts_clear_motion(struct parts *parts);
void parts_add_motion(struct parts *parts, struct parts_motion *motion);

// input.c
extern bool parts_began_click;
void parts_input_reset(void);
void parts_input_reset_drag(struct parts *parts);

// message.c
enum parts_message_type {
	PARTS_MSG_MOUSE_ENTER    = 1,
	PARTS_MSG_MOUSE_MOVE     = 2,
	PARTS_MSG_MOUSE_LEAVE    = 3,
	PARTS_MSG_MOUSE_WHEEL    = 4,
	PARTS_MSG_MOUSE_CLICK    = 5,
	PARTS_MSG_MOUSE_ON       = 6,
	PARTS_MSG_DRAG_BEGIN     = 7,
	PARTS_MSG_DRAGGING       = 8,
	PARTS_MSG_DRAG_END       = 9,
	PARTS_MSG_DROP_ENTER     = 10,
	PARTS_MSG_DROP_ON        = 11,
	PARTS_MSG_DROPPED        = 12,
	PARTS_MSG_DROP_LEAVE     = 13,
	PARTS_MSG_KEY_TRIGGER    = 14,
	PARTS_MSG_KEY_DOWN       = 15,
	// Номер 16 у нас ОТСУТСТВОВАЛ, и это ломало все кнопки, повешенные игрой через
	// `AddKeyPressEvent`: у Tsumamigui 3 так привязаны стрелки скроллбара, то есть
	// 前へ/次へ бэк-сцены (`CBackSceneView@PressPrevButton/PressNextButton`) — клик
	// доходил до парта, но сообщение этого типа никто не отправлял. Таблица типов взята
	// из диспетчера игры (`CPartsMessageManager@CallDelegate`, SWITCH по MessageType):
	// 1 MouseEnter, 2 MouseMove, 3 MouseLeave, 4 MouseWheel, 5 MouseClick,
	// 6 MouseOnCursor, 7..13 drag/drop, 14 KeyTrigger, 15 KeyDown, 16 KeyPress, 17 KeyUp.
	PARTS_MSG_KEY_PRESS      = 16,
	PARTS_MSG_KEY_UP         = 17,
	// Engine message type = game CallDelegate case + 1 (see CPartsMessageManager).
	// Scroll (game case 19) delivers (ScrollPos, Total) to a scrollbar's registered
	// scroll delegate. Tsumamigui 3 BACK LOG's ScrollEvent -> SetLineIndex rebuilds
	// the visible line window from this; without it the list stayed at the initial
	// bottom-anchored 1-line window and showed nothing.
	PARTS_MSG_SCROLL         = 20,
	/*
	 * FIXED — «ввод подтверждён» у текстового поля, БЕЗ аргументов. Номер снят из
	 * того же диспетчера игры: `.CASE 50:24 25` → `CallDelegateFixed`, а
	 * `CallDelegateFixed` требует `GetMessageVariableCount() == 0`.
	 * На него игра вешает чтение введённого текста: у Tsumamigui 3 это
	 * `C_SAVE_CONFIRM@CommentFixedEvent` (FUNC 7975) — он зовёт
	 * `Ｐ＿テキストボックス＿テキスト取得`, прячет поле и возвращает подсказку.
	 * Пока движок это сообщение не слал, комментарий в сейв не попадал вообще.
	 */
	/*
	 * CHANGED_FLG — «переключён чекбокс», ОДИН аргумент bool с новым состоянием.
	 * Номер снят из того же диспетчера игры: `.CASE 289:24` → `CallEvent1<bool>` с
	 * методом `CPartsFunctionSet@CallFunctionChangedFlg(bool check)`.
	 * На него игра вешает всю логику ярлыков ADV: лямбда
	 * `SceneAdvButtonMenu@AssignCheckEvent` (FUNC 36712) по `Check` добавляет или
	 * убирает пункт в `GameConfig.Shortcuts` и сразу зовёт `UpdateCheckable`.
	 * Пока сообщения не было, движок переключал галочку ЛОКАЛЬНО и игра об этом не
	 * узнавала: набор кнопок внизу экрана не менялся вообще (замер — кадры до и
	 * после клика по `Skip Mode` совпадают по нижней полосе).
	 * ★То же сообщение несёт и настройки CONFIG у Haha Ranman: `Config::MessagePage@Create`
	 * берёт `IActivity@GetCheckBox(имя)` и вешает делегат `(номер, флаг)`, который зовёт
	 * соответствующий `AFL_Config_Set…`.
	 */
	PARTS_MSG_CHANGED_FLG    = 24,
	PARTS_MSG_FIXED          = 25,
	/*
	 * CHANGED — «значение части изменилось». Его ждёт ГРУППА радиокнопок: обработчик
	 * `Config::WindowPage@Create()(67, 79)` берёт по имени обе кнопки группы, читает их
	 * `Checked` и зовёт `AFL_Config_SetScalingType`. Аргумент один (индекс кнопки),
	 * и сам обработчик его не использует — состояние он читает с частей.
	 */
	PARTS_MSG_CHANGED        = 26,
};

void parts_msg_push(struct parts* parts, int type, const char *fmt, ...);
void parts_msg_push_global(int type, const char *fmt, ...);
bool parts_msg_api_new(void);
// Кадровая граница видимости очереди сообщений (см. msg_cur_frame в message.c):
// зовёт PE_UpdateInputState раз в кадр, ДО постановки событий этого кадра.
void parts_msg_new_frame(void);
void parts_hscrollbar_drag_to(struct parts *parts, int cursor_abs_x);
void parts_vscrollbar_drag_to(struct parts *parts, int cursor_abs_y);
void PE_OnVScrollbarDragged(int parts_no, float rate);
bool parts_checkbox_toggle(struct parts *parts);
void PE_OnHScrollbarDragged(int parts_no, float rate);

// construction.c
void parts_cp_op_free(struct parts_cp_op *op);
void parts_add_cp_op(struct parts_construction_process *cproc, struct parts_cp_op *op);
bool parts_build_construction_process(struct parts *parts,
		struct parts_construction_process *cproc);
bool parts_clear_construction_process(struct parts_construction_process *cproc);

// flash.c
void parts_flash_free(struct parts_flash *f);
bool parts_flash_load(struct parts *parts, struct parts_flash *f, struct string *filename);
bool parts_flash_update(struct parts_flash *f, int passed_time);
bool parts_flash_seek(struct parts_flash *f, int frame);

// flat.c
void parts_flat_free(struct parts_flat *f);
bool parts_flat_load(struct parts *parts, struct parts_flat *f, struct string *filename);
bool parts_flat_update(struct parts_flat *f, int passed_time);
int parts_flat_find_library(struct flat *fl, const char *name);
// Библиотека-подкартинка из атласа TALT (когда в LIBL такого имени нет).
const struct flat_atlas_lib *parts_flat_find_atlas_lib(struct parts_flat *f, const char *name);
int parts_flat_stop_motion_get_cg_lib(struct parts_flat *f, int sm_lib_idx, int local);

struct flat_emitter;
struct flat_key_data_graphic;
struct flat_timeline;

struct flat_emitter_particle {
	vec2 pos;          // emitter-space position (pixels)
	vec2 scale;
	vec3 rot;          // degrees (x, y, z)
	float fade_alpha;  // 0-1.0
	int cg_lib_idx;    // CG library index of the texture for this particle
};

// Per-key emitter properties after applying the emitter's inherit_* flags.
struct flat_emitter_layer_effective {
	vec2 pos;
	bool reverse_lr, reverse_tb;
	float alpha;
	vec3 add_color;
	vec3 mul_color;
	int draw_filter;
	bool use_scale;
	bool use_rotation;
	bool use_origin;
};

#define FLAT_MAX_ANCESTOR_DEPTH 32
struct flat_key_stack {
	const struct flat_key_data_graphic *keys[FLAT_MAX_ANCESTOR_DEPTH];
	int count;
};

typedef void (*flat_emitter_particle_fn)(const struct flat_emitter_particle *p,
		void *ud);
bool parts_flat_emitter_get_align_offset(struct parts_flat *f, int emitter_lib_idx, vec2 out);
void parts_flat_foreach_emitter_particle(struct parts_flat *f, int emitter_lib_idx,
		const struct flat_timeline *tl,
		int birth_frame, int age, int frame_count,
		flat_emitter_particle_fn fn, void *ud);
void parts_flat_build_layer_matrix(const struct flat_key_data_graphic *key,
		vec2 pos,
		bool use_rotation, bool use_scale, bool use_origin,
		bool reverse_lr, bool reverse_tb,
		mat4 out);
void parts_flat_build_emitter_base_matrix(const struct flat_emitter *em,
		const struct flat_key_stack *stack, mat4 out);
void parts_flat_emitter_resolve_layer(
		const struct flat_emitter *em,
		const struct flat_key_data_graphic *key,
		float parts_alpha, float layer_alpha,
		struct flat_emitter_layer_effective *out);

// layoutbox.c
void parts_do_layout(struct parts *parts);
// Размер части-обёртки по прямоугольнику, который занимают её потомки.
void parts_get_content_size(struct parts *parts, int *w, int *h);

// message_window.c
struct parts_message_window *parts_message_window_alloc(void);
void parts_message_window_free(struct parts_message_window *mw);
void parts_message_window_relayout(struct parts *parts);
// Тик посимвольного проявления реплики; true — картинка изменилась.
bool parts_message_window_update(struct parts *parts, int passed_time);

// debug.c
struct sprite;
void parts_debug_init(void);
cJSON *parts_engine_to_json(struct sprite *sp, bool verbose);
cJSON *parts_sprite_to_json(struct sprite *sp, bool verbose);

static inline bool parts_state_valid(int state)
{
	return state >= 0 && state <= 2;
}

#endif /* SYSTEM4_PARTS_INTERNAL_H */
