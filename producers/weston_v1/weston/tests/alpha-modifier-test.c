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
#include "weston-test-runner.h"

static struct wp_alpha_modifier_surface_v1 *
get_alpha_modifier_surface(struct client *client, struct wl_surface *surface)
{
	struct wp_alpha_modifier_surface_v1 *ams;

	test_assert_ptr_not_null(client->alpha_modifier);
	ams = wp_alpha_modifier_v1_get_surface(client->alpha_modifier, surface);

	return ams;
}

struct setup_args {
	struct fixture_metadata meta;
	enum weston_renderer_type renderer;
};

static const struct setup_args my_setup_args[] = {
	{
		.renderer = WESTON_RENDERER_PIXMAN,
		.meta.name = "pixman"
	},
	{
		.renderer = WESTON_RENDERER_GL,
		.meta.name = "GL"
	},
	{
		.renderer = WESTON_RENDERER_VULKAN,
		.meta.name = "Vulkan"
	},
};

static enum test_result_code
fixture_setup(struct weston_test_harness *harness, const struct setup_args *arg)
{
	struct compositor_setup setup;

	compositor_setup_defaults(&setup);
	setup.shell = SHELL_TEST_DESKTOP;
	setup.refresh = HIGHEST_OUTPUT_REFRESH;
	setup.renderer = arg->renderer;

	return weston_test_harness_execute_as_client(harness, &setup);
}
DECLARE_FIXTURE_SETUP_WITH_ARG(fixture_setup, my_setup_args, meta);

static enum test_result_code
set_multiplier_then_destroy_before_commit(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct wp_alpha_modifier_surface_v1 *ams;
	struct buffer *buf;
	pixman_color_t blue;
	bool match;
	int seq_no = get_test_fixture_index();

	client = create_client_and_test_surface(100, 50, 100, 100);
	/* Move pointer away from the screenshot area. */
	weston_test_move_pointer(client->test->weston_test, 0, 1, 0, 0, 0);

	ams = get_alpha_modifier_surface(client, client->surface->wl_surface);

	/* Destroy should reset the surface alpha modifier to UINT32_MAX. */
	wp_alpha_modifier_surface_v1_set_multiplier(ams, 1000);
	wp_alpha_modifier_surface_v1_destroy(ams);

	color_rgb888(&blue, 0, 0, 255);
	buf = surface_commit_color(client, client->surface->wl_surface, &blue, 100, 100);

	match = verify_screen_content(client, "alpha-modifier",
				      0, NULL, seq_no, NULL, NO_DECORATIONS);
	test_assert_true(match);

	buffer_destroy(buf);
	client_destroy(client);

	return RESULT_OK;
}

static enum test_result_code
various_set_multiplier_before_commit(struct wet_testsuite_data *suite_data)
{
	struct client *client;
	struct wp_alpha_modifier_surface_v1 *ams;
	struct buffer *buf;
	pixman_color_t blue;
	bool match;
	int seq_no = get_test_fixture_index();

	client = create_client_and_test_surface(100, 50, 100, 100);
	/* Move pointer away from the screenshot area. */
	weston_test_move_pointer(client->test->weston_test, 0, 1, 0, 0, 0);

	ams = get_alpha_modifier_surface(client, client->surface->wl_surface);

	/* Last one should be the one set. */
	wp_alpha_modifier_surface_v1_set_multiplier(ams, 1000);
	wp_alpha_modifier_surface_v1_set_multiplier(ams, UINT32_MAX);
	wp_alpha_modifier_surface_v1_set_multiplier(ams, UINT32_MAX / 2);

	color_rgb888(&blue, 0, 0, 255);
	buf = surface_commit_color(client, client->surface->wl_surface, &blue, 100, 100);

	match = verify_screen_content(client, "alpha-modifier",
				      1, NULL, seq_no, NULL, NO_DECORATIONS);
	test_assert_true(match);

	buffer_destroy(buf);
	wp_alpha_modifier_surface_v1_destroy(ams);
	client_destroy(client);

	return RESULT_OK;
}

struct overlapping_surfaces_test_case {
	uint32_t main_surface_ams; /* bottom-most */
	uint32_t top_surface_ams;
	int ref_no;
};

static const struct overlapping_surfaces_test_case
overlapping_surfaces_test_cases[] = {
	{
		/* top transparent, bottom opaque */
		.main_surface_ams = UINT32_MAX,
		.top_surface_ams = UINT32_MAX / 2,
		.ref_no = 2,
	},
	{
		/* both transparent */
		.main_surface_ams = UINT32_MAX / 2,
		.top_surface_ams = UINT32_MAX / 2,
		.ref_no = 3,
	},
	{
		/* top opaque, bottom transparent */
		.main_surface_ams = UINT32_MAX / 2,
		.top_surface_ams = UINT32_MAX,
		.ref_no = 4,
	},
};

static enum test_result_code
overlapping_surfaces(struct wet_testsuite_data *suite_data,
		     const struct overlapping_surfaces_test_case *args)
{
	int seq_no = get_test_fixture_index();
	struct client *client;
	struct wl_subcompositor *subco;
	struct wp_alpha_modifier_surface_v1 *ams_main, *ams_top;
	struct wl_surface *top_surf;
	struct wl_subsurface *top_subsurf;
	pixman_color_t blue, red;
	struct buffer *buf_main, *buf_top;
	bool match;

	color_rgb888(&red, 255, 0, 0);
	color_rgb888(&blue, 0, 0, 255);

	client = create_client_and_test_surface(100, 50, 100, 100);
	subco = client_get_subcompositor(client);
	/* Move pointer away from the screenshot area. */
	weston_test_move_pointer(client->test->weston_test, 0, 1, 0, 0, 0);

	top_surf = wl_compositor_create_surface(client->wl_compositor);
	top_subsurf = wl_subcompositor_get_subsurface(subco, top_surf,
						      client->surface->wl_surface);
	wl_subsurface_set_position(top_subsurf, 80, 80);
	wl_subsurface_place_above(top_subsurf, client->surface->wl_surface);
	wl_subsurface_set_desync(top_subsurf);

	ams_main = get_alpha_modifier_surface(client, client->surface->wl_surface);
	ams_top = get_alpha_modifier_surface(client, top_surf);

	wp_alpha_modifier_surface_v1_set_multiplier(ams_main, args->main_surface_ams);
	wp_alpha_modifier_surface_v1_set_multiplier(ams_top, args->top_surface_ams);

	buf_main = surface_commit_color(client, client->surface->wl_surface, &blue, 100, 100);
	buf_top = surface_commit_color(client, top_surf, &red, 50, 50);

	match = verify_screen_content(client, "alpha-modifier",
				      args->ref_no, NULL, seq_no, NULL, NO_DECORATIONS);
	test_assert_true(match);

	buffer_destroy(buf_main);
	buffer_destroy(buf_top);
	wp_alpha_modifier_surface_v1_destroy(ams_main);
	wp_alpha_modifier_surface_v1_destroy(ams_top);
	wl_surface_destroy(top_surf);
	wl_subsurface_destroy(top_subsurf);
	wl_subcompositor_destroy(subco);
	client_destroy(client);

	return RESULT_OK;
}

DECLARE_TEST_LIST(
	TESTFN(set_multiplier_then_destroy_before_commit),
	TESTFN(various_set_multiplier_before_commit),
	TESTFN_ARG(overlapping_surfaces, overlapping_surfaces_test_cases),
);
