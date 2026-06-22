/*
 * Copyright (C) 2026 Amazon.com, Inc. or its affiliates
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

#include "weston-test-client-helper.h"
#include "weston-test-assert.h"


static struct wp_alpha_modifier_surface_v1 *
get_alpha_modifier_surface(struct client *client, struct wl_surface *surface)
{
	struct wp_alpha_modifier_surface_v1 *ams;

	test_assert_ptr_not_null(client->alpha_modifier);
	ams = wp_alpha_modifier_v1_get_surface(client->alpha_modifier, surface);

	return ams;
}

static enum test_result_code
fixture_setup(struct weston_test_harness *harness)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.shell = SHELL_TEST_DESKTOP;
	setup.refresh = HIGHEST_OUTPUT_REFRESH;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP(fixture_setup);

static enum test_result_code
recreate_succeeds(struct wet_testsuite_data *data)
{
	struct client *client;
	struct wp_alpha_modifier_surface_v1 *ams;

	client = create_client_and_test_surface(100, 50, 100, 100);

	ams = get_alpha_modifier_surface(client, client->surface->wl_surface);

	/* Test set multiplier. */
	wp_alpha_modifier_surface_v1_set_multiplier(ams, UINT32_MAX / 2);
	wl_surface_commit(client->surface->wl_surface);

	/* Destroy ams, so alpha modifier is reset to opaque. */
	wp_alpha_modifier_surface_v1_destroy(ams);
	wl_surface_commit(client->surface->wl_surface);

	/* After destruction and commit, creating a new ams should succeed. */
	ams = get_alpha_modifier_surface(client, client->surface->wl_surface);

	client_roundtrip(client);

	wp_alpha_modifier_surface_v1_destroy(ams);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
already_constructed_error(struct wet_testsuite_data *data)
{
	struct client *client;
	struct wp_alpha_modifier_surface_v1 *ams_A, *ams_B;

	client = create_client_and_test_surface(100, 50, 100, 100);

	ams_A = get_alpha_modifier_surface(client, client->surface->wl_surface);
	ams_B = get_alpha_modifier_surface(client, client->surface->wl_surface);
	expect_protocol_error(client, &wp_alpha_modifier_v1_interface,
			      WP_ALPHA_MODIFIER_V1_ERROR_ALREADY_CONSTRUCTED);

	wp_alpha_modifier_surface_v1_destroy(ams_A);
	wp_alpha_modifier_surface_v1_destroy(ams_B);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
surface_gone_before_destroy_request_error(struct wet_testsuite_data *data)
{
	struct client *client;
	struct wp_alpha_modifier_surface_v1 *ams;

	client = create_client_and_test_surface(100, 50, 100, 100);

	ams = get_alpha_modifier_surface(client, client->surface->wl_surface);

	wl_surface_destroy(client->surface->wl_surface);
	client->surface->wl_surface = NULL;

	/**
	 * We expect the error from unknown object, as the destroy request that
	 * we call below destroys the ams wl_proxy.
	 */
	wp_alpha_modifier_surface_v1_destroy(ams);
	expect_protocol_error(client, NULL,
			      WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_NO_SURFACE);

	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
surface_gone_before_set_multiplier_request_error(struct wet_testsuite_data *data)
{
	struct client *client;
	struct wp_alpha_modifier_surface_v1 *ams;

	client = create_client_and_test_surface(100, 50, 100, 100);

	ams = get_alpha_modifier_surface(client, client->surface->wl_surface);

	wl_surface_destroy(client->surface->wl_surface);
	client->surface->wl_surface = NULL;

	wp_alpha_modifier_surface_v1_set_multiplier(ams, UINT32_MAX / 2);
	expect_protocol_error(client, &wp_alpha_modifier_surface_v1_interface,
			      WP_ALPHA_MODIFIER_SURFACE_V1_ERROR_NO_SURFACE);

	wp_alpha_modifier_surface_v1_destroy(ams);
	client_destroy(client);

	return RESULT_OK;
}

DECLARE_TEST_LIST(
	TESTFN(recreate_succeeds),
	TESTFN(already_constructed_error),
	TESTFN(surface_gone_before_destroy_request_error),
	TESTFN(surface_gone_before_set_multiplier_request_error),
);
