/*
 * Copyright (C) 2019 Konsulko Group
 * Author: Matt Ranostay <matt.ranostay@konsulko.com>
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

#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-c/json.h>

#define AFB_BINDING_VERSION 3
#include <afb/afb-binding.h>

#include "navigation-api.h"

afb_api_t g_api;

static const char *vshl_capabilities_events[] = {
	"setDestination",
	"cancelNavigation",
	NULL,
};

struct navigation_state *navigation_get_userdata(void)
{
	return afb_api_get_userdata(g_api);
}

/*
* get event for subscription depending on value
*/
static afb_event_t get_event_from_value(struct navigation_state *ns,
			const char *value)
{
	if (!g_strcmp0(value, "status"))
		return ns->status_event;

	if (!g_strcmp0(value, "position"))
		return ns->position_event;

	if (!g_strcmp0(value, "waypoints"))
		return ns->waypoints_event;

	if (!g_strcmp0(value, "destination"))
		return ns->destination_event;

	return NULL;
}

/*
* get storage depending on Value
*/
static json_object **get_storage_from_value(struct navigation_state *ns,
			const char *value)
{
	if (!g_strcmp0(value, "status"))
		return &ns->status_storage;

	if (!g_strcmp0(value, "position"))
		return &ns->position_storage;

	if (!g_strcmp0(value, "waypoints"))
		return &ns->waypoints_storage;

	if (!g_strcmp0(value, "destination"))
		return &ns->destination_storage;

	return NULL;
}

/*
* subscribe/unsubscribe to an event depnding on it's name
*/
static void navigation_subscribe_unsubscribe(afb_req_t request,
		gboolean unsub)
{
	struct navigation_state *ns = navigation_get_userdata();
	json_object *jresp = NULL;
	const char *value;
	afb_event_t event;
	int rc;

	value = afb_req_value(request, "value");
	if (!value) {
		afb_req_fail_f(request, "failed", "Missing \"value\" event");
		return;
	}

	event = get_event_from_value(ns, value);
	if (!event) {
		afb_req_fail_f(request, "failed", "Bad \"value\" event \"%s\"",
				value);
		return;
	}

	if (!unsub) {
		json_object *storage;
		rc = afb_req_subscribe(request, event);

		g_rw_lock_reader_lock(&ns->rw_lock);
		storage = *get_storage_from_value(ns, value);
		if (storage) {
			// increment reference counter, and send out cached value
			json_object_get(storage);
			afb_event_push(event, storage);
		}
		g_rw_lock_reader_unlock(&ns->rw_lock);
	} else {
		rc = afb_req_unsubscribe(request, event);
	}
	if (rc != 0) {
		afb_req_fail_f(request, "failed",
					"%s error on \"value\" event \"%s\"",
					!unsub ? "subscribe" : "unsubscribe",
					value);
		return;
	}

	jresp = json_object_new_object();
	afb_req_success_f(request, jresp, "Navigation %s to event \"%s\"",
			!unsub ? "subscribed" : "unsubscribed",
			value);
}

/*
* subscribe & unsubscribe handlers for corresponding verbs
*/
static void subscribe(afb_req_t request)
{
	navigation_subscribe_unsubscribe(request, FALSE);
}

static void unsubscribe(afb_req_t request)
{
	navigation_subscribe_unsubscribe(request, TRUE);
}

/*
* broadcast brodcast information depending on name
*/
static void broadcast(json_object *jresp, const char *name, gboolean cache)
{
	struct navigation_state *ns = navigation_get_userdata();
	afb_event_t event = get_event_from_value(ns, name);
	json_object *tmp = NULL;

	if (json_object_deep_copy(jresp, (json_object **) &tmp, NULL))
		return;

	if (cache) {
		json_object **storage;

		g_rw_lock_writer_lock(&ns->rw_lock);

		storage = get_storage_from_value(ns, name);

		if (*storage)
			json_object_put(*storage);
		*storage = NULL;

		// increment reference for storage
		json_object_get(tmp);
		*storage = tmp;

		// increment reference for event
		json_object_get(tmp);
		afb_event_push(event, tmp);

		g_rw_lock_writer_unlock(&ns->rw_lock);

		return;
	}

	afb_event_push(event, tmp);
}

/*
* broadcast_status verb handler
*/
static void broadcast_status(afb_req_t request)
{
	json_object *jresp = afb_req_json(request);
	broadcast(jresp, "status", TRUE);

	afb_req_success(request, NULL, "Broadcast status send");

	// NOTE: If the Alexa SDK API for pushing local navigation
	//       updates gets exposed, send update to vshl-capabilities
	//       here.
}

/*
* broadcast_position verb handler
*/
static void broadcast_position(afb_req_t request)
{
	const char *position = afb_req_value(request, "position");
	gboolean cache = FALSE;

	// only send out a car position event on subscribe
	if (position && !g_strcmp0(position, "car"))
		cache = TRUE;

	json_object *jresp = afb_req_json(request);
	broadcast(jresp, "position", cache);

	afb_req_success(request, NULL, "Broadcast position send");

	// NOTE: If the Alexa SDK API for pushing local navigation
	//       updates gets exposed, send update to vshl-capabilities
	//       here.
}

/*
* broadcast_waypoints verb handler
*/
static void broadcast_waypoints(afb_req_t request)
{
	json_object *jresp = afb_req_json(request);
	broadcast(jresp, "waypoints", TRUE);

	afb_req_success(request, NULL, "Broadcast waypoints send");

	// NOTE: If the Alexa SDK API for pushing local navigation
	//       updates gets exposed, send update to vshl-capabilities
	//       here.
}

/*
* broadcast_destiantion verb handler
*/
static void broadcast_destination(afb_req_t request)
{
	json_object* jresp = afb_req_json(request);
	broadcast(jresp, "destination", TRUE);

	afb_req_success(request, NULL, "Broadcast destination send");

	// NOTE: If the Alexa SDK API for pushing local navigation
	//       updates gets exposed, send update to vshl-capabilities
	//       here.
}

/*
* Init for the AGL binding
*/
static int init(afb_api_t api)
{
	struct navigation_state *ns;
	int rc;

	ns = g_try_malloc0(sizeof(*ns));
	if (!ns) {
		AFB_ERROR("out of memory allocating navigation state");
		return -ENOMEM;
	}

	rc = afb_daemon_require_api("vshl-capabilities", 1);
	if (!rc) {
		const char **tmp = vshl_capabilities_events;
		json_object *args = json_object_new_object();
		json_object *actions = json_object_new_array();

		while (*tmp) {
			json_object_array_add(actions, json_object_new_string(*tmp++));
		}
		json_object_object_add(args, "actions", actions);
		if(json_object_array_length(actions)) {
			rc = afb_api_call_sync(api, "vshl-capabilities", "navigation/subscribe",
					       args, NULL, NULL, NULL);
			if(rc != 0)
				AFB_WARNING("afb_api_call_sync returned %d", rc);
		} else {
			json_object_put(args);
		}
	} else {
		AFB_WARNING("unable to initialize vshl-capabilities binding");
	}

	ns->status_event = afb_daemon_make_event("status");
	ns->position_event = afb_daemon_make_event("position");
	ns->waypoints_event = afb_daemon_make_event("waypoints");
	ns->destination_event = afb_daemon_make_event("destination");

	if (!afb_event_is_valid(ns->status_event) ||
	    !afb_event_is_valid(ns->position_event) ||
	    !afb_event_is_valid(ns->waypoints_event) ||
		!afb_event_is_valid(ns->destination_event)) {
		AFB_ERROR("Cannot create events");
		return -EINVAL;
	}

	afb_api_set_userdata(api, ns);
	g_api = api;

	g_rw_lock_init(&ns->rw_lock);

	return 0;
}

/*
* verbs of the agl-service-navigation
*/
static const afb_verb_t binding_verbs[] = {
	{
		.verb = "subscribe",
		.callback = subscribe,
		.info = "Subscribe to event"
	}, {
		.verb = "unsubscribe",
		.callback = unsubscribe,
		.info = "Unsubscribe to event"
	}, {
		.verb = "broadcast_status",
		.callback = broadcast_status,
		.info = "Allows clients to broadcast status events"
	}, {
		.verb = "broadcast_position",
		.callback = broadcast_position,
		.info = "Broadcast out position event"
	}, {
		.verb = "broadcast_waypoints",
		.callback = broadcast_waypoints,
		.info = "Broadcast out waypoint event"
	},{
		.verb = "broadcast_destination",
		.callback = broadcast_destination,
		.info = "Broadcast out destination event"
	},
	{}
};

/*
 * description of the binding for afb-daemon
 */
const afb_binding_t afbBindingV3 = {
	.api = "navigationv2",
	.verbs = binding_verbs,
	.init = init,
};
