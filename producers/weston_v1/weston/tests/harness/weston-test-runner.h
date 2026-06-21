/*
 * Copyright © 2012 Intel Corporation
 * Copyright © 2013 Sam Spilsbury <smspillaz@gmail.com>
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

#ifndef _WESTON_TEST_RUNNER_H_
#define _WESTON_TEST_RUNNER_H_

#include "config.h"

#include <semaphore.h>
#include <stdlib.h>

#include <wayland-util.h>
#include "shared/helpers.h"
#include "weston-test-fixture-compositor.h"
#include "weston-testsuite-data.h"

/** Test harness context
 *
 * \ingroup testharness
 */
struct weston_test_harness;

typedef enum test_result_code (*weston_test_run_fn)(struct wet_testsuite_data *);
typedef enum test_result_code (*weston_test_run_arg_fn)(struct wet_testsuite_data *,
							const void *);
typedef enum test_result_code (*weston_test_run_plugin_fn)(struct wet_testsuite_data *,
							   struct weston_compositor *);

/** Test program entry
 *
 * Arrays of entries are created with macros \c TESTFN(), \c TESTFN_ARG()
 * and \c TESTFN_PLUGIN() inside a \c DECLARE_TEST_LIST() arguments.
 * The array of entries lists all test functions in a test program for the
 * test harness to iterate through.
 *
 * \ingroup testharness_private
 */
struct weston_test_entry {
	const char *name;
	union {
		weston_test_run_fn plain;
		weston_test_run_arg_fn arg;
		weston_test_run_plugin_fn plugin;
	} run;
	const void *table_data;
	size_t element_size;
	int n_elements;
};

/** Add a test function with no parameters
 *
 * This adds the given function into the list of tests. Must be used
 * inside the argument list of \c DECLARE_TEST_LIST() .
 *
 * This macro is not usable if fixture setup is using
 * \c weston_test_harness_execute_as_plugin().
 *
 * \param func The name of the function, will be also the name of the test.
 *
 * The function must have the signature:
 * \code
 *
 *   enum test_result_code func(struct wet_testsuite_data *)
 *
 * \endcode
 *
 * \ingroup testharness
 */
#define TESTFN(func) (struct weston_test_entry) {			\
	.name = #func,							\
	.run.plain = func,						\
	.n_elements = 1,						\
}

/** Add an array driven test with a parameter
 *
 * This adds the given function into the list of tests. Must be used
 * inside the argument list of \c DECLARE_TEST_LIST() . The function
 * will be executed once for each element in \c data_array, passing the
 * element as the second argument to the function.
 *
 * This macro is not usable if fixture setup is using
 * \c weston_test_harness_execute_as_plugin().
 *
 * \param func The name of the function, will be also the name of the test.
 *
 * The function must have the signature:
 * \code
 *
 *   enum test_result_code func(struct wet_testsuite_data *,
 *                              const usertype *)
 *
 * \endcode
 * \param data_array A static const array of \c usertype type. The length will be
 * recorded implicitly. \c usertype can be anything you want.
 *
 * \ingroup testharness
 */
#define TESTFN_ARG(func, data_array) (struct weston_test_entry) {	\
	.name = #func,							\
	.run.arg = (weston_test_run_arg_fn) func,			\
	.table_data = data_array,					\
	.element_size = sizeof (data_array[0]),				\
	.n_elements = ARRAY_LENGTH(data_array) +			\
			/* verify function argument type vs. array */	\
			(false && func(NULL, &data_array[0])),		\
}

/** Add a test with weston_compositor argument
 *
 * This adds the given function into the list of tests. Must be used
 * inside the argument list of \c DECLARE_TEST_LIST() .
 * The second argument to the function will be the compositor instance.
 *
 * This macro is only usable if fixture setup is using
 * \c weston_test_harness_execute_as_plugin().
 *
 * \param func The name of the function, will be also the name of the test.
 *
 * The function must have the signature:
 * \code
 *
 *   enum test_result_code func(struct wet_testsuite_data *,
 *                              struct weston_compositor *)
 *
 * \endcode
 *
 * \ingroup testharness
 */
#define TESTFN_PLUGIN(func) (struct weston_test_entry) {		\
	.name = #func,							\
	.run.plugin = func,						\
	.n_elements = 1,						\
}

struct weston_test_entry_list {
	const struct weston_test_entry *array;
	size_t len;
};

extern const struct weston_test_entry_list weston_test_entry_list_;

/** Declare the list of tests in a test program
 *
 * When the fixture setup is using \c weston_test_harness_execute_standalone()
 * or \c weston_test_harness_execute_as_client(), the argument list of
 * \c DECLARE_TEST_LIST() can contain \c TESTFN() and \c TESTFN_ARG().
 *
 * When the fixture setup is using \c weston_test_harness_execute_as_plugin(),
 * the argument list of \c DECLARE_TEST_LIST() can contain \c TESTFN_PLUGIN()
 * only.
 *
 * \ingroup testharness
 */
#define DECLARE_TEST_LIST(...)						\
	static const struct weston_test_entry test_list[] = {		\
		__VA_ARGS__						\
	};								\
	const struct weston_test_entry_list weston_test_entry_list_ = {	\
		.array = test_list,					\
		.len = ARRAY_LENGTH(test_list),				\
	}

void
testlog(const char *fmt, ...) WL_PRINTF(1, 2);

const char *
get_test_name(void);

int
get_test_fixture_index(void);

int
get_test_fixture_number_from_harness(struct weston_test_harness *harness);

/** Metadata for fixture setup array elements
 *
 * Every type used as a fixture setup array's elements needs one member of
 * this type, initialized.
 *
 * \sa DECLARE_FIXTURE_SETUP_WITH_ARG()
 *
 * \ingroup testharness
 */
struct fixture_metadata {
	/** Human friendly name of the fixture setup */
	const char *name;
};

/** Fixture setup array record
 *
 * Helper to store the attributes of the data array passed in to
 * DECLARE_FIXTURE_SETUP_WITH_ARG().
 *
 * \ingroup testharness_private
 */
struct fixture_setup_array {
	const void *array;
	size_t element_size;
	int n_elements;
	size_t meta_offset;
};

const struct fixture_setup_array *
fixture_setup_array_get_(void);

enum test_result_code
fixture_setup_run_(struct weston_test_harness *harness, const void *arg_);

/** Register a fixture setup function
 *
 * This registers the given (preferably static) function to be used for setting
 * up any fixtures you might need. The function must have the signature:
 *
 * \code
 * enum test_result_code func_(struct weston_test_harness *harness)
 * \endcode
 *
 * The function must call one of weston_test_harness_execute_standalone(),
 * weston_test_harness_execute_as_plugin() or
 * weston_test_harness_execute_as_client() passing in the \c harness argument,
 * and return the return value from that call. The function can also return a
 * test_result_code on its own if it does not want to run the tests,
 * e.g. RESULT_SKIP or RESULT_HARD_ERROR.
 *
 * The function will be called once to run all tests.
 *
 * \param func_ The function to be used as fixture setup.
 *
 * \ingroup testharness
 */
#define DECLARE_FIXTURE_SETUP(func_)					\
	enum test_result_code						\
	fixture_setup_run_(struct weston_test_harness *harness,		\
			   const void *arg_)				\
	{								\
		return func_(harness);					\
	}

/** Register a fixture setup function with a data array
 *
 * This registers the given (preferably static) function to be used for setting
 * up any fixtures you might need. The function must have the signature:
 *
 * \code
 * enum test_result_code func_(struct weston_test_harness *harness, typeof(array_[0]) *arg)
 * \endcode
 *
 * The function must call one of weston_test_harness_execute_standalone(),
 * weston_test_harness_execute_as_plugin() or
 * weston_test_harness_execute_as_client() passing in the \c harness argument,
 * and return the return value from that call. The function can also return a
 * test_result_code on its own if it does not want to run the tests,
 * e.g. RESULT_SKIP or RESULT_HARD_ERROR.
 *
 * The function will be called once with each element of the array pointed to
 * by \c arg, so that all tests would be repeated for each element in turn.
 *
 * \param func_ The function to be used as fixture setup.
 * \param array_ A static const array of arbitrary type.
 * \param meta_ Name of the field with type struct fixture_metadata.
 *
 * \ingroup testharness
 */
#define DECLARE_FIXTURE_SETUP_WITH_ARG(func_, array_, meta_)		\
	const struct fixture_setup_array *				\
	fixture_setup_array_get_(void)					\
	{								\
		static const struct fixture_setup_array arr = {		\
			.array = array_,				\
			.element_size = sizeof(array_[0]),		\
			.n_elements = ARRAY_LENGTH(array_),		\
			.meta_offset = offsetof(typeof(array_[0]), meta_),	\
		};								\
		TYPEVERIFY(const struct fixture_metadata *, &array_[0].meta_);	\
		return &arr;						\
	}								\
									\
	enum test_result_code						\
	fixture_setup_run_(struct weston_test_harness *harness,		\
			   const void *arg_)				\
	{								\
		typeof(array_[0]) *arg = arg_;				\
		return func_(harness, arg);				\
	}

enum test_result_code
weston_test_harness_execute_as_client(struct weston_test_harness *harness,
				      const struct compositor_setup *setup);

enum test_result_code
weston_test_harness_execute_as_plugin(struct weston_test_harness *harness,
				      const struct compositor_setup *setup);

enum test_result_code
weston_test_harness_execute_standalone(struct weston_test_harness *harness);

#endif
