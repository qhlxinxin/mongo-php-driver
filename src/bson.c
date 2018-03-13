/*
 * Copyright 2014-2017 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <bson.h>

#include <php.h>
#include <Zend/zend_hash.h>
#include <Zend/zend_interfaces.h>

#include "php_phongo.h"
#include "php_bson.h"
#include "phongo_compat.h"
#include "php_array_api.h"

#undef MONGOC_LOG_DOMAIN
#define MONGOC_LOG_DOMAIN "PHONGO-BSON"

#define PHONGO_IS_CLASS_INSTANTIATABLE(ce) \
	(!(ce->ce_flags & (ZEND_ACC_INTERFACE | ZEND_ACC_IMPLICIT_ABSTRACT_CLASS | ZEND_ACC_EXPLICIT_ABSTRACT_CLASS)))

#if PHP_VERSION_ID >= 70000
#define PHONGO_BSON_STATE_ZCHILD(state) (&((php_phongo_bson_state*) (state))->zchild)
#else
#define PHONGO_BSON_STATE_ZCHILD(state) (((php_phongo_bson_state*) (state))->zchild)
#endif

#define PHONGO_FIELD_PATH_EXPANSION 8

/* Forward declarations */
static bool php_phongo_bson_visit_document(const bson_iter_t* iter ARG_UNUSED, const char* key, const bson_t* v_document, void* data);
static bool php_phongo_bson_visit_array(const bson_iter_t* iter ARG_UNUSED, const char* key, const bson_t* v_document, void* data);

/* Path builder */
char* php_phongo_field_path_as_string(php_phongo_field_path* field_path)
{
	size_t length = 1; /* NULL character */
	size_t i;
	char*  path;
	char*  ptr;

	if (!field_path) {
		return estrdup("");
	}
	if (!field_path->elements) {
		return estrdup("");
	}

	for (i = 0; i <= field_path->current_level; i++) {
		if (!field_path->elements[i]) {
			continue;
		}
		length += (1 + strlen(field_path->elements[i]));
	}

	path = emalloc(length);
	ptr  = path;

	for (i = 0; i <= field_path->current_level; i++) {
		if (!field_path->elements[i]) {
			continue;
		}
		strcpy(ptr, field_path->elements[i]);
		ptr += strlen(field_path->elements[i]);
		ptr[0] = '.';
		ptr++;
	}
	ptr[-1] = '\0';

	return path;
}

php_phongo_field_path* php_phongo_field_path_alloc(void)
{
	php_phongo_field_path* tmp = ecalloc(1, sizeof(php_phongo_field_path));
	tmp->ref_count             = 1;

	return tmp;
}

void php_phongo_field_path_free(php_phongo_field_path* field_path)
{
	if (field_path->elements) {
		efree(field_path->elements);
	}
	if (field_path->element_types) {
		efree(field_path->element_types);
	}
	efree(field_path);
}

static void php_phongo_field_path_ensure_allocation(php_phongo_field_path* field_path, size_t level)
{
	if (level >= field_path->allocated_levels) {
		size_t i;

		field_path->allocated_levels = field_path->current_level + PHONGO_FIELD_PATH_EXPANSION;
		field_path->elements         = erealloc(field_path->elements, sizeof(char**) * field_path->allocated_levels);
		field_path->element_types    = erealloc(field_path->element_types, sizeof(php_phongo_bson_field_path_item_types*) * field_path->allocated_levels);

		for (i = level; i < field_path->allocated_levels; i++) {
			field_path->elements[i]      = NULL;
			field_path->element_types[i] = PHONGO_FIELD_PATH_ITEM_NONE;
		}
	}
}

void php_phongo_field_path_write_item_at_current_level(php_phongo_field_path* field_path, const char* element)
{
	php_phongo_field_path_ensure_allocation(field_path, field_path->current_level);

	field_path->elements[field_path->current_level] = element;
}

void php_phongo_field_path_write_type_at_current_level(php_phongo_field_path* field_path, php_phongo_bson_field_path_item_types element_type)
{
	php_phongo_field_path_ensure_allocation(field_path, field_path->current_level);

	field_path->element_types[field_path->current_level] = element_type;
}

bool php_phongo_field_path_push(php_phongo_field_path* field_path, const char* element, php_phongo_bson_field_path_item_types element_type)
{
	php_phongo_field_path_write_item_at_current_level(field_path, element);
	php_phongo_field_path_write_type_at_current_level(field_path, element_type);

	field_path->current_level++;

	return true;
}

bool php_phongo_field_path_pop(php_phongo_field_path* field_path)
{
	field_path->elements[field_path->current_level]      = NULL;
	field_path->element_types[field_path->current_level] = PHONGO_FIELD_PATH_ITEM_NONE;

	field_path->current_level--;

	field_path->elements[field_path->current_level]      = NULL;
	field_path->element_types[field_path->current_level] = PHONGO_FIELD_PATH_ITEM_NONE;

	return true;
}

inline static bool php_phongo_bson_state_is_initialized(php_phongo_bson_state* state)
{
	return state->field_path != NULL;
}

void php_phongo_bson_state_ctor(php_phongo_bson_state* state)
{
	state->field_path = php_phongo_field_path_alloc();
}

void php_phongo_bson_state_copy_ctor(php_phongo_bson_state* dst, php_phongo_bson_state* src)
{
	dst->map = src->map;
	if (src->field_path) {
		src->field_path->ref_count++;
	}
	dst->field_path = src->field_path;
}

void php_phongo_bson_state_dtor(php_phongo_bson_state* state)
{
	if (state->field_path) {
		state->field_path->ref_count--;

		if (state->field_path->ref_count < 1) {
			php_phongo_field_path_free(state->field_path);
		}
		state->field_path = NULL;
	}
} /* }}} */

static void php_phongo_bson_visit_corrupt(const bson_iter_t* iter ARG_UNUSED, void* data ARG_UNUSED) /* {{{ */
{
	mongoc_log(MONGOC_LOG_LEVEL_WARNING, MONGOC_LOG_DOMAIN, "Corrupt BSON data detected!");
} /* }}} */

static void php_phongo_bson_visit_unsupported_type(const bson_iter_t* iter ARG_UNUSED, const char* key, uint32_t v_type_code, void* data ARG_UNUSED) /* {{{ */
{
	php_phongo_bson_state* state = (php_phongo_bson_state*) data;
	char*                  path_string;
	TSRMLS_FETCH();

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);
	path_string = php_phongo_field_path_as_string(state->field_path);

	phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE TSRMLS_CC, "Detected unknown BSON type 0x%02hhx for field path \"%s\". Are you using the latest driver?", (unsigned char) v_type_code, path_string);

	efree(path_string);
} /* }}} */

static bool php_phongo_bson_visit_double(const bson_iter_t* iter ARG_UNUSED, const char* key, double v_double, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;

	if (state->is_visiting_array) {
		add_next_index_double(retval, v_double);
	} else {
		add_assoc_double(retval, key, v_double);
	}

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_utf8(const bson_iter_t* iter ARG_UNUSED, const char* key, size_t v_utf8_len, const char* v_utf8, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;

	if (state->is_visiting_array) {
		ADD_NEXT_INDEX_STRINGL(retval, v_utf8, v_utf8_len);
	} else {
		ADD_ASSOC_STRING_EX(retval, key, strlen(key), v_utf8, v_utf8_len);
	}

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_binary(const bson_iter_t* iter ARG_UNUSED, const char* key, bson_subtype_t v_subtype, size_t v_binary_len, const uint8_t* v_binary, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
	TSRMLS_FETCH();

	if (v_subtype == 0x80 && strcmp(key, PHONGO_ODM_FIELD_NAME) == 0) {
#if PHP_VERSION_ID >= 70000
		zend_string*      zs_classname = zend_string_init((const char*) v_binary, v_binary_len, 0);
		zend_class_entry* found_ce     = zend_fetch_class(zs_classname, ZEND_FETCH_CLASS_AUTO | ZEND_FETCH_CLASS_SILENT TSRMLS_CC);
		zend_string_release(zs_classname);
#else
		zend_class_entry* found_ce = zend_fetch_class((const char*) v_binary, v_binary_len, ZEND_FETCH_CLASS_AUTO | ZEND_FETCH_CLASS_SILENT TSRMLS_CC);
#endif

		if (found_ce && PHONGO_IS_CLASS_INSTANTIATABLE(found_ce) && instanceof_function(found_ce, php_phongo_persistable_ce TSRMLS_CC)) {
			((php_phongo_bson_state*) data)->odm = found_ce;
		}
	}

	{
#if PHP_VERSION_ID >= 70000
		zval zchild;

		php_phongo_new_binary_from_binary_and_type(&zchild, (const char*) v_binary, v_binary_len, v_subtype TSRMLS_CC);

		if (state->is_visiting_array) {
			add_next_index_zval(retval, &zchild);
		} else {
			ADD_ASSOC_ZVAL(retval, key, &zchild);
		}
#else  /* PHP_VERSION_ID >= 70000 */
		zval*             zchild   = NULL;

		MAKE_STD_ZVAL(zchild);
		php_phongo_new_binary_from_binary_and_type(zchild, (const char*) v_binary, v_binary_len, v_subtype TSRMLS_CC);

		if (state->is_visiting_array) {
			add_next_index_zval(retval, zchild);
		} else {
			ADD_ASSOC_ZVAL(retval, key, zchild);
		}
#endif /* PHP_VERSION_ID >= 70000 */
	}

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_undefined(const bson_iter_t* iter, const char* key, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000 /* PHP_VERSION_ID >= 70000 */
	zval zchild;

	object_init_ex(&zchild, php_phongo_undefined_ce);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	object_init_ex(zchild, php_phongo_undefined_ce);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_oid(const bson_iter_t* iter ARG_UNUSED, const char* key, const bson_oid_t* v_oid, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_objectid_new_from_oid(&zchild, v_oid TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_objectid_new_from_oid(zchild, v_oid TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_bool(const bson_iter_t* iter ARG_UNUSED, const char* key, bool v_bool, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;

	if (state->is_visiting_array) {
		add_next_index_bool(retval, v_bool);
	} else {
		add_assoc_bool(retval, key, v_bool);
	}

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_date_time(const bson_iter_t* iter ARG_UNUSED, const char* key, int64_t msec_since_epoch, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_utcdatetime_from_epoch(&zchild, msec_since_epoch TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_utcdatetime_from_epoch(zchild, msec_since_epoch TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_decimal128(const bson_iter_t* iter ARG_UNUSED, const char* key, const bson_decimal128_t* decimal, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_decimal128(&zchild, decimal TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_decimal128(zchild, decimal TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_null(const bson_iter_t* iter ARG_UNUSED, const char* key, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;

	if (state->is_visiting_array) {
		add_next_index_null(retval);
	} else {
		add_assoc_null(retval, key);
	}

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_regex(const bson_iter_t* iter ARG_UNUSED, const char* key, const char* v_regex, const char* v_options, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_regex_from_regex_and_options(&zchild, v_regex, v_options TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_regex_from_regex_and_options(zchild, v_regex, v_options TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_symbol(const bson_iter_t* iter, const char* key, size_t v_symbol_len, const char* v_symbol, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_symbol(&zchild, v_symbol, v_symbol_len TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_symbol(zchild, v_symbol, v_symbol_len TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_code(const bson_iter_t* iter ARG_UNUSED, const char* key, size_t v_code_len, const char* v_code, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_javascript_from_javascript(1, &zchild, v_code, v_code_len TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_javascript_from_javascript(1, zchild, v_code, v_code_len TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_dbpointer(const bson_iter_t* iter, const char* key, size_t namespace_len, const char* namespace, const bson_oid_t* oid, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_dbpointer(&zchild, namespace, namespace_len, oid TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_dbpointer(zchild, namespace, namespace_len, oid TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_codewscope(const bson_iter_t* iter ARG_UNUSED, const char* key, size_t v_code_len, const char* v_code, const bson_t* v_scope, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_javascript_from_javascript_and_scope(1, &zchild, v_code, v_code_len, v_scope TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_javascript_from_javascript_and_scope(1, zchild, v_code, v_code_len, v_scope TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_int32(const bson_iter_t* iter ARG_UNUSED, const char* key, int32_t v_int32, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;

	if (state->is_visiting_array) {
		add_next_index_long(retval, v_int32);
	} else {
		add_assoc_long(retval, key, v_int32);
	}

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_timestamp(const bson_iter_t* iter ARG_UNUSED, const char* key, uint32_t v_timestamp, uint32_t v_increment, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	php_phongo_new_timestamp_from_increment_and_timestamp(&zchild, v_increment, v_timestamp TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	php_phongo_new_timestamp_from_increment_and_timestamp(zchild, v_increment, v_timestamp TSRMLS_CC);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_int64(const bson_iter_t* iter ARG_UNUSED, const char* key, int64_t v_int64, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if SIZEOF_PHONGO_LONG == 4
	TSRMLS_FETCH();
#endif

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	if (state->is_visiting_array) {
		ADD_NEXT_INDEX_INT64(retval, v_int64);
	} else {
		ADD_ASSOC_INT64(retval, key, v_int64);
	}

	return false;
} /* }}} */

static bool php_phongo_bson_visit_maxkey(const bson_iter_t* iter ARG_UNUSED, const char* key, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	object_init_ex(&zchild, php_phongo_maxkey_ce);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	object_init_ex(zchild, php_phongo_maxkey_ce);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static bool php_phongo_bson_visit_minkey(const bson_iter_t* iter ARG_UNUSED, const char* key, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	php_phongo_bson_state* state  = (php_phongo_bson_state*) data;
#if PHP_VERSION_ID >= 70000
	zval zchild;

	object_init_ex(&zchild, php_phongo_minkey_ce);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, &zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, &zchild);
	}
#else  /* PHP_VERSION_ID >= 70000 */
	zval* zchild = NULL;
	TSRMLS_FETCH();

	MAKE_STD_ZVAL(zchild);
	object_init_ex(zchild, php_phongo_minkey_ce);

	if (state->is_visiting_array) {
		add_next_index_zval(retval, zchild);
	} else {
		ADD_ASSOC_ZVAL(retval, key, zchild);
	}
#endif /* PHP_VERSION_ID >= 70000 */

	php_phongo_field_path_write_item_at_current_level(state->field_path, key);

	return false;
} /* }}} */

static const bson_visitor_t php_bson_visitors = {
	NULL /* php_phongo_bson_visit_before*/,
	NULL /*php_phongo_bson_visit_after*/,
	php_phongo_bson_visit_corrupt,
	php_phongo_bson_visit_double,
	php_phongo_bson_visit_utf8,
	php_phongo_bson_visit_document,
	php_phongo_bson_visit_array,
	php_phongo_bson_visit_binary,
	php_phongo_bson_visit_undefined,
	php_phongo_bson_visit_oid,
	php_phongo_bson_visit_bool,
	php_phongo_bson_visit_date_time,
	php_phongo_bson_visit_null,
	php_phongo_bson_visit_regex,
	php_phongo_bson_visit_dbpointer,
	php_phongo_bson_visit_code,
	php_phongo_bson_visit_symbol,
	php_phongo_bson_visit_codewscope,
	php_phongo_bson_visit_int32,
	php_phongo_bson_visit_timestamp,
	php_phongo_bson_visit_int64,
	php_phongo_bson_visit_maxkey,
	php_phongo_bson_visit_minkey,
	php_phongo_bson_visit_unsupported_type,
	php_phongo_bson_visit_decimal128,
	{ NULL }
};

static bool php_phongo_bson_visit_document(const bson_iter_t* iter ARG_UNUSED, const char* key, const bson_t* v_document, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	bson_iter_t            child;
	php_phongo_bson_state* parent_state = (php_phongo_bson_state*) data;
	TSRMLS_FETCH();

	php_phongo_field_path_push(parent_state->field_path, key, PHONGO_FIELD_PATH_ITEM_DOCUMENT);

	if (bson_iter_init(&child, v_document)) {
		php_phongo_bson_state state = PHONGO_BSON_STATE_INITIALIZER;

		php_phongo_bson_state_copy_ctor(&state, parent_state);

#if PHP_VERSION_ID >= 70000
		array_init(&state.zchild);
#else
		MAKE_STD_ZVAL(state.zchild);
		array_init(state.zchild);
#endif

		if (!bson_iter_visit_all(&child, &php_bson_visitors, &state) && !child.err_off) {
			/* If php_phongo_bson_visit_binary() finds an ODM class, it should
			 * supersede a default type map and named document class. */
			if (state.odm && state.map.document_type == PHONGO_TYPEMAP_NONE) {
				state.map.document_type = PHONGO_TYPEMAP_CLASS;
			}

			switch (state.map.document_type) {
				case PHONGO_TYPEMAP_NATIVE_ARRAY:
#if PHP_VERSION_ID >= 70000
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, &state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, &state.zchild);
					}
#else  /* PHP_VERSION_ID >= 70000 */
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, state.zchild);
					}
#endif /* PHP_VERSION_ID >= 70000 */
					break;

				case PHONGO_TYPEMAP_CLASS: {
#if PHP_VERSION_ID >= 70000
					zval obj;

					object_init_ex(&obj, state.odm ? state.odm : state.map.document);
					zend_call_method_with_1_params(&obj, NULL, NULL, BSON_UNSERIALIZE_FUNC_NAME, NULL, &state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, &obj);
					} else {
						ADD_ASSOC_ZVAL(retval, key, &obj);
					}
					zval_ptr_dtor(&state.zchild);
#else  /* PHP_VERSION_ID >= 70000 */
					zval* obj = NULL;

					MAKE_STD_ZVAL(obj);
					object_init_ex(obj, state.odm ? state.odm : state.map.document);
					zend_call_method_with_1_params(&obj, NULL, NULL, BSON_UNSERIALIZE_FUNC_NAME, NULL, state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, obj);
					} else {
						ADD_ASSOC_ZVAL(retval, key, obj);
					}
					zval_ptr_dtor(&state.zchild);
#endif /* PHP_VERSION_ID >= 70000 */
					break;
				}

				case PHONGO_TYPEMAP_NATIVE_OBJECT:
				default:
#if PHP_VERSION_ID >= 70000
					convert_to_object(&state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, &state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, &state.zchild);
					}
#else  /* PHP_VERSION_ID >= 70000 */
					convert_to_object(state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, state.zchild);
					}
#endif /* PHP_VERSION_ID >= 70000 */
			}
		} else {
			/* Iteration stopped prematurely due to corruption or a failed
			 * visitor. Free state.zchild, which we just initialized, and return
			 * true to stop iteration for our parent context. */
			zval_ptr_dtor(&state.zchild);
			php_phongo_bson_state_dtor(&state);
			return true;
		}

		php_phongo_bson_state_dtor(&state);
		php_phongo_field_path_pop(parent_state->field_path);
	}

	return false;
} /* }}} */

static bool php_phongo_bson_visit_array(const bson_iter_t* iter ARG_UNUSED, const char* key, const bson_t* v_array, void* data) /* {{{ */
{
	zval*                  retval = PHONGO_BSON_STATE_ZCHILD(data);
	bson_iter_t            child;
	php_phongo_bson_state* parent_state = (php_phongo_bson_state*) data;
	TSRMLS_FETCH();

	php_phongo_field_path_push(parent_state->field_path, key, PHONGO_FIELD_PATH_ITEM_ARRAY);

	if (bson_iter_init(&child, v_array)) {
		php_phongo_bson_state state = PHONGO_BSON_STATE_INITIALIZER;

		php_phongo_bson_state_copy_ctor(&state, parent_state);

		/* Note that we are visiting an array, so element visitors know to use
		 * add_next_index() (i.e. disregard BSON keys) instead of add_assoc()
		 * when building the PHP array.
		 */
		state.is_visiting_array = true;

#if PHP_VERSION_ID >= 70000
		array_init(&state.zchild);
#else
		MAKE_STD_ZVAL(state.zchild);
		array_init(state.zchild);
#endif

		if (!bson_iter_visit_all(&child, &php_bson_visitors, &state) && !child.err_off) {

			switch (state.map.array_type) {
				case PHONGO_TYPEMAP_CLASS: {
#if PHP_VERSION_ID >= 70000
					zval obj;

					object_init_ex(&obj, state.map.array);
					zend_call_method_with_1_params(&obj, NULL, NULL, BSON_UNSERIALIZE_FUNC_NAME, NULL, &state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, &obj);
					} else {
						ADD_ASSOC_ZVAL(retval, key, &obj);
					}
					zval_ptr_dtor(&state.zchild);
#else  /* PHP_VERSION_ID >= 70000 */
					zval* obj = NULL;

					MAKE_STD_ZVAL(obj);
					object_init_ex(obj, state.map.array);
					zend_call_method_with_1_params(&obj, NULL, NULL, BSON_UNSERIALIZE_FUNC_NAME, NULL, state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, obj);
					} else {
						ADD_ASSOC_ZVAL(retval, key, obj);
					}
					zval_ptr_dtor(&state.zchild);
#endif /* PHP_VERSION_ID >= 70000 */
					break;
				}

				case PHONGO_TYPEMAP_NATIVE_OBJECT:
#if PHP_VERSION_ID >= 70000
					convert_to_object(&state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, &state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, &state.zchild);
					}
#else  /* PHP_VERSION_ID >= 70000 */
					convert_to_object(state.zchild);
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, state.zchild);
					}
#endif /* PHP_VERSION_ID >= 70000 */
					break;

				case PHONGO_TYPEMAP_NATIVE_ARRAY:
				default:
#if PHP_VERSION_ID >= 70000
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, &state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, &state.zchild);
					}
#else  /* PHP_VERSION_ID >= 70000 */
					if (((php_phongo_bson_state*) data)->is_visiting_array) {
						add_next_index_zval(retval, state.zchild);
					} else {
						ADD_ASSOC_ZVAL(retval, key, state.zchild);
					}
#endif /* PHP_VERSION_ID >= 70000 */
					break;
			}
		} else {
			/* Iteration stopped prematurely due to corruption or a failed
			 * visitor. Free state.zchild, which we just initialized, and return
			 * true to stop iteration for our parent context. */
			zval_ptr_dtor(&state.zchild);
			php_phongo_bson_state_dtor(&state);
			return true;
		}

		php_phongo_bson_state_dtor(&state);
		php_phongo_field_path_pop(parent_state->field_path);
	}

	return false;
} /* }}} */

/* Converts a BSON document to a PHP value using the default typemap. */
#if PHP_VERSION_ID >= 70000
bool php_phongo_bson_to_zval(const unsigned char* data, int data_len, zval* zv) /* {{{ */
#else
bool php_phongo_bson_to_zval(const unsigned char* data, int data_len, zval** zv)
#endif
{
	bool                  retval;
	php_phongo_bson_state state = PHONGO_BSON_STATE_INITIALIZER;

	retval = php_phongo_bson_to_zval_ex(data, data_len, &state);
#if PHP_VERSION_ID >= 70000
	ZVAL_ZVAL(zv, &state.zchild, 1, 1);
#else
	*zv = state.zchild;
#endif

	return retval;
} /* }}} */

/* Converts a BSON document to a PHP value according to the typemap specified in
 * the state argument.
 *
 * On success, the result will be set on the state argument and true will be
 * returned. On error, an exception will have been thrown and false will be
 * returned.
 *
 * Note: the result zval in the state argument will always be initialized for
 * PHP 5.x so that the caller may always zval_ptr_dtor() it. The zval is left
 * as-is on PHP 7; however, it should have the type undefined if the state
 * was initialized to zero.
 */
bool php_phongo_bson_to_zval_ex(const unsigned char* data, int data_len, php_phongo_bson_state* state) /* {{{ */
{
	bson_reader_t* reader = NULL;
	bson_iter_t    iter;
	const bson_t*  b;
	bool           eof             = false;
	bool           retval          = false;
	bool           must_dtor_state = false;
	TSRMLS_FETCH();

#if PHP_VERSION_ID < 70000
	MAKE_STD_ZVAL(state->zchild);

	/* Ensure that state->zchild has a type, since the calling code may want to
	 * zval_ptr_dtor() it if we throw an exception. */
	ZVAL_NULL(state->zchild);

#endif

	if (!php_phongo_bson_state_is_initialized(state)) {
		php_phongo_bson_state_ctor(state);
		must_dtor_state = true;
	}

	reader = bson_reader_new_from_data(data, data_len);

	if (!(b = bson_reader_read(reader, NULL))) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE TSRMLS_CC, "Could not read document from BSON reader");

		goto cleanup;
	}

	if (!bson_iter_init(&iter, b)) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE TSRMLS_CC, "Could not initialize BSON iterator");

		goto cleanup;
	}

	/* We initialize an array because it will either be returned as-is (native
	 * array in type map), passed to bsonUnserialize() (ODM class), or used to
	 * initialize a stdClass object (native object in type map). */
#if PHP_VERSION_ID >= 70000
	array_init(&state->zchild);
#else
	array_init(state->zchild);
#endif

	if (bson_iter_visit_all(&iter, &php_bson_visitors, state) || iter.err_off) {
		/* Iteration stopped prematurely due to corruption or a failed visitor.
		 * While we free the reader, state->zchild should be left as-is, since
		 * the calling code may want to zval_ptr_dtor() it. If an exception has
		 * been thrown already (due to an unsupported BSON type for example,
		 * don't overwrite with a generic exception message. */
		if (!EG(exception)) {
			char* path = php_phongo_field_path_as_string(state->field_path);
			phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE TSRMLS_CC, "Detected corrupt BSON data for field path '%s' at offset %d", path, iter.err_off);
			efree(path);
		}

		goto cleanup;
	}

	/* If php_phongo_bson_visit_binary() finds an ODM class, it should supersede
	 * a default type map and named root class. */
	if (state->odm && state->map.root_type == PHONGO_TYPEMAP_NONE) {
		state->map.root_type = PHONGO_TYPEMAP_CLASS;
	}

	switch (state->map.root_type) {
		case PHONGO_TYPEMAP_NATIVE_ARRAY:
			/* Nothing to do here */
			break;

		case PHONGO_TYPEMAP_CLASS: {
#if PHP_VERSION_ID >= 70000
			zval obj;

			object_init_ex(&obj, state->odm ? state->odm : state->map.root);
			zend_call_method_with_1_params(&obj, NULL, NULL, BSON_UNSERIALIZE_FUNC_NAME, NULL, &state->zchild);
			zval_ptr_dtor(&state->zchild);
			ZVAL_COPY_VALUE(&state->zchild, &obj);
#else  /* PHP_VERSION_ID >= 70000 */
			zval* obj = NULL;

			MAKE_STD_ZVAL(obj);
			object_init_ex(obj, state->odm ? state->odm : state->map.root);
			zend_call_method_with_1_params(&obj, NULL, NULL, BSON_UNSERIALIZE_FUNC_NAME, NULL, state->zchild);
			zval_ptr_dtor(&state->zchild);
			state->zchild = obj;
#endif /* PHP_VERSION_ID >= 70000 */

			break;
		}

		case PHONGO_TYPEMAP_NATIVE_OBJECT:
		default:
#if PHP_VERSION_ID >= 70000
			convert_to_object(&state->zchild);
#else
			convert_to_object(state->zchild);
#endif
	}

	if (bson_reader_read(reader, &eof) || !eof) {
		phongo_throw_exception(PHONGO_ERROR_UNEXPECTED_VALUE TSRMLS_CC, "Reading document did not exhaust input buffer");

		goto cleanup;
	}

	retval = true;

cleanup:
	if (reader) {
		bson_reader_destroy(reader);
	}
	if (must_dtor_state) {
		php_phongo_bson_state_dtor(state);
	}

	return retval;
} /* }}} */

/* Fetches a zend_class_entry for the given class name and checks that it is
 * also instantiatable and implements a specified interface. Returns the class
 * on success; otherwise, NULL is returned and an exception is thrown. */
static zend_class_entry* php_phongo_bson_state_fetch_class(const char* classname, int classname_len, zend_class_entry* interface_ce TSRMLS_DC) /* {{{ */
{
#if PHP_VERSION_ID >= 70000
	zend_string*      zs_classname = zend_string_init(classname, classname_len, 0);
	zend_class_entry* found_ce     = zend_fetch_class(zs_classname, ZEND_FETCH_CLASS_AUTO | ZEND_FETCH_CLASS_SILENT TSRMLS_CC);
	zend_string_release(zs_classname);
#else
	zend_class_entry* found_ce = zend_fetch_class(classname, classname_len, ZEND_FETCH_CLASS_AUTO | ZEND_FETCH_CLASS_SILENT TSRMLS_CC);
#endif

	if (!found_ce) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT TSRMLS_CC, "Class %s does not exist", classname);
	} else if (!PHONGO_IS_CLASS_INSTANTIATABLE(found_ce)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT TSRMLS_CC, "Class %s is not instantiatable", classname);
	} else if (!instanceof_function(found_ce, interface_ce TSRMLS_CC)) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT TSRMLS_CC, "Class %s does not implement %s", classname, ZSTR_VAL(interface_ce->name));
	} else {
		return found_ce;
	}

	return NULL;
} /* }}} */

/* Parses a BSON type (i.e. array, document, or root). On success, the type and
 * type_ce output arguments will be assigned and true will be returned;
 * otherwise, false is returned and an exception is thrown. */
static bool php_phongo_bson_state_parse_type(zval* options, const char* name, php_phongo_bson_typemap_types* type, zend_class_entry** type_ce TSRMLS_DC) /* {{{ */
{
	char*     classname;
	int       classname_len;
	zend_bool classname_free = 0;
	bool      retval         = true;

	classname = php_array_fetch_string(options, name, &classname_len, &classname_free);

	if (!classname_len) {
		goto cleanup;
	}

	if (!strcasecmp(classname, "array")) {
		*type    = PHONGO_TYPEMAP_NATIVE_ARRAY;
		*type_ce = NULL;
	} else if (!strcasecmp(classname, "stdclass") || !strcasecmp(classname, "object")) {
		*type    = PHONGO_TYPEMAP_NATIVE_OBJECT;
		*type_ce = NULL;
	} else {
		if ((*type_ce = php_phongo_bson_state_fetch_class(classname, classname_len, php_phongo_unserializable_ce TSRMLS_CC))) {
			*type = PHONGO_TYPEMAP_CLASS;
		} else {
			retval = false;
		}
	}

cleanup:
	if (classname_free) {
		str_efree(classname);
	}

	return retval;
} /* }}} */

/* Loops over each element in the fieldPaths array (if exists, and is an
 * array), and then checks whether each element is a valid type mapping */
bool php_phongo_bson_state_parse_fieldpaths(zval* typemap, php_phongo_bson_typemap* map TSRMLS_DC) /* {{{ */
{
	zval*      fieldpaths = NULL;
	HashTable* ht_data;

	if (!php_array_existsc(typemap, "fieldPaths")) {
		return true;
	}

	fieldpaths = php_array_fetchc_array(typemap, "fieldPaths");

	if (!fieldpaths) {
		phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT TSRMLS_CC, "The 'fieldPaths' element is not an array");
		return false;
	}

	ht_data = HASH_OF(fieldpaths);

#if PHP_VERSION_ID >= 70000
	{
		zend_string* string_key = NULL;
		zend_ulong   num_key    = 0;
		zval*        property;

		ZEND_HASH_FOREACH_KEY_VAL(ht_data, num_key, string_key, property)
		{
			zend_class_entry*             map_ce = NULL;
			php_phongo_bson_typemap_types map_type;

			if (!string_key) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT TSRMLS_CC, "The 'fieldPaths' element is not an associative array");
				return false;
			}

			if (!php_phongo_bson_state_parse_type(fieldpaths, ZSTR_VAL(string_key), &map_type, &map_ce TSRMLS_CC)) {
				return false;
			}
		}
		ZEND_HASH_FOREACH_END();
	}
#else
	{
		HashPosition pos;
		zval**       property;

		for (
			zend_hash_internal_pointer_reset_ex(ht_data, &pos);
			zend_hash_get_current_data_ex(ht_data, (void**) &property, &pos) == SUCCESS;
			zend_hash_move_forward_ex(ht_data, &pos)) {

			char*                         string_key     = NULL;
			uint                          string_key_len = 0;
			ulong                         num_key        = 0;
			zend_class_entry*             map_ce         = NULL;
			php_phongo_bson_typemap_types map_type;

			if (HASH_KEY_IS_STRING != zend_hash_get_current_key_ex(ht_data, &string_key, &string_key_len, &num_key, 0, &pos)) {
				phongo_throw_exception(PHONGO_ERROR_INVALID_ARGUMENT TSRMLS_CC, "The 'fieldPaths' element is not an associative array");
				return false;
			}

			if (!php_phongo_bson_state_parse_type(fieldpaths, string_key, &map_type, &map_ce TSRMLS_CC)) {
				return false;
			}
		}
	}
#endif /* PHP_VERSION_ID >= 70000 */
	return true;
} /* }}} */

/* Applies the array argument to a typemap struct. Returns true on success;
 * otherwise, false is returned an an exception is thrown. */
bool php_phongo_bson_typemap_to_state(zval* typemap, php_phongo_bson_typemap* map TSRMLS_DC) /* {{{ */
{
	if (!typemap) {
		return true;
	}

	if (!php_phongo_bson_state_parse_type(typemap, "array", &map->array_type, &map->array TSRMLS_CC) ||
		!php_phongo_bson_state_parse_type(typemap, "document", &map->document_type, &map->document TSRMLS_CC) ||
		!php_phongo_bson_state_parse_type(typemap, "root", &map->root_type, &map->root TSRMLS_CC) ||
		!php_phongo_bson_state_parse_fieldpaths(typemap, map TSRMLS_CC)) {

		/* Exception should already have been thrown */
		return false;
	}

	return true;
} /* }}} */

/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
