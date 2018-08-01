/*
  Copyright 2006-2011 David Robillard <d@drobilla.net>
  Copyright 2006 Steve Harris <steve@plugin.org.uk>

  Permission to use, copy, modify, and/or distribute this software for any
  purpose with or without fee is hereby granted, provided that the above
  copyright notice and this permission notice appear in all copies.

  THIS SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
*/

/** Include standard C headers */
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>


#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

#define ALO_URI "http://devcurmudgeon.com/alo"

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Float;
	LV2_URID atom_Object;
	LV2_URID atom_Path;
	LV2_URID atom_Resource;
	LV2_URID atom_Sequence;
	LV2_URID time_Position;
	LV2_URID time_barBeat;
	LV2_URID time_beatsPerMinute;
	LV2_URID time_speed;
} AloURIs;

typedef enum {
	ALO_INPUT = 0,
	ALO_OUTPUT = 1,
	ALO_BUTTON = 2,
	ALO_CONTROL = 3
} PortIndex;

void log(const char *message, ...)
{
	FILE* f;
	f = fopen("/root/alo.log", "a+");

    char buffer[2048];
    va_list argumentList;
    va_start(argumentList, message);
    vsnprintf(&buffer[0], sizeof(buffer), message, argumentList);
    va_end(argumentList);
    fwrite(buffer, 1, strlen(buffer), f);
    fprintf(f, "\n");
    fclose(f);
}

void timestamp()
{
    time_t rawtime;
    struct tm * timeinfo;

    time (&rawtime);
    timeinfo = localtime (&rawtime);

    struct timeval te;
    gettimeofday(&te, NULL); // get current time
    long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000;
    log("%s %lld", asctime(timeinfo), milliseconds);
}

/**
   Every plugin defines a private structure for the plugin instance.  All data
   associated with a plugin instance is stored here, and is available to
   every instance method.
*/
typedef struct {

	LV2_URID_Map* map;   // URID map feature
	AloURIs     uris;  // Cache of mapped URIDs

	// Port buffers
	struct {
		const float* input;
		float*       output;
		float*       button;
		LV2_Atom_Sequence* control;
	} ports;


	// Variables to keep track of the tempo information sent by the host
	double rate;   // Sample rate
	float  bpm;    // Beats per minute (tempo)
	float  speed;  // Transport speed (usually 0=stop, 1=play)

	uint32_t elapsed_len;  // Frames since the start of the last click
	uint32_t wave_offset;  // Current play offset in the wave
} Alo;

/**
   The `instantiate()` function is called by the host to create a new plugin
   instance.  The host passes the plugin descriptor, sample rate, and bundle
   path for plugins that need to load additional resources (e.g. waveforms).
   The features parameter contains host-provided features defined in LV2
   extensions, but this simple plugin does not use any.

   This function is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
	log("Instantiate");
	Alo* self = (Alo*)calloc(1, sizeof(Alo));

	LV2_URID_Map* map = NULL;
	for (int i = 0; features[i]; ++i) {
		if (!strcmp(features[i]->URI, LV2_URID_URI "#map")) {
			map = (LV2_URID_Map*)features[i]->data;
		}
	}
	if (!map) {
		fprintf(stderr, "Host does not support urid:map.\n");
		free(self);
		return NULL;
	}

	// Map URIS
	AloURIs* const uris = &self->uris;
	self->map = map;
	uris->atom_Blank          = map->map(map->handle, LV2_ATOM__Blank);
	uris->atom_Float          = map->map(map->handle, LV2_ATOM__Float);
	uris->atom_Object         = map->map(map->handle, LV2_ATOM__Object);
	uris->atom_Path           = map->map(map->handle, LV2_ATOM__Path);
	uris->atom_Resource       = map->map(map->handle, LV2_ATOM__Resource);
	uris->atom_Sequence       = map->map(map->handle, LV2_ATOM__Sequence);
	uris->time_Position       = map->map(map->handle, LV2_TIME__Position);
	uris->time_barBeat        = map->map(map->handle, LV2_TIME__barBeat);
	uris->time_beatsPerMinute = map->map(map->handle, LV2_TIME__beatsPerMinute);
	uris->time_speed          = map->map(map->handle, LV2_TIME__speed);

	log("End of instantiate");
	return (LV2_Handle)self;
}

/**
   The `connect_port()` method is called by the host to connect a particular
   port to a buffer.  The plugin must store the data location, but data may not
   be accessed except in run().

   This method is in the ``audio'' threading class, and is called in the same
   context as run().
*/
static void
connect_port(LV2_Handle instance,
             uint32_t   port,
             void*      data)
{
	Alo* self = (Alo*)instance;

	switch ((PortIndex)port) {
	case ALO_INPUT:
		self->ports.input = (const float*)data;
		timestamp();
		log("Connect ALO_INPUT %d", port);
		break;
	case ALO_OUTPUT:
		self->ports.output = (float*)data;
		log("Connect ALO_OUTPUT %d", port);
		break;
	case ALO_BUTTON:
		self->ports.button = (float*)data;
		log("Connect ALO_BUTTON %d", port);
		break;
	case ALO_CONTROL:
		self->ports.control = (LV2_Atom_Sequence*)data;
		log("Connect ALO_CONTROL %d", port);
	}
}

/**
   The `activate()` method is called by the host to initialise and prepare the
   plugin instance for running.  The plugin must reset all internal state
   except for buffer locations set by `connect_port()`.  Since this plugin has
   no other internal state, this method does nothing.

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void
activate(LV2_Handle instance)
{
	log("Activate");
}

static void
update_position(Alo* self, const LV2_Atom_Object* obj)
{
	timestamp();
	log("Update position");
}
/**
   The `run()` method is the main process function of the plugin.  It processes
   a block of audio in the audio context.  Since this plugin is
   `lv2:hardRTCapable`, `run()` must be real-time safe, so blocking (e.g. with
   a mutex) or memory allocation are not allowed.
*/
static void
run(LV2_Handle instance, uint32_t n_samples)
{
	log("Run %d", n_samples);
	Alo* self = (Alo*)instance;

	const float* const input  = self->ports.input;
	float* const       output = self->ports.output;

	for (uint32_t pos = 0; pos < n_samples; pos++) {
		output[pos] = input[pos];
	}

	const AloURIs* uris = &self->uris;

	// from metro.c
	// Work forwards in time frame by frame, handling events as we go
	const LV2_Atom_Sequence* in = self->ports.control;
	uint32_t last_t = 0;

	for (const LV2_Atom_Event* ev = lv2_atom_sequence_begin(&in->body);
	     !lv2_atom_sequence_is_end(&in->body, in->atom.size, ev);
	     ev = lv2_atom_sequence_next(ev)) {

		// Play the click for the time slice from last_t until now
//		play(self, last_t, ev->time.frames);

		// Check if this event is an Object
		// (or deprecated Blank to tolerate old hosts)
		if (ev->body.type == uris->atom_Object ||
		    ev->body.type == uris->atom_Blank) {
			const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == uris->time_Position) {
				// Received position information, update
				update_position(self, obj);
			}
		}

		// Update time for next iteration and move to next event
		last_t = ev->time.frames;
	}

	// Play for remainder of cycle
//	play(self, last_t, sample_count);
}



/**
   The `deactivate()` method is the counterpart to `activate()`, and is called by
   the host after running the plugin.  It indicates that the host will not call
   `run()` again until another call to `activate()` and is mainly useful for more
   advanced plugins with ``live'' characteristics such as those with auxiliary
   processing threads.  As with `activate()`, this plugin has no use for this
   information so this method does nothing.

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void
deactivate(LV2_Handle instance)
{
	log("Deactivate");
}

/**
   Destroy a plugin instance (counterpart to `instantiate()`).

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void
cleanup(LV2_Handle instance)
{
	free(instance);
}

/**
   The `extension_data()` function returns any extension data supported by the
   plugin.  Note that this is not an instance method, but a function on the
   plugin descriptor.  It is usually used by plugins to implement additional
   interfaces.  This plugin does not have any extension data, so this function
   returns NULL.

   This method is in the ``discovery'' threading class, so no other functions
   or methods in this plugin library will be called concurrently with it.
*/
static const void*
extension_data(const char* uri)
{
	return NULL;
}

/**
   Every plugin must define an `LV2_Descriptor`.  It is best to define
   descriptors statically to avoid leaking memory and non-portable shared
   library constructors and destructors to clean up properly.
*/
static const LV2_Descriptor descriptor = {
	ALO_URI,
	instantiate,
	connect_port,
	activate,
	run,
	deactivate,
	cleanup,
	extension_data
};

/**
   The `lv2_descriptor()` function is the entry point to the plugin library.  The
   host will load the library and call this function repeatedly with increasing
   indices to find all the plugins defined in the library.  The index is not an
   indentifier, the URI of the returned descriptor is used to determine the
   identify of the plugin.

   This method is in the ``discovery'' threading class, so no other functions
   or methods in this plugin library will be called concurrently with it.
*/
LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
	switch (index) {
	case 0:  return &descriptor;
	default: return NULL;
	}
}