/*
 * Copyright © 2020 Stefan Agner <stefan@agner.ch>
 * based on AML examples
 * Copyright © 2020 Andri Yngvason
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

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <aml.h>
#include <poll.h>
#include <signal.h>
#include <wayland-server.h>

#include "aml-wl-backend.h"

struct wl_backend_state {
	struct aml* aml;
	struct wl_event_loop* loop;
};

struct event_source_data {
	void* aml_obj;
	struct wl_backend_state* state;
	struct wl_event_source* src;
};

static struct wl_event_loop* wl_loop = NULL;

static void* wl_backend_new_state(struct aml* aml)
{
	struct wl_backend_state* state = calloc(1, sizeof(*state));
	if (!state)
		return NULL;

	state->aml = aml;
	state->loop = wl_loop;

	return state;
}

static void wl_backend_del_state(void* state)
{
	free(state);
}

static void wl_backend_exit()
{
}

static uint32_t wl_backend_events_from_poll_events(uint32_t in)
{
	uint32_t out = 0;

	if (in & POLLIN) out |= WL_EVENT_READABLE;
	if (in & POLLOUT) out |= WL_EVENT_WRITABLE;

	return out;
}

static uint32_t wl_backend_events_to_poll_events(uint32_t in)
{
	uint32_t out = 0;

	if (in & WL_EVENT_READABLE) out |= POLLIN;
	if (in & WL_EVENT_WRITABLE) out |= POLLOUT;

	return out;
}

static int wl_backend_on_fd_event(int fd, uint32_t mask, void* userdata)
{
	struct event_source_data* data = userdata;
	uint32_t revents = wl_backend_events_to_poll_events(mask);

	aml_emit(data->state->aml, data->aml_obj, revents);
	aml_dispatch(data->state->aml);

	return 0;
}

static struct event_source_data*
event_source_data_new(void* obj, struct wl_backend_state* state)
{
	struct event_source_data* data = calloc(1, sizeof(*data));
	if (!data)
		return NULL;

	data->aml_obj = obj;
	data->state = state;

	return data;
}

static int wl_backend_add_fd(void* state, struct aml_handler* handler)
{
	struct wl_event_source* src;
	struct wl_backend_state* self = state;
	int fd = aml_get_fd(handler);
	uint32_t aml_event_mask = aml_get_event_mask(handler);
	uint32_t events = wl_backend_events_from_poll_events(aml_event_mask);

	struct event_source_data* data = event_source_data_new(handler, state);
	if (!data)
		return -1;

	src = wl_event_loop_add_fd(self->loop, fd, events,
	                           wl_backend_on_fd_event, data);
	if (!src)
		goto failure;

	data->src = src;
	aml_set_backend_data(handler, data);

	return 0;

failure:
	free(data);
	return -1;
}

static int wl_backend_mod_fd(void* state, struct aml_handler* handler)
{
	struct event_source_data* data = aml_get_backend_data(handler);
	uint32_t aml_event_mask = aml_get_event_mask(handler);
	uint32_t wl_mask = wl_backend_events_from_poll_events(aml_event_mask);
	wl_event_source_fd_update(data->src, wl_mask);
	return 0;
}

static int wl_backend_del_fd(void* state, struct aml_handler* handler)
{
	struct event_source_data* data = aml_get_backend_data(handler);
	wl_event_source_remove(data->src);
	free(data);
	return 0;
}

static int wl_backend_on_signal(int signo, void* userdata)
{
	struct event_source_data* data = userdata;

	aml_emit(data->state->aml, data->aml_obj, 0);
	aml_dispatch(data->state->aml);

	return 0;
}

static int wl_backend_add_signal(void* state, struct aml_signal* sig)
{
	struct wl_event_source* src;
	struct wl_backend_state* self = state;
	int signo = aml_get_signo(sig);

	struct event_source_data* data = event_source_data_new(sig, state);
	if (!data)
		return -1;

	src = wl_event_loop_add_signal(self->loop, signo, wl_backend_on_signal,
	                               data);
	if (!src)
		goto failure;

	data->src = src;
	aml_set_backend_data(sig, data);

	return 0;

failure:
	free(data);
	return -1;
}

static int wl_backend_del_signal(void* state, struct aml_signal* sig)
{
	struct event_source_data* data = aml_get_backend_data(sig);
	wl_event_source_remove(data->src);
	free(data);
	return 0;
}

static struct aml_backend aml_wl_backend = {
	.new_state = wl_backend_new_state,
	.del_state = wl_backend_del_state,
	.poll = NULL,
	.exit = wl_backend_exit,
	.add_fd = wl_backend_add_fd,
	.mod_fd = wl_backend_mod_fd,
	.del_fd = wl_backend_del_fd,
	.add_signal = wl_backend_add_signal,
	.del_signal = wl_backend_del_signal,
};

struct aml* aml_wl_loop_init(struct wl_event_loop* loop)
{
	wl_loop = loop;

	return aml_new(&aml_wl_backend, sizeof(aml_wl_backend));
}
