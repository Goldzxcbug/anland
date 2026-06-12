/*
 * Copyright © 2013 Intel Corporation
 * Copyright 2025 Collabora, Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial
 * portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT.  IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "config.h"

#include <stdio.h>

#include <libweston/config-parser.h>

#include "weston-test-client-helper.h"
#include "weston-test-assert.h"

static struct weston_config *
load_config(const char *text)
{
	struct weston_config *config = NULL;
	char *content = NULL;
	size_t file_len = 0;
	int write_len;
	FILE *file;

	file = open_memstream(&content, &file_len);
	test_assert_ptr_not_null(file);

	write_len = fwrite(text, 1, strlen(text), file);
	test_assert_int_eq((int)strlen(text), write_len);

	test_assert_int_eq(fflush(file), 0);
	fseek(file, 0L, SEEK_SET);

	config = weston_config_parse_fp(file);

	fclose(file);
	free(content);

	return config;
}

static struct weston_config *
assert_load_config(const char *text)
{
	struct weston_config *config = load_config(text);
	test_assert_ptr_not_null(config);

	return config;
}

static const char *comment_only_text =
	"# nothing in this file...\n";

static enum test_result_code
comment_only(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(comment_only_text);

	weston_config_destroy(config);

	return RESULT_OK;
}

/** @todo legit tests should have more descriptive names. */

static const char *legit_text =
	"# comment line here...\n"
	"\n"
	"[foo]\n"
	"a=b\n"
	"name=  Roy Batty    \n"
	"\n"
	"\n"
	"[bar]\n"
	"# more comments\n"
	"number=5252\n"
	"zero=0\n"
	"negative=-42\n"
	"flag=false\n"
	"real=4.667\n"
	"negreal=-3.2\n"
	"expval=24.687E+15\n"
	"negexpval=-3e-2\n"
	"notanumber=nan\n"
	"empty=\n"
	"tiny=0.0000000000000000000000000000000000000063548\n"
	"\n"
	"[colors]\n"
	"none=0x00000000\n"
	"low=0x11223344\n"
	"high=0xff00ff00\n"
	"oct=01234567\n"
	"dec=12345670\n"
	"short=1234567\n"
	"\n"
	"[stuff]\n"
	"flag=     true \n"
	"\n"
	"[bucket]\n"
	"color=blue \n"
	"contents=live crabs\n"
	"pinchy=true\n"
	"\n"
	"[bucket]\n"
	"material=plastic \n"
	"color=red\n"
	"contents=sand\n";

static enum test_result_code
legit_test01(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;

	section = weston_config_get_section(config,
					    "mollusc", NULL, NULL);
	test_assert_ptr_null(section);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test02(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	char *s;
	int r;

	section = weston_config_get_section(config, "foo", NULL, NULL);
	r = weston_config_section_get_string(section, "a", &s, NULL);

	test_assert_int_eq(0, r);
	test_assert_str_eq("b", s);

	free(s);
	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test03(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	char *s;
	int r;

	section = weston_config_get_section(config, "foo", NULL, NULL);
	r = weston_config_section_get_string(section, "b", &s, NULL);

	test_assert_int_eq(-1, r);
	test_assert_errno(ENOENT);
	test_assert_ptr_null(s);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test04(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	char *s;
	int r;

	section = weston_config_get_section(config, "foo", NULL, NULL);
	r = weston_config_section_get_string(section, "name", &s, NULL);

	test_assert_int_eq(0, r);
	test_assert_str_eq("Roy Batty", s);

	free(s);
	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test05(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	char *s;
	int r;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_string(section, "a", &s, "boo");

	test_assert_int_eq(-1, r);
	test_assert_errno(ENOENT);
	test_assert_str_eq("boo", s);

	free(s);
	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test06(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	int32_t n;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_int(section, "number", &n, 600);

	test_assert_int_eq(0, r);
	test_assert_s32_eq(5252, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test07(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	int32_t n;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_int(section, "+++", &n, 700);

	test_assert_int_eq(-1, r);
	test_assert_errno(ENOENT);
	test_assert_s32_eq(700, n);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test08(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t u;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_uint(section, "number", &u, 600);

	test_assert_int_eq(0, r);
	test_assert_u32_eq(5252, u);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test09(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t u;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_uint(section, "+++", &u, 600);

	test_assert_int_eq(-1, r);
	test_assert_errno(ENOENT);
	test_assert_u32_eq(600, u);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test10(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	bool b;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_bool(section, "flag", &b, true);

	test_assert_int_eq(0, r);
	test_assert_false(b);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test11(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	bool b;

	section = weston_config_get_section(config, "stuff", NULL, NULL);
	r = weston_config_section_get_bool(section, "flag", &b, false);

	test_assert_int_eq(0, r);
	test_assert_true(b);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test12(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	bool b;

	section = weston_config_get_section(config, "stuff", NULL, NULL);
	r = weston_config_section_get_bool(section, "bonk", &b, false);

	test_assert_int_eq(-1, r);
	test_assert_errno(ENOENT);
	test_assert_false(b);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test13(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	char *s;
	int r;

	section = weston_config_get_section(config,
					    "bucket", "color", "blue");
	r = weston_config_section_get_string(section, "contents", &s, NULL);

	test_assert_int_eq(0, r);
	test_assert_str_eq("live crabs", s);

	free(s);
	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test14(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	char *s;
	int r;

	section = weston_config_get_section(config,
					    "bucket", "color", "red");
	r = weston_config_section_get_string(section, "contents", &s, NULL);

	test_assert_int_eq(0, r);
	test_assert_str_eq("sand", s);

	free(s);
	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test15(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	char *s;
	int r;

	section = weston_config_get_section(config,
					    "bucket", "color", "pink");
	test_assert_ptr_null(section);
	r = weston_config_section_get_string(section, "contents", &s, "eels");

	test_assert_int_eq(-1, r);
	test_assert_errno(ENOENT);
	test_assert_str_eq("eels", s);

	free(s);
	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test16(struct wet_testsuite_data *suite_data)
{
	static const char *section_names[] = {
		"foo", "bar", "colors", "stuff", "bucket", "bucket"
	};
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	const char *name;
	int i;

	section = NULL;
	i = 0;
	while (weston_config_next_section(config, &section, &name))
		test_assert_str_eq(section_names[i++], name);

	test_assert_int_eq(6, i);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test17(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	int32_t n;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_int(section, "zero", &n, 600);

	test_assert_int_eq(0, r);
	test_assert_s32_eq(0, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test18(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_uint(section, "zero", &n, 600);

	test_assert_int_eq(0, r);
	test_assert_u32_eq(0, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test19(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	section = weston_config_get_section(config, "colors", NULL, NULL);
	r = weston_config_section_get_color(section, "none", &n, 0xff336699);

	test_assert_int_eq(0, r);
	test_assert_u32_eq(0x000000, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test20(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	section = weston_config_get_section(config, "colors", NULL, NULL);
	r = weston_config_section_get_color(section, "low", &n, 0xff336699);

	test_assert_int_eq(0, r);
	test_assert_u32_eq(0x11223344, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test21(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	section = weston_config_get_section(config, "colors", NULL, NULL);
	r = weston_config_section_get_color(section, "high", &n, 0xff336699);

	test_assert_int_eq(0, r);
	test_assert_u32_eq(0xff00ff00, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test22(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	/* Treat colors as hex values even if missing the leading 0x */
	section = weston_config_get_section(config, "colors", NULL, NULL);
	r = weston_config_section_get_color(section, "oct", &n, 0xff336699);

	test_assert_int_eq(0, r);
	test_assert_u32_eq(0x01234567, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test23(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	/* Treat colors as hex values even if missing the leading 0x */
	section = weston_config_get_section(config, "colors", NULL, NULL);
	r = weston_config_section_get_color(section, "dec", &n, 0xff336699);

	test_assert_int_eq(0, r);
	test_assert_u32_eq(0x12345670, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test24(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	/* 7-digit colors are not valid (most likely typos) */
	section = weston_config_get_section(config, "colors", NULL, NULL);
	r = weston_config_section_get_color(section, "short", &n, 0xff336699);

	test_assert_int_eq(-1, r);
	test_assert_u32_eq(0xff336699, n);
	test_assert_errno(EINVAL);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test25(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	/* String color names are unsupported */
	section = weston_config_get_section(config, "bucket", NULL, NULL);
	r = weston_config_section_get_color(section, "color", &n, 0xff336699);

	test_assert_int_eq(-1, r);
	test_assert_u32_eq(0xff336699, n);
	test_assert_errno(EINVAL);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test26(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	int32_t n;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_int(section, "negative", &n, 600);

	test_assert_int_eq(0, r);
	test_assert_s32_eq(-42, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
legit_test27(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	uint32_t n;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_uint(section, "negative", &n, 600);

	test_assert_int_eq(-1, r);
	test_assert_u32_eq(600, n);
	test_assert_errno(ERANGE);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_number(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "number", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(5252.0, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_missing(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "+++", &n, 600.0);

	test_assert_int_eq(-1, r);
	test_assert_f64_eq(600.0, n);
	test_assert_errno(ENOENT);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_zero(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "zero", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(n, 0.0);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_negative(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "negative", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(n, -42.0);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_flag(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "flag", &n, 600.0);

	test_assert_int_eq(-1, r);
	test_assert_f64_eq(n, 600.0);
	test_assert_errno(EINVAL);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_real(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "real", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(4.667, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_negreal(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "negreal", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(-3.2, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_expval(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "expval", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(24.687e+15, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_negexpval(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "negexpval", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(-3e-2, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_notanumber(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "notanumber", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_true(isnan(n));
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_empty(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "empty", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(0.0, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

static enum test_result_code
get_double_tiny(struct wet_testsuite_data *suite_data)
{
	struct weston_config *config = assert_load_config(legit_text);
	struct weston_config_section *section;
	int r;
	double n;

	errno = 0;
	section = weston_config_get_section(config, "bar", NULL, NULL);
	r = weston_config_section_get_double(section, "tiny", &n, 600.0);

	test_assert_int_eq(0, r);
	test_assert_f64_eq(6.3548e-39, n);
	test_assert_errno(0);

	weston_config_destroy(config);

	return RESULT_OK;
}

struct doesnt_parse_test { char *text; };

static const struct doesnt_parse_test doesnt_parse_test_data[] = {
	{
		"# invalid section...\n"
		"[this bracket isn't closed\n",
	}, {
		"# line without = ...\n"
		"[bambam]\n"
		"this line isn't any kind of valid\n",
	}, {
		"# starting with = ...\n"
		"[bambam]\n"
		"=not valid at all\n",
	},
};

static enum test_result_code
doesnt_parse(struct wet_testsuite_data *suite_data,
	     const struct doesnt_parse_test *test)
{
	struct weston_config *config = load_config(test->text);
	test_assert_ptr_null(config);

	return RESULT_OK;
}

static enum test_result_code
destroy_null(struct wet_testsuite_data *suite_data)
{
	weston_config_destroy(NULL);
	test_assert_int_eq(0, weston_config_next_section(NULL, NULL, NULL));

	return RESULT_OK;
}

static enum test_result_code
section_from_null(struct wet_testsuite_data *suite_data)
{
	struct weston_config_section *section;
	section = weston_config_get_section(NULL, "bucket", NULL, NULL);
	test_assert_ptr_null(section);

	return RESULT_OK;
}

static enum test_result_code
parse_comma_separated_list(struct wet_testsuite_data *suite_data)
{
	const char *matter;
	struct weston_string_array strarr;

	matter = "";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 0);

	matter = "   ";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 0);

	matter = " \t    \t \t";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 0);

	matter = "k";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 1);
	test_assert_str_eq(strarr.array[0], "k");
	weston_string_array_fini(&strarr);

	matter = " k";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 1);
	test_assert_str_eq(strarr.array[0], "k");
	weston_string_array_fini(&strarr);

	matter = "k ";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 1);
	test_assert_str_eq(strarr.array[0], "k");
	weston_string_array_fini(&strarr);

	matter = " k ";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 1);
	test_assert_str_eq(strarr.array[0], "k");
	weston_string_array_fini(&strarr);

	matter = "kissa kassi";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 2);
	test_assert_str_eq(strarr.array[0], "kissa");
	test_assert_str_eq(strarr.array[1], "kassi");
	weston_string_array_fini(&strarr);

	matter = "kissa\tkassi";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 2);
	test_assert_str_eq(strarr.array[0], "kissa");
	test_assert_str_eq(strarr.array[1], "kassi");
	weston_string_array_fini(&strarr);

	matter = "  kissa\t kassi";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 2);
	test_assert_str_eq(strarr.array[0], "kissa");
	test_assert_str_eq(strarr.array[1], "kassi");
	weston_string_array_fini(&strarr);

	matter = " 4.556\ra bab c \nkoe\t";
	strarr = weston_parse_space_separated_list(matter);
	test_assert_u64_eq(strarr.len, 5);
	test_assert_str_eq(strarr.array[0], "4.556");
	test_assert_str_eq(strarr.array[1], "a");
	test_assert_str_eq(strarr.array[2], "bab");
	test_assert_str_eq(strarr.array[3], "c");
	test_assert_str_eq(strarr.array[4], "koe");
	weston_string_array_fini(&strarr);

	return RESULT_OK;
}

DECLARE_TEST_LIST(
	TESTFN(comment_only),
	TESTFN(legit_test01),
	TESTFN(legit_test02),
	TESTFN(legit_test03),
	TESTFN(legit_test04),
	TESTFN(legit_test05),
	TESTFN(legit_test06),
	TESTFN(legit_test07),
	TESTFN(legit_test08),
	TESTFN(legit_test09),
	TESTFN(legit_test10),
	TESTFN(legit_test11),
	TESTFN(legit_test12),
	TESTFN(legit_test13),
	TESTFN(legit_test14),
	TESTFN(legit_test15),
	TESTFN(legit_test16),
	TESTFN(legit_test17),
	TESTFN(legit_test18),
	TESTFN(legit_test19),
	TESTFN(legit_test20),
	TESTFN(legit_test21),
	TESTFN(legit_test22),
	TESTFN(legit_test23),
	TESTFN(legit_test24),
	TESTFN(legit_test25),
	TESTFN(legit_test26),
	TESTFN(legit_test27),
	TESTFN(get_double_number),
	TESTFN(get_double_missing),
	TESTFN(get_double_zero),
	TESTFN(get_double_negative),
	TESTFN(get_double_flag),
	TESTFN(get_double_real),
	TESTFN(get_double_negreal),
	TESTFN(get_double_expval),
	TESTFN(get_double_negexpval),
	TESTFN(get_double_notanumber),
	TESTFN(get_double_empty),
	TESTFN(get_double_tiny),
	TESTFN_ARG(doesnt_parse, doesnt_parse_test_data),
	TESTFN(destroy_null),
	TESTFN(section_from_null),
	TESTFN(parse_comma_separated_list),
);
