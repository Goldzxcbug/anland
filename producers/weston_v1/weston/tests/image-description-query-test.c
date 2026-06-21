/*
 * Copyright 2023, 2026 Collabora, Ltd.
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

#include <unistd.h>

#include "color_util.h"
#include "color-manager-client.h"
#include "weston-test-client-helper.h"
#include "weston-test-fixture-compositor.h"
#include "weston-test-assert.h"

#include "color.h"
#include "color-properties.h"

struct profile_params {
	struct weston_color_profile_params template;

	/* Cannot statically initialize these in the template: */
	enum weston_transfer_function tf;
	enum weston_color_primaries named_prim;
	bool use_named_prim;
};

struct setup_args {
	struct fixture_metadata meta;

	/** Output ICC profile, otherwise set parametric profile */
	const char *icc_file;
	size_t icc_size;

	/** Parametric profile
	 *
	 * If icc_file, this matches the parametric surface preferred image description.
	 * If no icc_file, this matches all of output, surface preferred, and
	 * parametric surface preferred.
	 */
	struct profile_params param;
};

static const struct setup_args my_setup_args[] = {
	{
		.meta.name = "ICC file DisplayP3",
		.icc_file = "DisplayP3.icm",
		.icc_size = 2740,
		.param = {
			.template = {
				.primaries = prim_display_p3,
				.target_primaries = prim_display_p3,
				.min_luminance = 0.2f,
				.max_luminance = 80.f,
				.reference_white_luminance = 80.f,
				.target_min_luminance = 0.2f,
				.target_max_luminance = 80.f,
				.maxCLL = NO_VALUE,
				.maxFALL = NO_VALUE,
			},
			.tf = WESTON_TF_GAMMA22,
		},
	},
	{
		.meta.name = "sRGB power 2.3",
		.param = {
			.template = {
				.tf.params = { 2.3f, },
				.primaries = prim_bt709,
				.target_primaries = prim_bt709,
				.min_luminance = 0.2f,
				.max_luminance = 80.f,
				.reference_white_luminance = 80.f,
				.target_min_luminance = 0.2f,
				.target_max_luminance = 80.f,
				.maxCLL = NO_VALUE,
				.maxFALL = NO_VALUE,
			},
			.tf = WESTON_TF_POWER,
			.named_prim = WESTON_PRIMARIES_CICP_SRGB,
			.use_named_prim = true,
		},
	},
	{
		.meta.name = "BT.2020 SDR with P3 target",
		.param = {
			.template = {
				.primaries = prim_bt2020,
				.target_primaries = prim_display_p3,
				.min_luminance = 0.2f,
				.max_luminance = 80.f,
				.reference_white_luminance = 80.f,
				.target_min_luminance = 0.2f,
				.target_max_luminance = 80.f,
				.maxCLL = NO_VALUE,
				.maxFALL = NO_VALUE,
			},
			.tf = WESTON_TF_GAMMA22,
			.named_prim = WESTON_PRIMARIES_CICP_BT2020,
			.use_named_prim = true,
		},
	},
	{
		.meta.name = "BT.2100 PQ with HP target",
		.param = {
			.template = {
				.primaries = prim_bt2020,
				.target_primaries = prim_hp_5dq99aa,
				.min_luminance = 0.0f,
				.max_luminance = 10000.f,
				.reference_white_luminance = 270.f,
				.target_min_luminance = 0.005f,
				.target_max_luminance = 1200.f,
				.maxCLL = 1150.f,
				.maxFALL = 600.f,
			},
			.tf = WESTON_TF_ST2084_PQ,
		},
	},
};

static struct weston_color_profile_params
params_from_template(const struct profile_params *tmp)
{
	struct weston_color_profile_params ret = tmp->template;

	ret.tf.info = weston_color_tf_info_from(NULL, tmp->tf);
	if (tmp->use_named_prim)
		ret.primaries_info = weston_color_primaries_info_from(NULL, tmp->named_prim);

	return ret;
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *args)
{
	struct compositor_setup setup;
	char *conf;
	char *sect;

	compositor_setup_defaults(&setup);
	setup.refresh = HIGHEST_OUTPUT_REFRESH;

	if (args->icc_file) {
		conf = cfgln("icc_profile=%s/%s\n", reference_path(), args->icc_file);
		sect = NULL;
	} else {
		struct weston_color_profile_params params;

		params = params_from_template(&args->param);

		conf = cfgln("color-profile=mydisp");
		sect = cfg_color_profile_params("mydisp", &params);
	}

	weston_ini_setup(&setup,
			 cfgln("[core]"),
			 cfgln("color-management=true"),
			 cfgln("[output]"),
			 cfgln("name=headless"),
			 conf,
			 sect);

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, my_setup_args, meta);

static bool
test_assert_CIExy_eq(const struct weston_CIExy *ref,
		     const struct weston_CIExy *tst,
		     float tolerance,
		     int indent,
		     const char *desc)
{
	bool r = true;

	r = test_assert_f32_absdiff_lt(ref->x, tst->x, tolerance) && r;
	r = test_assert_f32_absdiff_lt(ref->y, tst->y, tolerance) && r;

	if (!r)
		testlog("%*sin %s\n", indent, "", desc);

	return r;
}

static bool
test_assert_color_gamut_eq(const struct weston_color_gamut *ref,
			   const struct weston_color_gamut *tst,
			   float tolerance,
			   int indent,
			   const char *desc)
{
	static const char *chan[] = { "red", "green", "blue" };
	bool r = true;
	unsigned i;

	for (i = 0; i < ARRAY_LENGTH(tst->primary); i++) {
		r = test_assert_CIExy_eq(&ref->primary[i], &tst->primary[i],
					 tolerance, indent + 2, chan[i]) && r;
	}

	r = test_assert_CIExy_eq(&ref->white_point, &tst->white_point,
				 tolerance, indent + 2, "white point") && r;

	if (!r)
		testlog("%*sin %s\n", indent, "", desc);

	return r;
}

static void
assert_params_equal(const struct weston_color_profile_params *ref,
		    const struct image_description_info *tst)
{
	float tol = 0.0001;
	int indent = 4;

	test_assert_color_gamut_eq(&tst->primaries, &ref->primaries, tol, indent, "primaries");
	if (ref->primaries_info)
		test_assert_enum(tst->primaries_named, ref->primaries_info->protocol_primaries);

	test_assert_enum(tst->tf_named, ref->tf.info->protocol_tf);
	if (ref->tf.info->tf == WESTON_TF_POWER) {
		test_assert_bit_set(tst->events_received, (1u << IMAGE_DESCR_INFO_EVENT_TF_POWER_EXP));
		test_assert_f32_absdiff_lt(ref->tf.params[0], tst->tf_power, tol);
	}

	test_assert_f32_absdiff_lt(ref->min_luminance, tst->min_lum, tol);
	test_assert_f32_absdiff_lt(ref->max_luminance, tst->max_lum, tol);
	test_assert_f32_absdiff_lt(ref->reference_white_luminance, tst->ref_lum, tol);

	test_assert_color_gamut_eq(&ref->target_primaries, &tst->target_primaries,
				   tol, indent, "target primaries");

	test_assert_f32_absdiff_lt(ref->target_min_luminance, tst->target_min_lum, tol);
	test_assert_f32_absdiff_lt(ref->target_max_luminance, tst->target_max_lum, tol);
	if (ref->maxCLL != NO_VALUE) {
		test_assert_bit_set(tst->events_received, (1u << IMAGE_DESCR_INFO_EVENT_TARGET_MAXCLL));
		test_assert_f32_absdiff_lt(ref->maxCLL, tst->target_max_cll, tol);
	}
	if (ref->maxFALL != NO_VALUE) {
		test_assert_bit_set(tst->events_received, (1u << IMAGE_DESCR_INFO_EVENT_TARGET_MAXFALL));
		test_assert_f32_absdiff_lt(ref->maxFALL, tst->target_max_fall, tol);
	}
}

static void
verify_image_description(const struct image_description_info *info,
			 const struct setup_args *arg)
{
	struct weston_color_profile_params params;

	if (arg->icc_file) {
		off_t size;

		/* Let's pretend checking the size is enough. */
		size = lseek(info->icc_fd, 0, SEEK_END);
		test_assert_s64_eq(size, info->icc_size);
		test_assert_s64_eq(arg->icc_size, info->icc_size);
	} else {
		params = params_from_template(&arg->param);
		assert_params_equal(&params, info);
	}
}

static enum test_result_code
output_get_image_description(struct wet_testsuite_data *suite_data)
{
	const struct setup_args *arg = &my_setup_args[get_test_fixture_index()];
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct image_description_info *info;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = client_get_color_manager(client, 1);

	/* Get image description from output */
	image_descr = image_description_create_for_output(cm, client->output);
	image_description_wait_until_ready(client, image_descr);

	/* Get output image description information */
	info = image_description_get_information(client, image_descr);
	verify_image_description(info, arg);

	image_description_info_destroy(info);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
surface_get_preferred_image_description(struct wet_testsuite_data *suite_data)
{
	const struct setup_args *arg = &my_setup_args[get_test_fixture_index()];
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct image_description_info *info;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = client_get_color_manager(client, 1);

	/* Get preferred image description from surface */
	image_descr = image_description_create_for_preferred(cm, client->surface);
	image_description_wait_until_ready(client, image_descr);

	/* Get surface image description information */
	info = image_description_get_information(client, image_descr);
	verify_image_description(info, arg);

	image_description_info_destroy(info);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

DECLARE_TEST_LIST(
	TESTFN(output_get_image_description),
	TESTFN(surface_get_preferred_image_description),
);
