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

#include <string.h>
#include <stdlib.h>

#include "system4/ex.h"
#include "system4/string.h"
#include "system4/ain.h"

#include "xsystem4.h"
#include "vm.h"
#include "vm/heap.h"
#include "vm/page.h"
#include "hll.h"
#include "iarray.h"

#include "system4/utfsjis.h"

// TODO: Later versions of this library have a different interface, but use the
//       same function names. Will need to handle this somehow...
//       Probably best to have separate implementations and choose the correct
//       one at startup.

static struct ex *ex;
static struct ex_value **handles = NULL;
static unsigned nr_handles = 0;

static int set_indices(struct ex_value *val, int id)
{
	val->id = id++;

	switch (val->type) {
	case EX_INT:
	case EX_FLOAT:
	case EX_STRING:
		break;
	case EX_TABLE:
		for (unsigned row = 0; row < val->t->nr_rows; row++) {
			for (unsigned col = 0; col < val->t->nr_columns; col++) {
				id = set_indices(&val->t->rows[row][col], id);
			}
		}
		break;
	case EX_LIST:
		for (unsigned i = 0; i < val->list->nr_items; i++) {
			id = set_indices(&val->list->items[i].value, id);
		}
		break;
	case EX_TREE:
		if (val->tree->is_leaf) {
			id = set_indices(&val->tree->leaf.value, id);
		} else {
			for (unsigned i = 0; i < val->tree->nr_children; i++) {
				id = set_indices(&val->tree->_children[i], id);
			}
		}
		break;
	}

	return id;
}

static void map_handles(struct ex_value *val)
{
	handles[val->id] = val;

	switch (val->type) {
	case EX_INT:
	case EX_FLOAT:
	case EX_STRING:
		break;
	case EX_TABLE:
		for (unsigned row = 0; row < val->t->nr_rows; row++) {
			for (unsigned col = 0; col < val->t->nr_columns; col++) {
				map_handles(&val->t->rows[row][col]);
			}
		}
		break;
	case EX_LIST:
		for (unsigned i = 0; i < val->list->nr_items; i++) {
			map_handles(&val->list->items[i].value);
		}
		break;
	case EX_TREE:
		if (val->tree->is_leaf) {
			map_handles(&val->tree->leaf.value);
		} else {
			for (unsigned i = 0; i < val->tree->nr_children; i++) {
				map_handles(&val->tree->_children[i]);
			}
		}
		break;
	}
}

static void MainEXFile_ModuleInit(void)
{
	// load .ex file
	if (!config.ex_path || !(ex = ex_read_file(config.ex_path)))
		ERROR("Failed to load .ex file: %s", display_utf0(config.ex_path));

	// assign IDs to each ex_value
	int id = 1;
	for (unsigned i = 0; i <  ex->nr_blocks; i++) {
		id = set_indices(&ex->blocks[i].val, id);
	}

	// create index mapping IDs to ex_values
	nr_handles = id;
	handles = xcalloc(nr_handles, sizeof(struct ex_value*));
	for (unsigned i = 0; i < ex->nr_blocks; i++) {
		map_handles(&ex->blocks[i].val);
	}
}

static void MainEXFile_ModuleFini(void)
{
	ex_free(ex);
	ex = NULL;
	free(handles);
	handles = NULL;
	nr_handles = 0;
}

HLL_WARN_UNIMPLEMENTED(false, bool, MainEXFile, ReloadDebugEXFile, void);

/*
 * Get handle for top-level value.
 */
static int MainEXFile_Handle(struct string *name)
{
	struct ex_value *v = ex_get(ex, name->text);
	return v ? v->id : 0;
}

/*
 * Newer System 4 games use a string-key based MainEXFile API where each
 * accessor takes the full dotted path directly (Int(key, ref, exidx), ...)
 * instead of first resolving an integer handle via Handle(). This resolver
 * maps a dotted key to the internal handle id so we can reuse the existing
 * handle-based navigation logic.
 */
static int ex_key_handle(struct string *key)
{
	struct ex_value *v = ex_get(ex, key->text);
	if (getenv("XSYS4_EX_TRACE"))
		WARNING("EX '%s' -> %s", display_sjis0(key->text), v ? "HIT" : "MISS");
	return v ? v->id : 0;
}

/* Internal handle-based value readers (shared by scalar and array accessors). */
static bool hv_int(int handle, int *data)
{
	if (handle <= 0 || (unsigned)handle >= nr_handles)
		return false;
	if (handles[handle]->type != EX_INT) {
		WARNING("Value is not an integer");
		return false;
	}
	*data = handles[handle]->i;
	return true;
}

static bool hv_float(int handle, float *data)
{
	if (handle <= 0 || (unsigned)handle >= nr_handles)
		return false;
	if (handles[handle]->type != EX_FLOAT) {
		WARNING("Value is not a float");
		return false;
	}
	*data = handles[handle]->f;
	return true;
}

static bool hv_string(int handle, struct string **data)
{
	if (handle <= 0 || (unsigned)handle >= nr_handles)
		return false;
	if (handles[handle]->type != EX_STRING) {
		WARNING("Value is not a string");
		return false;
	}
	*data = string_ref(handles[handle]->s);
	return true;
}

static struct ex_table *handle_to_table(int handle)
{
	if (handle <= 0 || (unsigned)handle >= nr_handles)
		return NULL;
	if (handles[handle]->type != EX_TABLE) {
		WARNING("Value is not a table");
		return NULL;
	}
	return handles[handle]->t;
}

static struct ex_list *handle_to_list(int handle)
{
	if (handle <= 0 || (unsigned)handle >= nr_handles)
		return NULL;
	if (handles[handle]->type != EX_LIST) {
		WARNING("Value is not a list");
		return NULL;
	}
	return handles[handle]->list;
}

static int MainEXFile_AHandle(int handle, int index)
{
	struct ex_list *list = handle_to_list(handle);
	if (!list)
		return 0;
	struct ex_value *v = ex_list_get(list, index);
	return v ? v->id : 0;
}

static int MainEXFile_A2Handle(int handle, int row, int col)
{
	struct ex_table *t = handle_to_table(handle);
	if (!t)
		return 0;
	struct ex_value *v = ex_table_get(t, row, col);
	return v ? v->id : 0;
}

static int MainEXFile_IA2Handle(int handle, int key, struct string *format_name)
{
	struct ex_table *t = handle_to_table(handle);
	if (!t)
		return 0;
	int row = ex_row_at_int_key(t, key);
	if (row < 0)
		return 0;
	int col = ex_col_from_name(t, format_name->text);
	if (col < 0)
		return 0;
	return t->rows[row][col].id;
}

static int MainEXFile_SA2Handle(int handle, struct string *key, struct string *format_name)
{
	struct ex_table *t = handle_to_table(handle);
	if (!t)
		return 0;
	int row = ex_row_at_string_key(t, key->text);
	if (row < 0)
		return 0;
	int col = ex_col_from_name(t, format_name->text);
	if (col < 0)
		return 0;
	return t->rows[row][col].id;
}

static int MainEXFile_RA2Handle(int handle, int row, struct string *format_name)
{
	struct ex_table *t = handle_to_table(handle);
	if (!t)
		return 0;
	if (row < 0 || (unsigned)row >= t->nr_rows)
		return 0;
	int col = ex_col_from_name(t, format_name->text);
	if (col < 0)
		return 0;
	return t->rows[row][col].id;
}

static int MainEXFile_Row(struct string *path, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	return t ? t->nr_rows : 0;
}

static int MainEXFile_Col(struct string *path, int exidx)
{
	(void)exidx;
	int handle = ex_key_handle(path);
	if (handle > 0 && (unsigned)handle < nr_handles &&
	    handles[handle]->type == EX_LIST)
		return handles[handle]->list->nr_items;
	struct ex_table *t = handle_to_table(handle);
	return t ? t->nr_columns : 0;
}

static int MainEXFile_Type(struct string *path, int exidx)
{
	(void)exidx;
	int handle = ex_key_handle(path);
	if (handle <= 0 || (unsigned)handle >= nr_handles)
		return 0;
	return handles[handle]->type;
}

static int MainEXFile_AType(struct string *path, int index, int exidx)
{
	(void)exidx;
	struct ex_list *list = handle_to_list(ex_key_handle(path));
	if (!list)
		return 0;
	struct ex_value *v = ex_list_get(list, index);
	return v ? v->type : 0;
}

static int MainEXFile_A2Type(struct string *path, int row, int col, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;
	struct ex_value *v = ex_table_get(t, row, col);
	return v ? v->type : 0;
}

static int MainEXFile_IA2Type(struct string *path, int key, struct string *format_name, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;
	int row = ex_row_at_int_key(t, key);
	if (row < 0)
		return 0;
	int col = ex_col_from_name(t, format_name->text);
	if (col < 0)
		return 0;
	return t->rows[row][col].type;
}

static int MainEXFile_SA2Type(struct string *path, struct string *key, struct string *format_name, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;
	int row = ex_row_at_string_key(t, key->text);
	if (row < 0)
		return 0;
	int col = ex_col_from_name(t, format_name->text);
	if (col < 0)
		return 0;
	return t->rows[row][col].type;
}

static int MainEXFile_RA2Type(struct string *path, int row, struct string *format_name, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;
	if (row < 0 || (unsigned)row >= t->nr_rows)
		return 0;
	int col = ex_col_from_name(t, format_name->text);
	if (col < 0)
		return 0;
	return t->rows[row][col].type;
}

static bool MainEXFile_Exists(struct string *path, int exidx)
{
	(void)exidx;
	int handle = ex_key_handle(path);
	if (handle <= 0 || (unsigned)handle >= nr_handles)
		return false;
	return true;
}

static bool MainEXFile_AExists(struct string *path, int index, int exidx)
{
	(void)exidx;
	struct ex_list *list = handle_to_list(ex_key_handle(path));
	if (!list)
		return false;
	return !!ex_list_get(list, index);
}

static bool MainEXFile_A2Exists(struct string *path, int row, int col, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return false;
	return !!ex_table_get(t, row, col);
}

static bool MainEXFile_IA2Exists(struct string *path, int key, struct string *format_name, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;
	int row = ex_row_at_int_key(t, key);
	if (row < 0)
		return 0;
	return ex_col_from_name(t, format_name->text) >= 0;
}

static bool MainEXFile_SA2Exists(struct string *path, struct string *key, struct string *format_name, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;
	int row = ex_row_at_string_key(t, key->text);
	if (row < 0)
		return 0;
	return ex_col_from_name(t, format_name->text) >= 0;
}

static bool MainEXFile_RA2Exists(struct string *path, int row, struct string *format_name, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;
	if (row < 0 || (unsigned)row >= t->nr_rows)
		return 0;
	return ex_col_from_name(t, format_name->text) >= 0;
}

static bool MainEXFile_Int(struct string *path, int *data, int exidx)
{
	(void)exidx;
	return hv_int(ex_key_handle(path), data);
}

static bool MainEXFile_Float(struct string *path, float *data, int exidx)
{
	(void)exidx;
	return hv_float(ex_key_handle(path), data);
}

static bool MainEXFile_String(struct string *path, struct string **data, int exidx)
{
	(void)exidx;
	return hv_string(ex_key_handle(path), data);
}

static bool MainEXFile_AInt(struct string *path, int index, int *data, int exidx)
{
	(void)exidx;
	struct ex_list *list = handle_to_list(ex_key_handle(path));
	if (!list)
		return 0;

	struct ex_value *v = ex_list_get(list, index);
	if (!v)
		return 0;
	if (v->type != EX_INT) {
		WARNING("Value is not an integer");
		return 0;
	}

	*data = v->i;
	return 1;
}

static bool MainEXFile_AFloat(struct string *path, int index, float *data, int exidx)
{
	(void)exidx;
	struct ex_list *list = handle_to_list(ex_key_handle(path));
	if (!list)
		return 0;

	struct ex_value *v = ex_list_get(list, index);
	if (!v)
		return 0;
	if (v->type != EX_FLOAT) {
		WARNING("Value is not a float");
		return 0;
	}

	*data = v->f;
	return 1;
}

static bool MainEXFile_AString(struct string *path, int index, struct string **data, int exidx)
{
	(void)exidx;
	struct ex_list *list = handle_to_list(ex_key_handle(path));
	if (!list)
		return 0;

	struct ex_value *v = ex_list_get(list, index);
	if (!v)
		return 0;
	if (v->type != EX_STRING) {
		WARNING("Value is not a string");
		return 0;
	}

	*data = string_ref(v->s);
	return 1;
}

static bool MainEXFile_A2Int(struct string *path, int row, int col, int *data, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;

	struct ex_value *v = ex_table_get(t, row, col);
	if (!v)
		return 0;
	if (v->type != EX_INT) {
		WARNING("Value is not an integer");
		return 0;
	}

	*data = v->i;
	return 1;
}

static bool MainEXFile_A2Float(struct string *path, int row, int col, float *data, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;

	struct ex_value *v = ex_table_get(t, row, col);
	if (!v)
		return 0;
	if (v->type != EX_FLOAT) {
		WARNING("Value is not a float");
		return 0;
	}

	*data = v->f;
	return 1;
}

static bool MainEXFile_A2String(struct string *path, int row, int col, struct string **data, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return 0;

	struct ex_value *v = ex_table_get(t, row, col);
	if (!v)
		return 0;
	if (v->type != EX_STRING) {
		WARNING("Value is not a string");
		return 0;
	}

	*data = string_ref(v->s);
	return 1;
}

static int MainEXFile_GetRowAtIntKey(struct string *path, int key, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return -1;
	return ex_row_at_int_key(t, key);
}

static int MainEXFile_GetRowAtStringKey(struct string *path, struct string *key, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return -1;
	return ex_row_at_string_key(t, key->text);
}

static bool MainEXFile_IA2Int(struct string *path, int key, struct string *format_name, int *data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_IA2Handle(ex_key_handle(path), key, format_name);
	if (v_handle <= 0)
		return false;
	return hv_int(v_handle, data);
}

static bool MainEXFile_IA2Float(struct string *path, int key, struct string *format_name, float *data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_IA2Handle(ex_key_handle(path), key, format_name);
	if (v_handle <= 0)
		return false;
	return hv_float(v_handle, data);
}

static bool MainEXFile_IA2String(struct string *path, int key, struct string *format_name, struct string **data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_IA2Handle(ex_key_handle(path), key, format_name);
	if (v_handle <= 0)
		return false;
	return hv_string(v_handle, data);
}

static bool MainEXFile_SA2Int(struct string *path, struct string *key, struct string *format_name, int *data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_SA2Handle(ex_key_handle(path), key, format_name);
	if (v_handle <= 0)
		return false;
	return hv_int(v_handle, data);
}

static bool MainEXFile_SA2Float(struct string *path, struct string *key, struct string *format_name, float *data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_SA2Handle(ex_key_handle(path), key, format_name);
	if (v_handle <= 0)
		return false;
	return hv_float(v_handle, data);
}

static bool MainEXFile_SA2String(struct string *path, struct string *key, struct string *format_name, struct string **data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_SA2Handle(ex_key_handle(path), key, format_name);
	if (v_handle <= 0)
		return false;
	return hv_string(v_handle, data);
}

static bool MainEXFile_RA2Int(struct string *path, int row, struct string *format_name, int *data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_RA2Handle(ex_key_handle(path), row, format_name);
	if (v_handle <= 0)
		return false;
	return hv_int(v_handle, data);
}

static bool MainEXFile_RA2Float(struct string *path, int row, struct string *format_name, float *data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_RA2Handle(ex_key_handle(path), row, format_name);
	if (v_handle <= 0)
		return false;
	return hv_float(v_handle, data);
}

static bool MainEXFile_RA2String(struct string *path, int row, struct string *format_name, struct string **data, int exidx)
{
	(void)exidx;
	int v_handle = MainEXFile_RA2Handle(ex_key_handle(path), row, format_name);
	if (v_handle <= 0)
		return false;
	return hv_string(v_handle, data);
}

static int MainEXFile_GetNodeNameCount(struct string *tree_path)
{
	struct ex_value *v = ex_get(ex, tree_path->text);
	if (!v || v->type != EX_TREE || v->tree->is_leaf)
		return 0;

	int count = 0;
	for (unsigned i = 0; i < v->tree->nr_children; i++) {
		if (!v->tree->children[i].is_leaf)
			count++;
	}
	return count;
}

static int MainEXFile_GetEXNameCount(struct string *tree_path)
{
	struct ex_value *v = ex_get(ex, tree_path->text);
	if (!v || v->type != EX_TREE || v->tree->is_leaf)
		return 0;

	int count = 0;
	for (unsigned i = 0; i < v->tree->nr_children; i++) {
		if (v->tree->children[i].is_leaf)
			count++;
	}
	return count;
}

static bool MainEXFile_GetNodeName(struct string *tree_path, int index, struct string **node_name)
{
	struct ex_value *v = ex_get(ex, tree_path->text);
	if (!v || v->type != EX_TREE || v->tree->is_leaf)
		return false;
	if (index < 0 || (unsigned)index >= v->tree->nr_children)
		return false;

	for (unsigned i = 0, count = 0; i < v->tree->nr_children; i++) {
		if (v->tree->children[i].is_leaf)
			continue;
		if (count == (unsigned)index) {
			*node_name = string_ref(v->tree->children[i].name);
			return true;
		}
		count++;
	}

	return false;
}

static bool MainEXFile_GetEXName(struct string *tree_path, int index, struct string **ex_name)
{
	struct ex_value *v = ex_get(ex, tree_path->text);
	if (!v || v->type != EX_TREE || v->tree->is_leaf)
		return false;
	if (index < 0 || (unsigned)index >= v->tree->nr_children)
		return false;

	for (unsigned i = 0, count = 0; i < v->tree->nr_children; i++) {
		if (!v->tree->children[i].is_leaf)
			continue;
		if (count == (unsigned)index) {
			*ex_name = string_ref(v->tree->children[i].leaf.name);
			return true;
		}
		count++;
	}

	return false;
}

// Tsumamigui 3 и др.: разом вернуть имена всех узлов-детей дерева .ex в массив.
// Хвостовой int (индекс ex-файла) игнорируем, как и прочие функции библиотеки.
static bool MainEXFile_GetNodeNameList(struct string *tree_path, struct page **out, int ex_index)
{
	(void)ex_index;
	struct ex_value *v = ex_get(ex, tree_path->text);
	if (!v || v->type != EX_TREE || v->tree->is_leaf)
		return false;

	struct string **names = NULL;
	int n = 0;
	for (unsigned i = 0; i < v->tree->nr_children; i++) {
		if (v->tree->children[i].is_leaf)
			continue;
		names = xrealloc_array(names, n, n + 1, sizeof(struct string *));
		names[n++] = string_ref(v->tree->children[i].name);
	}

	union vm_value dim = { .i = n };
	struct page *page = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < n; i++)
		page->values[i].i = heap_alloc_string(names[i]);
	free(names);

	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
	}
	*out = page;
	return true;
}

// Like GetNodeNameList, but returns the names of ALL children (leaf or not).
// Used by the CG gallery (title「ＣＧ」button) to enumerate entries under a path.
static bool MainEXFile_GetEXNameList(struct string *tree_path, struct page **out, int ex_index)
{
	(void)ex_index;
	struct ex_value *v = ex_get(ex, tree_path->text);
	if (!v || v->type != EX_TREE || v->tree->is_leaf)
		return false;

	struct string **names = NULL;
	int n = 0;
	for (unsigned i = 0; i < v->tree->nr_children; i++) {
		names = xrealloc_array(names, n, n + 1, sizeof(struct string *));
		names[n++] = string_ref(v->tree->children[i].name);
	}

	union vm_value dim = { .i = n };
	struct page *page = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < n; i++)
		page->values[i].i = heap_alloc_string(names[i]);
	free(names);

	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
	}
	*out = page;
	return true;
}

// Healing Touch и др.: индекс колонки таблицы по её формат-имени (заголовку поля).
static int MainEXFile_GetColAtFormatName(struct string *path, struct string *format_name, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return -1;
	return ex_col_from_name(t, format_name->text);
}

// Вернуть имена всех колонок (полей) таблицы .ex в строковый массив.
static bool MainEXFile_GetFormatNameList(struct string *path, struct page **out, int exidx)
{
	(void)exidx;
	struct ex_table *t = handle_to_table(ex_key_handle(path));
	if (!t)
		return false;

	int n = t->nr_fields;
	union vm_value dim = { .i = n };
	struct page *page = alloc_array(1, &dim, AIN_ARRAY_STRING, 0, false);
	for (int i = 0; i < n; i++)
		page->values[i].i = heap_alloc_string(string_ref(t->fields[i].name));

	if (*out) {
		delete_page_vars(*out);
		free_page(*out);
	}
	*out = page;
	return true;
}

/*
 * Handle-based accessor family. Legacy System 4 games (e.g. Escalayer Reboot)
 * pass a pre-resolved integer handle (obtained from Handle()/AHandle()/...) and
 * omit the trailing exidx argument, whereas the string-key family above takes a
 * dotted path plus exidx. The *Handle() resolvers are already handle-based and
 * shared between both interfaces; only the value/metadata accessors differ.
 * MainEXFile_PreLink() swaps these in when the .ain declares the handle-based
 * signature (Int's first argument is int, not string), leaving string-key games
 * such as Tsumamigui 3 completely untouched.
 */
static int  MEX_h_Row(int h) { struct ex_table *t = handle_to_table(h); return t ? t->nr_rows : 0; }
static int  MEX_h_Col(int h) { if (h > 0 && (unsigned)h < nr_handles && handles[h]->type == EX_LIST) return handles[h]->list->nr_items; struct ex_table *t = handle_to_table(h); return t ? t->nr_columns : 0; }
static int  MEX_h_Type(int h) { if (h <= 0 || (unsigned)h >= nr_handles) return 0; return handles[h]->type; }
static int  MEX_h_AType(int h, int i) { struct ex_list *l = handle_to_list(h); if (!l) return 0; struct ex_value *v = ex_list_get(l, i); return v ? v->type : 0; }
static int  MEX_h_A2Type(int h, int col, int row) { struct ex_table *t = handle_to_table(h); if (!t) return 0; struct ex_value *v = ex_table_get(t, col, row); return v ? v->type : 0; }
static int  MEX_h_IA2Type(int h, int key, struct string *fn) { int vh = MainEXFile_IA2Handle(h, key, fn); return vh > 0 ? MEX_h_Type(vh) : 0; }
static int  MEX_h_SA2Type(int h, struct string *key, struct string *fn) { int vh = MainEXFile_SA2Handle(h, key, fn); return vh > 0 ? MEX_h_Type(vh) : 0; }
static int  MEX_h_RA2Type(int h, int row, struct string *fn) { int vh = MainEXFile_RA2Handle(h, row, fn); return vh > 0 ? MEX_h_Type(vh) : 0; }
static bool MEX_h_Exists(int h) { return h > 0 && (unsigned)h < nr_handles; }
static bool MEX_h_AExists(int h, int i) { struct ex_list *l = handle_to_list(h); return l && !!ex_list_get(l, i); }
static bool MEX_h_A2Exists(int h, int col, int row) { struct ex_table *t = handle_to_table(h); return t && !!ex_table_get(t, col, row); }
static bool MEX_h_IA2Exists(int h, int key, struct string *fn) { return MainEXFile_IA2Handle(h, key, fn) > 0; }
static bool MEX_h_SA2Exists(int h, struct string *key, struct string *fn) { return MainEXFile_SA2Handle(h, key, fn) > 0; }
static bool MEX_h_RA2Exists(int h, int row, struct string *fn) { return MainEXFile_RA2Handle(h, row, fn) > 0; }
static bool MEX_h_Int(int h, int *d) { return hv_int(h, d); }
static bool MEX_h_Float(int h, float *d) { return hv_float(h, d); }
static bool MEX_h_String(int h, struct string **d) { return hv_string(h, d); }
static bool MEX_h_AInt(int h, int i, int *d) { return hv_int(MainEXFile_AHandle(h, i), d); }
static bool MEX_h_AFloat(int h, int i, float *d) { return hv_float(MainEXFile_AHandle(h, i), d); }
static bool MEX_h_AString(int h, int i, struct string **d) { return hv_string(MainEXFile_AHandle(h, i), d); }
static bool MEX_h_A2Int(int h, int a, int b, int *d) { struct ex_table *t = handle_to_table(h); if (!t) return 0; struct ex_value *v = ex_table_get(t, a, b); if (!v || v->type != EX_INT) return 0; *d = v->i; return 1; }
static bool MEX_h_A2Float(int h, int a, int b, float *d) { struct ex_table *t = handle_to_table(h); if (!t) return 0; struct ex_value *v = ex_table_get(t, a, b); if (!v || v->type != EX_FLOAT) return 0; *d = v->f; return 1; }
static bool MEX_h_A2String(int h, int a, int b, struct string **d) { struct ex_table *t = handle_to_table(h); if (!t) return 0; struct ex_value *v = ex_table_get(t, a, b); if (!v || v->type != EX_STRING) return 0; *d = string_ref(v->s); return 1; }
static int  MEX_h_GetRowAtIntKey(int h, int key) { struct ex_table *t = handle_to_table(h); return t ? ex_row_at_int_key(t, key) : -1; }
static int  MEX_h_GetRowAtStringKey(int h, struct string *key) { struct ex_table *t = handle_to_table(h); return t ? ex_row_at_string_key(t, key->text) : -1; }
static bool MEX_h_IA2Int(int h, int key, struct string *fn, int *d) { return hv_int(MainEXFile_IA2Handle(h, key, fn), d); }
static bool MEX_h_IA2Float(int h, int key, struct string *fn, float *d) { return hv_float(MainEXFile_IA2Handle(h, key, fn), d); }
static bool MEX_h_IA2String(int h, int key, struct string *fn, struct string **d) { return hv_string(MainEXFile_IA2Handle(h, key, fn), d); }
static bool MEX_h_SA2Int(int h, struct string *key, struct string *fn, int *d) { return hv_int(MainEXFile_SA2Handle(h, key, fn), d); }
static bool MEX_h_SA2Float(int h, struct string *key, struct string *fn, float *d) { return hv_float(MainEXFile_SA2Handle(h, key, fn), d); }
static bool MEX_h_SA2String(int h, struct string *key, struct string *fn, struct string **d) { return hv_string(MainEXFile_SA2Handle(h, key, fn), d); }
static bool MEX_h_RA2Int(int h, int row, struct string *fn, int *d) { return hv_int(MainEXFile_RA2Handle(h, row, fn), d); }
static bool MEX_h_RA2Float(int h, int row, struct string *fn, float *d) { return hv_float(MainEXFile_RA2Handle(h, row, fn), d); }
static bool MEX_h_RA2String(int h, int row, struct string *fn, struct string **d) { return hv_string(MainEXFile_RA2Handle(h, row, fn), d); }

/*
 * Default-return value accessors. Another string-key variant (e.g. Healing
 * Touch) takes the fallback value as an argument and returns the resolved value
 * directly, instead of the bool + ref-out convention of the family above. Only
 * the value accessors differ; Row/Col/Type/Exists match the string-key family
 * and are left in place.
 */
static int MEX_d_Int(struct string *name, int def, int id) { (void)id; int v; return hv_int(ex_key_handle(name), &v) ? v : def; }
static float MEX_d_Float(struct string *name, float def, int id) { (void)id; float v; return hv_float(ex_key_handle(name), &v) ? v : def; }
static struct string *MEX_d_String(struct string *name, struct string *def, int id) { (void)id; struct string *v = NULL; return hv_string(ex_key_handle(name), &v) ? v : string_ref(def); }
static int MEX_d_AInt(struct string *name, int index, int def, int id) { (void)id; int v; return hv_int(MainEXFile_AHandle(ex_key_handle(name), index), &v) ? v : def; }
static float MEX_d_AFloat(struct string *name, int index, float def, int id) { (void)id; float v; return hv_float(MainEXFile_AHandle(ex_key_handle(name), index), &v) ? v : def; }
static struct string *MEX_d_AString(struct string *name, int index, struct string *def, int id) { (void)id; struct string *v = NULL; return hv_string(MainEXFile_AHandle(ex_key_handle(name), index), &v) ? v : string_ref(def); }
static int MEX_d_A2Int(struct string *name, int row, int col, int def, int id) { (void)id; int v; return hv_int(MainEXFile_A2Handle(ex_key_handle(name), row, col), &v) ? v : def; }
static float MEX_d_A2Float(struct string *name, int row, int col, float def, int id) { (void)id; float v; return hv_float(MainEXFile_A2Handle(ex_key_handle(name), row, col), &v) ? v : def; }
static struct string *MEX_d_A2String(struct string *name, int row, int col, struct string *def, int id) { (void)id; struct string *v = NULL; return hv_string(MainEXFile_A2Handle(ex_key_handle(name), row, col), &v) ? v : string_ref(def); }

/*
 * Прямое чтение int-элемента list-значения главного .ex движковыми модулями
 * (既読-цвет окна сообщений: список «Ｅ＿既読メッセージ色»). Ключ — UTF-8,
 * внутри конвертируется в SJIS ключей .ex. false — .ex не загружен, ключа/
 * элемента нет или тип не int; *out при этом не трогается.
 */
bool mainex_list_int_get(const char *key_utf8, int index, int *out)
{
	if (!ex)
		return false;
	char *sjis = utf2sjis(key_utf8, strlen(key_utf8));
	struct ex_value *v = ex_get(ex, sjis);
	free(sjis);
	if (!v || v->type != EX_LIST)
		return false;
	struct ex_value *item = ex_list_get(v->list, index);
	if (!item || item->type != EX_INT)
		return false;
	*out = item->i;
	return true;
}

/*
 * Save/Load(wrap<array<int>> image) — снапшот ИЗМЕНЕНИЙ главного .ex в
 * сейв-образ (зовётся из gamesave::detail::セーブ実行/ロード復帰; без Save
 * сохранение падало фатальной «Unimplemented HLL function»). Наш движок EX не
 * модифицирует вовсе (EXWriter не реализован), поэтому честный снапшот —
 * «изменений нет»: Save пишет пустой образ с магией, Load принимает любой.
 * Формат наш собственный, с образом оригинального DLL совпадать не обязан
 * (сейвы читаются нашим же загрузчиком).
 */
static bool MainEXFile_Save(struct page **image)
{
	struct iarray_writer w;
	iarray_init_writer(&w, "MEX");
	iarray_write(&w, 0); // версия формата: «изменений нет»
	if (*image) {
		delete_page_vars(*image);
		free_page(*image);
	}
	*image = iarray_to_page(&w);
	iarray_free_writer(&w);
	return true;
}

static bool MainEXFile_Load(possibly_unused struct page **image)
{
	// Восстанавливать нечего: движок не накапливает изменений EX.
	return true;
}

static void MainEXFile_PreLink(void);

HLL_LIBRARY(MainEXFile,
	    HLL_EXPORT(_ModuleInit, MainEXFile_ModuleInit),
	    HLL_EXPORT(_ModuleFini, MainEXFile_ModuleFini),
	    HLL_EXPORT(_PreLink, MainEXFile_PreLink),
	    HLL_EXPORT(ReloadDebugEXFile, MainEXFile_ReloadDebugEXFile),
	    HLL_EXPORT(Save, MainEXFile_Save),
	    HLL_EXPORT(Load, MainEXFile_Load),
	    HLL_EXPORT(Handle, MainEXFile_Handle),
	    HLL_EXPORT(AHandle, MainEXFile_AHandle),
	    HLL_EXPORT(A2Handle, MainEXFile_A2Handle),
	    HLL_EXPORT(IA2Handle, MainEXFile_IA2Handle),
	    HLL_EXPORT(SA2Handle, MainEXFile_SA2Handle),
	    HLL_EXPORT(RA2Handle, MainEXFile_RA2Handle),
	    HLL_EXPORT(Row, MainEXFile_Row),
	    HLL_EXPORT(Col, MainEXFile_Col),
	    HLL_EXPORT(Type, MainEXFile_Type),
	    HLL_EXPORT(AType, MainEXFile_AType),
	    HLL_EXPORT(A2Type, MainEXFile_A2Type),
	    HLL_EXPORT(IA2Type, MainEXFile_IA2Type),
	    HLL_EXPORT(SA2Type, MainEXFile_SA2Type),
	    HLL_EXPORT(RA2Type, MainEXFile_RA2Type),
	    HLL_EXPORT(Exists, MainEXFile_Exists),
	    HLL_EXPORT(AExists, MainEXFile_AExists),
	    HLL_EXPORT(A2Exists, MainEXFile_A2Exists),
	    HLL_EXPORT(IA2Exists, MainEXFile_IA2Exists),
	    HLL_EXPORT(SA2Exists, MainEXFile_SA2Exists),
	    HLL_EXPORT(RA2Exists, MainEXFile_RA2Exists),
	    HLL_EXPORT(Int, MainEXFile_Int),
	    HLL_EXPORT(Float, MainEXFile_Float),
	    HLL_EXPORT(String, MainEXFile_String),
	    HLL_EXPORT(AInt, MainEXFile_AInt),
	    HLL_EXPORT(AFloat, MainEXFile_AFloat),
	    HLL_EXPORT(AString, MainEXFile_AString),
	    HLL_EXPORT(A2Int, MainEXFile_A2Int),
	    HLL_EXPORT(A2Float, MainEXFile_A2Float),
	    HLL_EXPORT(A2String, MainEXFile_A2String),
	    HLL_EXPORT(GetRowAtIntKey, MainEXFile_GetRowAtIntKey),
	    HLL_EXPORT(GetRowAtStringKey, MainEXFile_GetRowAtStringKey),
	    HLL_EXPORT(IA2Int, MainEXFile_IA2Int),
	    HLL_EXPORT(IA2Float, MainEXFile_IA2Float),
	    HLL_EXPORT(IA2String, MainEXFile_IA2String),
	    HLL_EXPORT(SA2Int, MainEXFile_SA2Int),
	    HLL_EXPORT(SA2Float, MainEXFile_SA2Float),
	    HLL_EXPORT(SA2String, MainEXFile_SA2String),
	    HLL_EXPORT(RA2Int, MainEXFile_RA2Int),
	    HLL_EXPORT(RA2Float, MainEXFile_RA2Float),
	    HLL_EXPORT(RA2String, MainEXFile_RA2String),
	    HLL_EXPORT(GetNodeNameCount, MainEXFile_GetNodeNameCount),
	    HLL_EXPORT(GetEXNameCount, MainEXFile_GetEXNameCount),
	    HLL_EXPORT(GetNodeName, MainEXFile_GetNodeName),
	    HLL_EXPORT(GetNodeNameList, MainEXFile_GetNodeNameList),
	    HLL_EXPORT(GetEXNameList, MainEXFile_GetEXNameList),
	    HLL_EXPORT(GetColAtFormatName, MainEXFile_GetColAtFormatName),
	    HLL_EXPORT(GetFormatNameList, MainEXFile_GetFormatNameList),
	    HLL_EXPORT(GetEXName, MainEXFile_GetEXName));

/*
 * Select the accessor interface based on the .ain's declared signatures. The
 * string-key family (path + exidx) is the default (registered above); when the
 * game instead uses the legacy handle-based signatures — detected by Int's
 * first argument being an int handle rather than a string — swap in the
 * handle-based family. The *Handle() resolvers and Handle() are identical in
 * both interfaces and are left in place.
 */
static void MainEXFile_PreLink(void)
{
	int libno = ain_get_library(ain, "MainEXFile");
	if (libno < 0)
		return;
	int fno = ain_get_library_function(ain, libno, "Int");
	if (fno < 0)
		return;
	struct ain_hll_function *fn = &ain->libraries[libno].functions[fno];
	if (fn->nr_arguments < 1)
		return;

	if (fn->arguments[0].type.data != AIN_INT) {
		// String-key interface. Two sub-variants share the same Row/Col/Type/
		// Exists metadata accessors (already registered) but differ in the value
		// accessors: the default is bool Int(name, ref int, id) (kept as-is);
		// when Int instead takes a plain-int fallback (int Int(name, def, id))
		// swap in the default-return value accessors.
		if (fn->nr_arguments >= 2 && fn->arguments[1].type.data == AIN_INT) {
			static_library_replace(&lib_MainEXFile, "Int", MEX_d_Int);
			static_library_replace(&lib_MainEXFile, "Float", MEX_d_Float);
			static_library_replace(&lib_MainEXFile, "String", MEX_d_String);
			static_library_replace(&lib_MainEXFile, "AInt", MEX_d_AInt);
			static_library_replace(&lib_MainEXFile, "AFloat", MEX_d_AFloat);
			static_library_replace(&lib_MainEXFile, "AString", MEX_d_AString);
			static_library_replace(&lib_MainEXFile, "A2Int", MEX_d_A2Int);
			static_library_replace(&lib_MainEXFile, "A2Float", MEX_d_A2Float);
			static_library_replace(&lib_MainEXFile, "A2String", MEX_d_A2String);
		}
		return;
	}

	// Handle-based interface (Int's first argument is an int handle).
	static_library_replace(&lib_MainEXFile, "Row", MEX_h_Row);
	static_library_replace(&lib_MainEXFile, "Col", MEX_h_Col);
	static_library_replace(&lib_MainEXFile, "Type", MEX_h_Type);
	static_library_replace(&lib_MainEXFile, "AType", MEX_h_AType);
	static_library_replace(&lib_MainEXFile, "A2Type", MEX_h_A2Type);
	static_library_replace(&lib_MainEXFile, "IA2Type", MEX_h_IA2Type);
	static_library_replace(&lib_MainEXFile, "SA2Type", MEX_h_SA2Type);
	static_library_replace(&lib_MainEXFile, "RA2Type", MEX_h_RA2Type);
	static_library_replace(&lib_MainEXFile, "Exists", MEX_h_Exists);
	static_library_replace(&lib_MainEXFile, "AExists", MEX_h_AExists);
	static_library_replace(&lib_MainEXFile, "A2Exists", MEX_h_A2Exists);
	static_library_replace(&lib_MainEXFile, "IA2Exists", MEX_h_IA2Exists);
	static_library_replace(&lib_MainEXFile, "SA2Exists", MEX_h_SA2Exists);
	static_library_replace(&lib_MainEXFile, "RA2Exists", MEX_h_RA2Exists);
	static_library_replace(&lib_MainEXFile, "Int", MEX_h_Int);
	static_library_replace(&lib_MainEXFile, "Float", MEX_h_Float);
	static_library_replace(&lib_MainEXFile, "String", MEX_h_String);
	static_library_replace(&lib_MainEXFile, "AInt", MEX_h_AInt);
	static_library_replace(&lib_MainEXFile, "AFloat", MEX_h_AFloat);
	static_library_replace(&lib_MainEXFile, "AString", MEX_h_AString);
	static_library_replace(&lib_MainEXFile, "A2Int", MEX_h_A2Int);
	static_library_replace(&lib_MainEXFile, "A2Float", MEX_h_A2Float);
	static_library_replace(&lib_MainEXFile, "A2String", MEX_h_A2String);
	static_library_replace(&lib_MainEXFile, "GetRowAtIntKey", MEX_h_GetRowAtIntKey);
	static_library_replace(&lib_MainEXFile, "GetRowAtStringKey", MEX_h_GetRowAtStringKey);
	static_library_replace(&lib_MainEXFile, "IA2Int", MEX_h_IA2Int);
	static_library_replace(&lib_MainEXFile, "IA2Float", MEX_h_IA2Float);
	static_library_replace(&lib_MainEXFile, "IA2String", MEX_h_IA2String);
	static_library_replace(&lib_MainEXFile, "SA2Int", MEX_h_SA2Int);
	static_library_replace(&lib_MainEXFile, "SA2Float", MEX_h_SA2Float);
	static_library_replace(&lib_MainEXFile, "SA2String", MEX_h_SA2String);
	static_library_replace(&lib_MainEXFile, "RA2Int", MEX_h_RA2Int);
	static_library_replace(&lib_MainEXFile, "RA2Float", MEX_h_RA2Float);
	static_library_replace(&lib_MainEXFile, "RA2String", MEX_h_RA2String);
}
