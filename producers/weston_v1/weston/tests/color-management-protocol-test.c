/*
 * Copyright 2024 Collabora, Ltd.
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

#include <fcntl.h>
#include <sys/stat.h>

#include "weston-test-client-helper.h"
#include "weston-test-assert.h"
#include "shared/xalloc.h"

#include "color-manager-client.h"
#include "lcms_util.h"

static char srgb_icc_profile_path[500] = "\0";

const struct lcms_pipeline pipeline_sRGB = {
	.color_space = "sRGB",
	.prim_output = {
		.Red =   { 0.640, 0.330, 1.0 },
		.Green = { 0.300, 0.600, 1.0 },
		.Blue =  { 0.150, 0.060, 1.0 }
	},
	.pre_fn = TRANSFER_FN_SRGB,
	.mat = WESTON_MAT3F_IDENTITY,
	.post_fn = TRANSFER_FN_SRGB_INVERSE
};

static void
build_sRGB_icc_profile(const char *filename)
{
	cmsHPROFILE profile;
	double vcgt_exponents[COLOR_CHAN_NUM] = { 0.0 };
	bool saved;

	profile = build_lcms_matrix_shaper_profile_output(NULL, &pipeline_sRGB,
							  vcgt_exponents);
	test_assert_ptr_not_null(profile);

	saved = cmsSaveProfileToFile(profile, filename);
	test_assert_true(saved);

	cmsCloseProfile(profile);
}

static struct color_manager_client *
color_manager_get(struct client *client)
{
	struct color_manager_client *cm = client_get_color_manager(client, 1);

	/* Weston supports all color features. */
	test_assert_u32_eq(cm->supported_features,
			   (1 << WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4) |
			   (1 << WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC) |
			   (1 << WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES) |
			   (1 << WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES) |
			   (1 << WP_COLOR_MANAGER_V1_FEATURE_SET_TF_POWER) |
			   (1 << WP_COLOR_MANAGER_V1_FEATURE_SET_MASTERING_DISPLAY_PRIMARIES) |
			   (1 << WP_COLOR_MANAGER_V1_FEATURE_EXTENDED_TARGET_VOLUME));

	/* Weston supports all rendering intents. */
	test_assert_u32_eq(cm->supported_rendering_intents,
			   (1 << WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL) |
			   (1 << WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE) |
			/* (1 << WP_COLOR_MANAGER_V1_RENDER_INTENT_SATURATION) | */
			   (1 << WP_COLOR_MANAGER_V1_RENDER_INTENT_ABSOLUTE) |
			   (1 << WP_COLOR_MANAGER_V1_RENDER_INTENT_RELATIVE_BPC));

	/* Weston supports all primaries. */
	test_assert_u32_eq(cm->supported_primaries,
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_SRGB) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_PAL_M) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_PAL) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_NTSC) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_GENERIC_FILM) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_CIE1931_XYZ) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_DCI_P3) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_DISPLAY_P3) |
			   (1 << WP_COLOR_MANAGER_V1_PRIMARIES_ADOBE_RGB));

	/* Weston supports only a few transfer functions, and we make use of
	 * them in our tests. */
	test_assert_u32_eq(cm->supported_tf,
			   (1 << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22) |
			   (1 << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA28) |
			   (1 << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR) |
			   (1 << WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ));

	test_assert_true(cm->init_done);

	return cm;
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.shell = SHELL_TEST_DESKTOP;
	setup.refresh = HIGHEST_OUTPUT_REFRESH; /* create_client_and_test_surface() */
	setup.logging_scopes = "log,color-lcms-profiles";

	/* Create the sRGB ICC profile. We do that only once for this test
	 * program. */
	if (strlen(srgb_icc_profile_path) == 0) {
		char *tmp;

		tmp = output_filename_for_test_program(THIS_TEST_NAME,
						       NULL, "icm");
		test_assert_int_lt(strlen(tmp), ARRAY_LENGTH(srgb_icc_profile_path));
		strcpy(srgb_icc_profile_path, tmp);
		free(tmp);

		build_sRGB_icc_profile(srgb_icc_profile_path);
	}

	weston_ini_setup(&setup,
			 cfgln("[core]"),
			 cfgln("color-management=true"));

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);


static enum test_result_code
create_image_description_before_setting_icc_file(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_icc_v1 *image_descr_creator_icc;
	struct wp_image_description_v1 *image_desc;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	image_descr_creator_icc =
		wp_color_manager_v1_create_icc_creator(cm->manager_proxy);

	/* Try creating image description based on ICC profile but without
	 * setting the ICC file, what should fail.
	 *
	 * We expect a protocol error from unknown object, because the
	 * image_descr_creator_icc wl_proxy will get destroyed with the create
	 * call below. It is a destructor request. */
	image_desc = wp_image_description_creator_icc_v1_create(image_descr_creator_icc);
	expect_protocol_error(client, NULL,
			      WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_INCOMPLETE_SET);

	wp_image_description_v1_destroy(image_desc);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_unreadable_icc_fd(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_icc_v1 *image_descr_creator_icc;
	int32_t icc_fd;
	struct stat st;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	image_descr_creator_icc =
		wp_color_manager_v1_create_icc_creator(cm->manager_proxy);

	/* The file is being open with WRITE, not READ permission. So the
	 * compositor should complain. */
	icc_fd = open(srgb_icc_profile_path, O_WRONLY);
	test_assert_s32_ge(icc_fd, 0);
	test_assert_int_eq(fstat(icc_fd, &st), 0);

	/* Try setting the bad ICC file fd, it should fail. */
	wp_image_description_creator_icc_v1_set_icc_file(image_descr_creator_icc,
							 icc_fd, 0, st.st_size);
	expect_protocol_error(client, &wp_image_description_creator_icc_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_BAD_FD);

	close(icc_fd);
	wp_image_description_creator_icc_v1_destroy(image_descr_creator_icc);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_too_short_icc_fd(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_icc_v1 *image_descr_creator_icc;
	struct wp_image_description_v1 *proxy;
	int32_t icc_fd;
	struct stat st;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	image_descr_creator_icc =
		wp_color_manager_v1_create_icc_creator(cm->manager_proxy);

	icc_fd = open(srgb_icc_profile_path, O_RDONLY);
	test_assert_s32_ge(icc_fd, 0);
	test_assert_int_eq(fstat(icc_fd, &st), 0);

	/* Claim the ICC file has 10 more bytes than it actually does. */
	wp_image_description_creator_icc_v1_set_icc_file(image_descr_creator_icc,
							 icc_fd, 0, st.st_size + 10);
	client_roundtrip(client);

	proxy = wp_image_description_creator_icc_v1_create(image_descr_creator_icc);
	expect_protocol_error(client, NULL,
			      WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_OUT_OF_FILE);

	close(icc_fd);
	wp_image_description_v1_destroy(proxy);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_bad_icc_size_zero(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_icc_v1 *image_descr_creator_icc;
	int32_t icc_fd;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	image_descr_creator_icc =
		wp_color_manager_v1_create_icc_creator(cm->manager_proxy);

	icc_fd = open(srgb_icc_profile_path, O_RDONLY);
	test_assert_s32_ge(icc_fd, 0);

	/* Try setting ICC file with a bad size, it should fail. */
	wp_image_description_creator_icc_v1_set_icc_file(image_descr_creator_icc,
							 icc_fd, 0, 0);
	expect_protocol_error(client, &wp_image_description_creator_icc_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_BAD_SIZE);

	close(icc_fd);
	wp_image_description_creator_icc_v1_destroy(image_descr_creator_icc);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_bad_icc_non_seekable(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_icc_v1 *image_descr_creator_icc;
	int32_t fds[2];

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	image_descr_creator_icc =
		wp_color_manager_v1_create_icc_creator(cm->manager_proxy);

	/* We need a non-seekable file, and pipes are non-seekable. */
	test_assert_int_ge(pipe(fds), 0);

	/* Pretend that it has a valid size of 1024 bytes. That still should
	 * fail because the fd is non-seekable. */
	wp_image_description_creator_icc_v1_set_icc_file(image_descr_creator_icc,
							 fds[0], 0, 1024);
	expect_protocol_error(client, &wp_image_description_creator_icc_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_BAD_FD);

	close(fds[0]);
	close(fds[1]);
	wp_image_description_creator_icc_v1_destroy(image_descr_creator_icc);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_icc_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_icc_v1 *image_descr_creator_icc;
	int32_t icc_fd;
	struct stat st;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	image_descr_creator_icc =
		wp_color_manager_v1_create_icc_creator(cm->manager_proxy);

	icc_fd = open(srgb_icc_profile_path, O_RDONLY);
	test_assert_s32_ge(icc_fd, 0);
	test_assert_int_eq(fstat(icc_fd, &st), 0);

	wp_image_description_creator_icc_v1_set_icc_file(image_descr_creator_icc,
							 icc_fd, 0, st.st_size);
	client_roundtrip(client);

	/* Set the ICC again, what should fail. */
	wp_image_description_creator_icc_v1_set_icc_file(image_descr_creator_icc,
							 icc_fd, 0, st.st_size);
	expect_protocol_error(client, &wp_image_description_creator_icc_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_ICC_V1_ERROR_ALREADY_SET);

	close(icc_fd);
	wp_image_description_creator_icc_v1_destroy(image_descr_creator_icc);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
create_icc_image_description_no_info(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct wp_image_description_info_v1 *proxy;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	/* Create image description based on ICC profile */
	image_descr = image_description_create_for_icc(cm, srgb_icc_profile_path);
	image_description_wait_until_ready(client, image_descr);

	/* Get image description information, and that should fail. Images
	 * descriptions that we create do not accept this request. */
	proxy = wp_image_description_v1_get_information(image_descr->proxy);
	expect_protocol_error(client, &wp_image_description_v1_interface,
			      WP_IMAGE_DESCRIPTION_V1_ERROR_NO_INFORMATION);

	wp_image_description_info_v1_destroy(proxy);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
create_image_description_soft_fail(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	enum image_description_status status;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	image_descr = image_description_create_soft_fail(cm);
	status = image_description_wait(client, image_descr);
	test_assert_enum(status, CM_IMAGE_DESC_FAILED);

	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_failed_image_description(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct wp_color_management_surface_v1 *cm_surface;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);

	image_descr = image_description_create_soft_fail(cm);
	wp_color_management_surface_v1_set_image_description(cm_surface,
							     image_descr->proxy,
							     WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	expect_protocol_error(client, &wp_color_management_surface_v1_interface,
			      WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_IMAGE_DESCRIPTION);

	wp_color_management_surface_v1_destroy(cm_surface);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
failed_image_description_get_info(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct wp_image_description_info_v1 *proxy;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	/*
	 * Create a soft-failed image description. Getting its information
	 * must fail.
	 */
	image_descr = image_description_create_soft_fail(cm);

	proxy = wp_image_description_v1_get_information(image_descr->proxy);
	expect_protocol_error(client, &wp_image_description_v1_interface,
			      WP_IMAGE_DESCRIPTION_V1_ERROR_NOT_READY);

	wp_image_description_info_v1_destroy(proxy);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
get_surface_twice_bad(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_color_management_surface_v1 *cm_surface[2];

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	cm_surface[0] = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);
	cm_surface[1] = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);

	expect_protocol_error(client, &wp_color_manager_v1_interface,
			      WP_COLOR_MANAGER_V1_ERROR_SURFACE_EXISTS);

	wp_color_management_surface_v1_destroy(cm_surface[1]);
	wp_color_management_surface_v1_destroy(cm_surface[0]);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
get_surface_twice_good(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_color_management_surface_v1 *cm_surface;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);
	wp_color_management_surface_v1_destroy(cm_surface);
	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);
	wp_color_management_surface_v1_destroy(cm_surface);
	client_roundtrip(client);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_surface_image_description(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct wp_color_management_surface_v1 *cm_surface;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);

	/* Create image description based on ICC profile */
	image_descr = image_description_create_for_icc(cm, srgb_icc_profile_path);
	image_description_wait_until_ready(client, image_descr);

	/* Set surface image description */
	wp_color_management_surface_v1_set_image_description(cm_surface,
							     image_descr->proxy,
							     WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	wl_surface_attach(client->surface->wl_surface, client->surface->buffer->proxy, 0, 0);
	wl_surface_damage(client->surface->wl_surface, 0, 0, 1000, 1000);
	wl_surface_commit(client->surface->wl_surface);
	client_roundtrip(client);

	wp_color_management_surface_v1_destroy(cm_surface);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_inert_surface_image_description(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct wp_color_management_surface_v1 *cm_surface;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	/* Get guaranteed good image description. */
	image_descr = image_description_create_for_preferred(cm, client->surface);
	image_description_wait_until_ready(client, image_descr);

	/* Destroy the wl_surface, making cm_surface inert. */
	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);
	surface_destroy(client->surface);
	client->surface = NULL;

	/* Set image description on inert surface. */
	wp_color_management_surface_v1_set_image_description(cm_surface,
							     image_descr->proxy,
							     WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	expect_protocol_error(client, &wp_color_management_surface_v1_interface,
			      WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT);

	wp_color_management_surface_v1_destroy(cm_surface);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_bad_rendering_intent(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct image_description *image_descr;
	struct wp_color_management_surface_v1 *cm_surface;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	/* Get guaranteed good image description. */
	image_descr = image_description_create_for_preferred(cm, client->surface);
	image_description_wait_until_ready(client, image_descr);

	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);

	wp_color_management_surface_v1_set_image_description(cm_surface,
							     image_descr->proxy,
							     9999);
	expect_protocol_error(client, &wp_color_management_surface_v1_interface,
			      WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_RENDER_INTENT);

	wp_color_management_surface_v1_destroy(cm_surface);
	image_description_destroy(image_descr);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
inert_get_preferred_image_description(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_v1 *proxy;
	struct wp_color_management_surface_feedback_v1 *cm_feedback;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	cm_feedback = wp_color_manager_v1_get_surface_feedback(cm->manager_proxy,
							       client->surface->wl_surface);

	surface_destroy(client->surface);
	client->surface = NULL;

	proxy = wp_color_management_surface_feedback_v1_get_preferred(cm_feedback);
	expect_protocol_error(client, &wp_color_management_surface_feedback_v1_interface,
			      WP_COLOR_MANAGEMENT_SURFACE_FEEDBACK_V1_ERROR_INERT);

	wp_image_description_v1_destroy(proxy);
	wp_color_management_surface_feedback_v1_destroy(cm_feedback);
	client_destroy(client);

	return RESULT_OK;
}

#define NOT_SET -99
#define BAD_ENUM 99999

/**
 * This is used to know where to expect the error in the test.
 */
enum error_point {
	ERROR_POINT_NONE = 0,
	ERROR_POINT_PRIMARIES_NAMED,
	ERROR_POINT_PRIMARIES,
	ERROR_POINT_TF_NAMED,
	ERROR_POINT_TF_POWER,
	ERROR_POINT_PRIMARIES_LUM,
	ERROR_POINT_TARGET_LUM,
	ERROR_POINT_IMAGE_DESC,
	ERROR_POINT_GRACEFUL_FAILURE,
};

struct parametric_case {
	int32_t primaries_named;
	const struct weston_color_gamut *primaries;
	int32_t tf_named;
	float tf_power;
	float primaries_min_lum;
	int32_t primaries_max_lum;
	int32_t primaries_ref_lum;
	const struct weston_color_gamut *target_primaries;
	float target_min_lum;
	int32_t target_max_lum;
	int32_t target_max_cll;
	int32_t target_max_fall;
	int32_t expected_error;
	enum error_point error_point;
};

static const struct weston_color_gamut color_gamut_sRGB = {
	.primary = { { 0.64, 0.33 }, /* RGB order */
		     { 0.30, 0.60 },
		     { 0.15, 0.06 },
	},
	.white_point = { 0.3127, 0.3290 },
};

static const struct weston_color_gamut color_gamut_invalid_primaries = {
	.primary = { { -100.00, 0.33 }, /* RGB order */
		     {    0.30, 0.60 },
		     {    0.15, 0.06 },
	},
	.white_point = { 0.3127, 0.3290 },
};

static const struct weston_color_gamut color_gamut_invalid_white_point = {
	.primary = { { 0.64, 0.33 }, /* RGB order */
		     { 0.30, 0.60 },
		     { 0.15, 0.06 },
	},
	.white_point = { 1.0, 1.0 },
};

static const struct parametric_case parametric_cases[] = {

	/******** Successful cases *******/

	{
	  /* sRGB primaries with gamma22; succeeds. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* Custom primaries with gamma22; succeeds. */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_sRGB,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* sRGB primaries, gamma22 and valid luminance values; succeeds. */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_sRGB,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = 0.5,
	  .primaries_max_lum = 2000,
	  .primaries_ref_lum = 300,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* sRGB primaries with custom power-law TF; succeeds. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = NOT_SET,
	  .tf_power = 2.4f,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* sRGB primaries, gamma22 and valid target primaries; succeeds. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = &color_gamut_sRGB,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* sRGB primaries, PQ TF and valid target luminance; succeeds. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 2.0f,
	  .target_max_lum = 3,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* sRGB primaries, PQ TF and valid max cll; succeeds. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = 5,
	  .target_max_fall = NOT_SET,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* sRGB primaries, PQ TF and valid max fall; succeeds. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = 5,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},
	{
	  /* sRGB primaries, PQ TF and valid target luminance, max fall and
	   * max cll; succeeds. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 1.0f,
	  .target_max_lum = 3,
	  .target_max_cll = 2,
	  .target_max_fall = 2,
	  .expected_error = NOT_SET,
	  .error_point = ERROR_POINT_NONE,
	},

	/************ Failing cases  *************/

	{
	  /* Invalid named primaries; protocol error. */
	  .primaries_named = BAD_ENUM,
	  .expected_error = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_PRIMARIES_NAMED,
	  .error_point = ERROR_POINT_PRIMARIES_NAMED,
	},
	{
	  /* Invalid TF named; protocol error. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = BAD_ENUM,
	  .expected_error = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
	  .error_point = ERROR_POINT_TF_NAMED,
	},
	{
	  /* Invalid power-law TF exponent (0.9 < 1.0, which is the minimum);
	   * protocol error. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = NOT_SET,
	  .tf_power = 0.9f,
	  .expected_error = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_TF,
	  .error_point = ERROR_POINT_TF_POWER,
	},
	{
	  /* Invalid luminance (ref white < min lum); protocol error. */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_sRGB,
	  .tf_named = NOT_SET,
	  .tf_power = 5.0f,
	  .primaries_min_lum = 50.0,
	  .primaries_max_lum = 100,
	  .primaries_ref_lum = 49,
	  .expected_error = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
	  .error_point = ERROR_POINT_PRIMARIES_LUM,
	},
	{
	  /* Invalid target luminance (min_lum == max_lum); protocol error. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 5.0f,
	  .target_max_lum = 5,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
	  .error_point = ERROR_POINT_TARGET_LUM,
	},
	{
	  /* Invalid max cll (max cll < min target luminance);
	   * protocol error. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 6.0f,
	  .target_max_lum = 7,
	  .target_max_cll = 5,
	  .target_max_fall = NOT_SET,
	  .expected_error = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
	  .error_point = ERROR_POINT_IMAGE_DESC,
	},
	{
	  /* Invalid max fall (max fall < min target luminance);
	   * protocol error. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = 6.0f,
	  .target_max_lum = 7,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = 5,
	  .expected_error = WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_INVALID_LUMINANCE,
	  .error_point = ERROR_POINT_IMAGE_DESC,
	},
	{
	  /* Invalid custom primaries (CIE XY value out of compositor defined
	   * range); graceful failure. */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_invalid_primaries,
	  .tf_named = NOT_SET,
	  .tf_power = 5.0f,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* Invalid custom primaries (white point out of color gamut);
	   * graceful failure. */
	  .primaries_named = NOT_SET,
	  .primaries = &color_gamut_invalid_white_point,
	  .tf_named = NOT_SET,
	  .tf_power = 5.0f,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = NULL,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* Invalid custom target primaries (CIE XY value out of compositor
	   * defined range); graceful failure. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = &color_gamut_invalid_primaries,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
	{
	  /* Invalid custom target primaries (white point out of color gamut);
	   * graceful failure. */
	  .primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
	  .primaries = NULL,
	  .tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
	  .tf_power = NOT_SET,
	  .primaries_min_lum = NOT_SET,
	  .primaries_max_lum = NOT_SET,
	  .primaries_ref_lum = NOT_SET,
	  .target_primaries = &color_gamut_invalid_white_point,
	  .target_min_lum = NOT_SET,
	  .target_max_lum = NOT_SET,
	  .target_max_cll = NOT_SET,
	  .target_max_fall = NOT_SET,
	  .expected_error = WP_IMAGE_DESCRIPTION_V1_CAUSE_UNSUPPORTED,
	  .error_point = ERROR_POINT_GRACEFUL_FAILURE,
	},
};

static enum test_result_code
create_parametric_image_description(struct wet_testsuite_data *suite_data,
				    const struct parametric_case *args)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param = NULL;
	struct image_description *image_desc = NULL;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);

	if (args->primaries_named != NOT_SET)
		wp_image_description_creator_params_v1_set_primaries_named(image_desc_creator_param,
									   args->primaries_named);
	if (args->error_point == ERROR_POINT_PRIMARIES_NAMED) {
		expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
				      args->expected_error);
		goto out;
	}

	if (args->primaries)
		param_creator_set_primaries(image_desc_creator_param, args->primaries);
	if (args->error_point == ERROR_POINT_PRIMARIES) {
		expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
				      args->expected_error);
		goto out;
	}

	if (args->tf_named != NOT_SET)
		wp_image_description_creator_params_v1_set_tf_named(image_desc_creator_param,
								    args->tf_named);
	if (args->error_point == ERROR_POINT_TF_NAMED) {
		expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
				      args->expected_error);
		goto out;
	}

	if (args->tf_power != NOT_SET)
		wp_image_description_creator_params_v1_set_tf_power(image_desc_creator_param,
								    args->tf_power * 10000);
	if (args->error_point == ERROR_POINT_TF_POWER) {
		expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
				      args->expected_error);
		goto out;
	}

	if (args->primaries_min_lum != NOT_SET && args->primaries_max_lum != NOT_SET &&
	    args->primaries_ref_lum != NOT_SET)
		wp_image_description_creator_params_v1_set_luminances(image_desc_creator_param,
								      args->primaries_min_lum * 10000,
								      args->primaries_max_lum,
								      args->primaries_ref_lum);

	if (args->error_point == ERROR_POINT_PRIMARIES_LUM) {
		expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
				      args->expected_error);
		goto out;
	}

	if (args->target_primaries)
		param_creator_set_mastering_display_primaries(image_desc_creator_param,
							      args->target_primaries);
	/*
	 * The only possible failure for set_mastering_display() is ALREADY_SET,
	 * but we test that in set_target_primaries_twice().
	 * So we don't have ERROR_POINT_MASTERING_DISPLAY_PRIMARIES.
	 */

	if (args->target_min_lum != NOT_SET && args->target_max_lum != NOT_SET)
		wp_image_description_creator_params_v1_set_mastering_luminance(image_desc_creator_param,
									       args->target_min_lum * 10000,
									       args->target_max_lum);
	if (args->error_point == ERROR_POINT_TARGET_LUM) {
		expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
				      args->expected_error);
		goto out;
	}

	if (args->target_max_cll != NOT_SET)
		wp_image_description_creator_params_v1_set_max_cll(image_desc_creator_param,
								   args->target_max_cll);
	/*
	 * The only possible failure for set_max_cll() is ALREADY_SET,
	 * but we test that in set_cll_twice().
	 * So we don't have ERROR_POINT_TARGET_MAX_CLL.
	 */

	if (args->target_max_fall != NOT_SET)
		wp_image_description_creator_params_v1_set_max_fall(image_desc_creator_param,
								    args->target_max_fall);
	/*
	 * The only possible failure for set_max_fall() is ALREADY_SET,
	 * but we test that in set_max_fall_twice().
	 * So we don't have ERROR_POINT_TARGET_MAX_FALL.
	 */

	image_desc = image_description_from_param(image_desc_creator_param);
	image_desc_creator_param = NULL;
	if (args->error_point == ERROR_POINT_IMAGE_DESC) {
		/* We expect a protocol error from unknown object, because the
		 * image_desc_creator_param wl_proxy will get destroyed with
		 * the create call above. It is a destructor request. */
		expect_protocol_error(client, NULL, args->expected_error);
		goto out;
	}

	while (image_desc->status == CM_IMAGE_DESC_NOT_CREATED)
		if (!test_assert_int_ge(wl_display_dispatch(client->wl_display), 0))
			return RESULT_FAIL;

	if (args->error_point == ERROR_POINT_NONE) {
		test_assert_enum(args->expected_error, NOT_SET);
		test_assert_enum(image_desc->status, CM_IMAGE_DESC_READY);
	} else {
		test_assert_enum(args->error_point, ERROR_POINT_GRACEFUL_FAILURE);
		test_assert_enum(image_desc->status, CM_IMAGE_DESC_FAILED);
		test_assert_enum(image_desc->failure_reason, args->expected_error);
	}

out:
	if (image_desc)
		image_description_destroy(image_desc);
	if (image_desc_creator_param)
		wp_image_description_creator_params_v1_destroy(image_desc_creator_param);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_primaries_named_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_primaries_named(image_desc_creator_param,
								   WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_primaries_named(image_desc_creator_param,
								   WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_primaries_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	param_creator_set_primaries(image_desc_creator_param, &color_gamut_sRGB);
	client_roundtrip(client); /* make sure connection is still valid */
	param_creator_set_primaries(image_desc_creator_param, &color_gamut_sRGB);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_primaries_then_primaries_named(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	param_creator_set_primaries(image_desc_creator_param, &color_gamut_sRGB);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_primaries_named(image_desc_creator_param,
								   WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_primaries_named_then_primaries(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_primaries_named(image_desc_creator_param,
								   WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
	client_roundtrip(client); /* make sure connection is still valid */
	param_creator_set_primaries(image_desc_creator_param, &color_gamut_sRGB);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_tf_power_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_tf_power(image_desc_creator_param,
							    2.4 * 10000);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_tf_power(image_desc_creator_param,
							    2.4 * 10000);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_tf_named_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_tf_named(image_desc_creator_param,
							    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_tf_named(image_desc_creator_param,
							    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_tf_power_then_tf_named(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_tf_power(image_desc_creator_param,
							    2.4 * 10000);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_tf_named(image_desc_creator_param,
							    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_tf_named_then_tf_power(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_tf_named(image_desc_creator_param,
							    WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_tf_power(image_desc_creator_param,
							    2.4 * 10000);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_luminance_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;
	float min_lum = 0.5;
	float max_lum = 2000.0;
	float ref_lum = 300.0;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_luminances(image_desc_creator_param,
							      min_lum * 10000,
							      max_lum,
							      ref_lum);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_luminances(image_desc_creator_param,
							      min_lum * 10000,
							      max_lum,
							      ref_lum);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_target_primaries_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	param_creator_set_mastering_display_primaries(image_desc_creator_param, &color_gamut_sRGB);
	client_roundtrip(client); /* make sure connection is still valid */
	param_creator_set_mastering_display_primaries(image_desc_creator_param, &color_gamut_sRGB);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_target_luminance_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;
	float target_min_lum = 2.0f;
	float target_max_lum = 3.0f;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_mastering_luminance(image_desc_creator_param,
								       target_min_lum * 10000,
								       target_max_lum);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_mastering_luminance(image_desc_creator_param,
								       target_min_lum * 10000,
								       target_max_lum);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_max_cll_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_max_cll(image_desc_creator_param, 5.0f);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_max_cll(image_desc_creator_param, 5.0f);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
set_max_fall_twice(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_image_description_creator_params_v1 *image_desc_creator_param;

	client = create_client();
	cm = color_manager_get(client);

	image_desc_creator_param = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_max_fall(image_desc_creator_param, 5.0f);
	client_roundtrip(client); /* make sure connection is still valid */
	wp_image_description_creator_params_v1_set_max_fall(image_desc_creator_param, 5.0f);
	expect_protocol_error(client, &wp_image_description_creator_params_v1_interface,
			      WP_IMAGE_DESCRIPTION_CREATOR_PARAMS_V1_ERROR_ALREADY_SET);
	wp_image_description_creator_params_v1_destroy(image_desc_creator_param);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
unset_image_description(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_color_management_surface_v1 *cm_surface;
	struct wp_image_description_creator_params_v1 *creator;
	struct image_description *image_desc;

	client = create_client_and_test_surface(0, 0, 100, 100);
	cm = color_manager_get(client);

	/* Create a simple image description. */
	creator = color_manager_create_param(cm);
	wp_image_description_creator_params_v1_set_primaries_named(creator, WP_COLOR_MANAGER_V1_PRIMARIES_SRGB);
	wp_image_description_creator_params_v1_set_tf_named(creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22);
	image_desc = image_description_from_param(creator);
	creator = NULL;
	image_description_wait_until_ready(client, image_desc);

	/* Set it on the surface so we get something to unset. */
	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);
	wp_color_management_surface_v1_set_image_description(cm_surface,
							     image_desc->proxy,
							     WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	wl_surface_commit(client->surface->wl_surface);
	client_roundtrip(client);

	/* Explicit unset */
	wp_color_management_surface_v1_unset_image_description(cm_surface);
	wl_surface_commit(client->surface->wl_surface);
	client_roundtrip(client);

	/* Set it again on the surface. */
	wp_color_management_surface_v1_set_image_description(cm_surface,
							     image_desc->proxy,
							     WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	wl_surface_commit(client->surface->wl_surface);
	client_roundtrip(client);

	/* Implicit unset */
	wp_color_management_surface_v1_destroy(cm_surface);
	wl_surface_commit(client->surface->wl_surface);
	client_roundtrip(client);

	image_description_destroy(image_desc);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
unset_inert_surface_image_description(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct color_manager_client *cm;
	struct wp_color_management_surface_v1 *cm_surface;

	client = create_client_and_test_surface(100, 100, 100, 100);
	cm = color_manager_get(client);

	/* Destroy the wl_surface, making cm_surface inert. */
	cm_surface = wp_color_manager_v1_get_surface(cm->manager_proxy, client->surface->wl_surface);
	surface_destroy(client->surface);
	client->surface = NULL;

	/* Unset image description on inert surface. */
	wp_color_management_surface_v1_unset_image_description(cm_surface);
	expect_protocol_error(client, &wp_color_management_surface_v1_interface,
			      WP_COLOR_MANAGEMENT_SURFACE_V1_ERROR_INERT);

	wp_color_management_surface_v1_destroy(cm_surface);
	client_destroy(client);

	return RESULT_OK;
}

DECLARE_TEST_LIST(
	TESTFN(create_image_description_before_setting_icc_file),
	TESTFN(set_unreadable_icc_fd),
	TESTFN(set_too_short_icc_fd),
	TESTFN(set_bad_icc_size_zero),
	TESTFN(set_bad_icc_non_seekable),
	TESTFN(set_icc_twice),
	TESTFN(create_icc_image_description_no_info),
	TESTFN(create_image_description_soft_fail),
	TESTFN(set_failed_image_description),
	TESTFN(failed_image_description_get_info),
	TESTFN(get_surface_twice_bad),
	TESTFN(get_surface_twice_good),
	TESTFN(set_surface_image_description),
	TESTFN(set_inert_surface_image_description),
	TESTFN(set_bad_rendering_intent),
	TESTFN(inert_get_preferred_image_description),
	TESTFN_ARG(create_parametric_image_description, parametric_cases),
	TESTFN(set_primaries_named_twice),
	TESTFN(set_primaries_twice),
	TESTFN(set_primaries_then_primaries_named),
	TESTFN(set_primaries_named_then_primaries),
	TESTFN(set_tf_power_twice),
	TESTFN(set_tf_named_twice),
	TESTFN(set_tf_power_then_tf_named),
	TESTFN(set_tf_named_then_tf_power),
	TESTFN(set_luminance_twice),
	TESTFN(set_target_primaries_twice),
	TESTFN(set_target_luminance_twice),
	TESTFN(set_max_cll_twice),
	TESTFN(set_max_fall_twice),
	TESTFN(unset_image_description),
	TESTFN(unset_inert_surface_image_description),
);
