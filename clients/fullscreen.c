/*
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2012 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "config.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <assert.h>
#include <errno.h>
#include <cairo.h>

#include <linux/input.h>
#include <wayland-client.h>
#include "window.h"
#include "shared/cairo-util.h"
#include "fullscreen-shell-unstable-v1-client-protocol.h"
#include <libweston/zalloc.h>

struct fs_output {
	struct wl_list link;
	struct output *output;
};

struct fullscreen {
	struct display *display;
	struct window *window;
	struct widget *widget;
	struct zwp_fullscreen_shell_v1 *fshell;
	enum zwp_fullscreen_shell_v1_present_method present_method;
	int32_t width, height;
	float pointer_x, pointer_y;
	int draw_cursor;

	char *filename;
	cairo_surface_t *image;
	bool initialized;
	cairo_matrix_t matrix;

	struct wl_list output_list;
	struct fs_output *current_output;
};

static double
get_scale(struct fullscreen *fullscreen)
{
	assert(fullscreen->matrix.xy == 0.0 &&
	       fullscreen->matrix.yx == 0.0 &&
	       fullscreen->matrix.xx == fullscreen->matrix.yy);
	return fullscreen->matrix.xx;
}

static void
clamp_view(struct fullscreen *fullscreen)
{
	struct rectangle allocation;
	double scale = get_scale(fullscreen);
	double sw, sh;

	sw = fullscreen->width * scale;
	sh = fullscreen->height * scale;
	widget_get_allocation(fullscreen->widget, &allocation);

	if (sw < allocation.width) {
		fullscreen->matrix.x0 =
			(allocation.width - fullscreen->width * scale) / 2;
	} else {
		if (fullscreen->matrix.x0 > 0.0)
			fullscreen->matrix.x0 = 0.0;
		if (sw + fullscreen->matrix.x0 < allocation.width)
			fullscreen->matrix.x0 = allocation.width - sw;
	}

	if (sh < allocation.height) {
		fullscreen->matrix.y0 =
			(allocation.height - fullscreen->height * scale) / 2;
	} else {
		if (fullscreen->matrix.y0 > 0.0)
			fullscreen->matrix.y0 = 0.0;
		if (sh + fullscreen->matrix.y0 < allocation.height)
			fullscreen->matrix.y0 = allocation.height - sh;
	}
}

static void
redraw_handler(struct widget *widget, void *data)
{
	struct fullscreen *fullscreen = data;
	struct rectangle allocation;
	cairo_surface_t *surface;
	cairo_t *cr;
	double width, height, doc_aspect, window_aspect, scale;
	cairo_matrix_t matrix;
	cairo_matrix_t translate;
	// int i;
	//double x, y, border;
	//const char *method_name[] = { "default", "center", "zoom", "zoom_crop", "stretch"};

	surface = window_get_surface(fullscreen->window);
	if (surface == NULL ||
	    cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		fprintf(stderr, "failed to create cairo egl surface\n");
		return;
	}

	widget_get_allocation(fullscreen->widget, &allocation);

	cr = widget_cairo_create(widget);

	cairo_rectangle(cr, allocation.x, allocation.y,
			allocation.width, allocation.height);
	cairo_clip(cr);
	cairo_push_group(cr);
	cairo_translate(cr, allocation.x, allocation.y);

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 1);
	cairo_paint(cr);

	if (!fullscreen->initialized) {
		fullscreen->initialized = true;
		width = cairo_image_surface_get_width(fullscreen->image);
		height = cairo_image_surface_get_height(fullscreen->image);

		doc_aspect = width / height;
		window_aspect = (double) allocation.width / allocation.height;
		if (doc_aspect < window_aspect)
			scale = allocation.height / height;
		else
			scale = allocation.width / width;

		fullscreen->width = width;
		fullscreen->height = height;
		cairo_matrix_init_scale(&fullscreen->matrix, scale, scale);

		clamp_view(fullscreen);
	}

	matrix = fullscreen->matrix;
	cairo_matrix_init_translate(&translate, allocation.x, allocation.y);
	cairo_matrix_multiply(&matrix, &matrix, &translate);
	cairo_set_matrix(cr, &matrix);

	cairo_set_source_surface(cr, fullscreen->image, 0, 0);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	cairo_paint(cr);

	cairo_pop_group_to_source(cr);
	cairo_paint(cr);
	cairo_destroy(cr);

	cairo_surface_destroy(surface);
}

static int
enter_handler(struct widget *widget,
	      struct input *input,
	      float x, float y, void *data)
{
	struct fullscreen *fullscreen = data;

	fullscreen->pointer_x = x;
	fullscreen->pointer_y = y;

	widget_schedule_redraw(widget);

	return fullscreen->draw_cursor ? CURSOR_BLANK : CURSOR_LEFT_PTR;
}

static void
fshell_capability_handler(void *data, struct zwp_fullscreen_shell_v1 *fshell,
			  uint32_t capability)
{
	struct fullscreen *fullscreen = data;

	switch (capability) {
	case ZWP_FULLSCREEN_SHELL_V1_CAPABILITY_CURSOR_PLANE:
		fullscreen->draw_cursor = 0;
		break;
	default:
		break;
	}
}

struct zwp_fullscreen_shell_v1_listener fullscreen_shell_listener = {
	fshell_capability_handler
};

static void
usage(int error_code)
{
	fprintf(stderr, "Usage: fullscreen [OPTIONS]\n\n"
		"   -w <width>\tSet window width to <width>\n"
		"   -h <height>\tSet window height to <height>\n"
		"   --help\tShow this help text\n\n");

	exit(error_code);
}

static void
output_handler(struct output *output, void *data)
{
	struct fullscreen *fullscreen = data;
	struct fs_output *fsout;

	/* If we've already seen this one, don't add it to the list */
	wl_list_for_each(fsout, &fullscreen->output_list, link)
		if (fsout->output == output)
			return;

	fsout = zalloc(sizeof *fsout);
	if (fsout == NULL) {
		fprintf(stderr, "out of memory in output_handler\n");
		return;
	}
	fsout->output = output;
	wl_list_insert(&fullscreen->output_list, &fsout->link);
}

static void
global_handler(struct display *display, uint32_t id, const char *interface,
	       uint32_t version, void *data)
{
	struct fullscreen *fullscreen = data;

	if (strcmp(interface, "zwp_fullscreen_shell_v1") == 0) {
		fullscreen->fshell = display_bind(display, id,
						  &zwp_fullscreen_shell_v1_interface,
						  1);
		zwp_fullscreen_shell_v1_add_listener(fullscreen->fshell,
						     &fullscreen_shell_listener,
						     fullscreen);
	}
}

int main(int argc, char *argv[])
{
	struct fullscreen fullscreen;
	struct display *d;
	int i;

	if (argc <= 1 || argv[1][0]=='-') {
		printf("Usage: %s image...\n", argv[0]); return 1;
	}

	fullscreen.filename = strdup(argv[1]);
	printf("fullscreen.filename: %s", fullscreen.filename);

	fullscreen.width = 200;
	fullscreen.height = 100;
	fullscreen.present_method = ZWP_FULLSCREEN_SHELL_V1_PRESENT_METHOD_DEFAULT;
	wl_list_init(&fullscreen.output_list);
	fullscreen.current_output = NULL;

	for (i = 2; i < argc; i++) {
		if (strcmp(argv[i], "-w") == 0) {
			if (++i >= argc)
				usage(EXIT_FAILURE);

			fullscreen.width = atol(argv[i]);
		} else if (strcmp(argv[i], "-h") == 0) {
			if (++i >= argc)
				usage(EXIT_FAILURE);

			fullscreen.height = atol(argv[i]);
		} else if (strcmp(argv[i], "--help") == 0)
			usage(EXIT_SUCCESS);
		else
			usage(EXIT_FAILURE);
	}

	d = display_create(&argc, argv);
	if (d == NULL) {
		fprintf(stderr, "failed to create display: %s\n",
			strerror(errno));
		return -1;
	}

	fullscreen.display = d;
	fullscreen.fshell = NULL;

	fullscreen.image = load_cairo_surface(argv[1]);

	if (!fullscreen.image) {
		free(fullscreen.filename);
		return -1;
	}

	display_set_user_data(fullscreen.display, &fullscreen);
	display_set_global_handler(fullscreen.display, global_handler);
	display_set_output_configure_handler(fullscreen.display, output_handler);

	if (fullscreen.fshell) {
		fullscreen.window = window_create_custom(d);
		zwp_fullscreen_shell_v1_present_surface(fullscreen.fshell,
							window_get_wl_surface(fullscreen.window),
							fullscreen.present_method,
							NULL);
		/* If we get the CURSOR_PLANE capability, we'll change this */
		fullscreen.draw_cursor = 1;
	} else {
		fullscreen.window = window_create(d);
		fullscreen.draw_cursor = 0;
	}

	fullscreen.widget =
		window_add_widget(fullscreen.window, &fullscreen);

	window_set_title(fullscreen.window, "Fullscreen");

	widget_set_transparent(fullscreen.widget, 0);

	widget_set_default_cursor(fullscreen.widget, CURSOR_LEFT_PTR);
	widget_set_redraw_handler(fullscreen.widget, redraw_handler);
	widget_set_enter_handler(fullscreen.widget, enter_handler);

	/* force fullscreen mode */
	window_set_fullscreen(fullscreen.window, 1);

	window_set_user_data(fullscreen.window, &fullscreen);
	/* Hack to set minimum allocation so we can shrink later */
	window_schedule_resize(fullscreen.window,
			       1, 1);
	window_schedule_resize(fullscreen.window,
			       fullscreen.width, fullscreen.height);

	display_run(d);

	widget_destroy(fullscreen.widget);
	window_destroy(fullscreen.window);
	display_destroy(d);

	return 0;
}
