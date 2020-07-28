/*
  Copyright 2006-2012 David Robillard <d@drobilla.net>
  Copyright 2006 Steve Harris <steve@plugin.org.uk>
  Copyright 2018 Stevie <modplugins@radig.com>
  Copyright 2018 Paul Sherwood <devcurmudgeon@gmail.com>

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
#include <lv2/lv2plug.in/ns/ext/midi/midi.h>

#define ALO_URI "http://devcurmudgeon.com/alo"

typedef struct {
	LV2_URID atom_Blank;
	LV2_URID atom_Float;
	LV2_URID atom_Object;
	LV2_URID midi_MidiEvent;
	LV2_URID atom_Path;
	LV2_URID atom_Resource;
	LV2_URID atom_Sequence;
	LV2_URID time_Position;
	LV2_URID time_barBeat;
	LV2_URID time_beatsPerMinute;
	LV2_URID time_beatsPerBar;
	LV2_URID time_speed;
} AloURIs;

typedef enum {
	ALO_INPUT = 0,
	ALO_OUTPUT = 1,
	ALO_BARS = 2,
	ALO_CONTROL = 3,
	ALO_LOOP1 = 4,
	ALO_LOOP2 = 5,
	ALO_LOOP3 = 6,
	ALO_LOOP4 = 7,
	ALO_LOOP5 = 8,
	ALO_LOOP6 = 9,
	ALO_THRESHOLD = 10,
	ALO_MIDIIN = 11,
	ALO_MIDI_BASE = 12,
	ALO_PER_BEAT_LOOPS = 13,
	ALO_CLICK = 14,
} PortIndex;

typedef enum {
	// NB: for all states, we are always recording in the background
	STATE_RECORDING, // no loop is set, we are only recording
	STATE_LOOP_ON, // the loop is playing
	STATE_LOOP_OFF // the loop is not playing
} State;

typedef enum {
    STATE_OFF,      // No click
    STATE_ATTACK,  // Envelope rising
    STATE_DECAY,   // Envelope lowering
    STATE_SILENT  // Silent
} ClickState;

static const size_t STORAGE_MEMORY = 2880000;
static const int NUM_LOOPS = 6;
static const bool LOG_ENABLED = false;

#define DEFAULT_BEATS_PER_BAR 4
#define DEFAULT_NUM_BARS 4
#define DEFAULT_BPM 120
#define DEFAULT_PER_BEAT_LOOPS 0

void log(const char *message, ...)
{
	if (!LOG_ENABLED) {
		return;
	}

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

///
/// Convert an input parameter expressed as db into a linear float value
///
static float dbToFloat(float db)
{
    if (db <= -90.0f)
        return 0.0f;
    return powf(10.0f, db * 0.05f);
}

/**
   Every plugin defines a private structure for the plugin instance.  All data
   associated with a plugin instance is stored here, and is available to
   every instance method.
*/
typedef struct {

	LV2_URID_Map* map;   // URID map feature
	AloURIs	    uris;    // Cache of mapped URIDs

	// Port buffers
	struct {
		const float* input;
		float* loops[NUM_LOOPS];
		float* bars;
		LV2_Atom_Sequence* control;
		float* threshold;
		float* output;
		float* midi_base;	// start note for midi control of loops
		float* pb_loops;		// number of loops in  per-beat mode
		float* click;		// click mode on/off
		LV2_Atom_Sequence* midiin;	// midi input
	} ports;

	// Variables to keep track of the tempo information sent by the host
	double rate;		// Sample rate
	float  bpm;		// Beats per minute (tempo)
	float  bpb;		// Beats per bar
	float  speed;		// Transport speed (usually 0=stop, 1=play)
	float threshold;	// minimum level to trigger loop start
	uint32_t loop_beats;	// loop length in beats
	uint32_t loop_samples;	// loop length in samples
	uint32_t current_bb;	// which beat of the bar we are on (1, 2, 3, 0)
	uint32_t current_lb;	// which beat of the loop we are on (1, 2, ...)

	uint32_t pb_loops;	// number of loops in per-beat mode

	State state[NUM_LOOPS];	   // we're recording, playing or not playing

	bool button_state[NUM_LOOPS];
	bool midi_control;
	uint32_t  button_time[NUM_LOOPS]; // last time button was pressed

	float* loops[NUM_LOOPS]; // pointers to memory for playing loops
	uint32_t phrase_start[NUM_LOOPS]; // index into recording/loop
	float* recording;    // pointer to memory for recording - for all loops
	uint32_t loop_index; // index into loop for current play point

	ClickState clickstate;

	uint32_t elapsed_len;  // Frames since the start of the last click
	uint32_t wave_offset;  // Current play offset in the wave

	// One cycle of a sine wave
	float*   wave;
	uint32_t wave_len;

	// Envelope parameters
	uint32_t attack_len;
	uint32_t decay_len;
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
	    double		      rate,
	    const char*		      bundle_path,
	    const LV2_Feature* const* features)
{
	Alo* self = (Alo*)calloc(1, sizeof(Alo));
	self->rate = rate;
	self->bpb = DEFAULT_BEATS_PER_BAR;
	self->loop_beats = DEFAULT_BEATS_PER_BAR * DEFAULT_NUM_BARS;
	self->bpm = DEFAULT_BPM;
	self->loop_samples = self->loop_beats * self->rate  * 60.0f / self->bpm;
	self->current_bb = 0;
	self->current_lb = 0;
	self->pb_loops = DEFAULT_PER_BEAT_LOOPS;
	
	self->midi_control = false;

	self->recording = (float *)calloc(STORAGE_MEMORY, sizeof(float));

	for (int i = 0; i < NUM_LOOPS; i++) {
		self->loops[i] = (float *)calloc(STORAGE_MEMORY, sizeof(float));
		self->phrase_start[i] = 0;
		self->state[i] = STATE_RECORDING;
	}
	self->loop_index = 0;
	self->threshold = 0.0;

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
	uris->atom_Blank	  = map->map(map->handle, LV2_ATOM__Blank);
	uris->atom_Float	  = map->map(map->handle, LV2_ATOM__Float);
	uris->atom_Object	  = map->map(map->handle, LV2_ATOM__Object);
	uris->atom_Path		  = map->map(map->handle, LV2_ATOM__Path);
	uris->atom_Resource	  = map->map(map->handle, LV2_ATOM__Resource);
	uris->atom_Sequence	  = map->map(map->handle, LV2_ATOM__Sequence);
	uris->time_Position	  = map->map(map->handle, LV2_TIME__Position);
	uris->time_barBeat	  = map->map(map->handle, LV2_TIME__barBeat);
	uris->time_beatsPerMinute = map->map(map->handle, LV2_TIME__beatsPerMinute);
	uris->time_speed	  = map->map(map->handle, LV2_TIME__speed);
	uris->time_beatsPerBar = map->map(map->handle, LV2_TIME__beatsPerBar);
	uris->midi_MidiEvent   = map->map (map->handle, LV2_MIDI__MidiEvent);

	// Generate one cycle of a sine wave at the desired frequency
	const double freq = 440.0 * 2.0;
	const double amp  = 0.5;
	self->wave_len = (uint32_t)(rate / freq);
	self->wave     = (float*)malloc(self->wave_len * sizeof(float));
	for (uint32_t i = 0; i < self->wave_len; ++i) {
		self->wave[i] = (float)(sin(i * 2 * M_PI * freq / rate) * amp);
	}

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
	     uint32_t	port,
	     void*	data)
{
	Alo* self = (Alo*)instance;

	switch ((PortIndex)port) {
	case ALO_INPUT:
		self->ports.input = (const float*)data;
		log("Connect ALO_INPUT %d", port);
		break;
	case ALO_OUTPUT:
		self->ports.output = (float*)data;
		log("Connect ALO_OUTPUT %d", port);
		break;
	case ALO_BARS:
		self->ports.bars = (float*)data;
		log("Connect ALO_BEATS %d %d", port);
		break;
	case ALO_CONTROL:
		self->ports.control = (LV2_Atom_Sequence*)data;
		log("Connect ALO_CONTROL %d", port);
		break;
	case ALO_THRESHOLD:
		self->ports.threshold = (float*)data;
		log("Connect ALO_THRESHOLD %d %d", port);
		break;
	case ALO_MIDIIN:
		self->ports.midiin = (LV2_Atom_Sequence*)data;
		log("Connect ALO_MIDIIN %d %d", port);
		break;
	case ALO_MIDI_BASE:
		self->ports.midi_base = (float*)data;
		log("Connect ALO_MIDI_BASE %d %d", port);
		break;
	case ALO_PER_BEAT_LOOPS:
		self->ports.pb_loops = (float*)data;
		log("Connect ALO_PER_BEAT_LOOPS %d %d", port);
		break;
	case ALO_CLICK:
		self->ports.click = (float*)data;
		log("Connect ALO_CLICK %d %d", port);
		break;
	default:
		int loop = port - 4;
		self->ports.loops[loop] = (float*)data;
		log("Connect ALO_LOOP %d", loop);
	}
}

static void
reset(Alo* self)
{
	self->pb_loops = (uint32_t)floorf(*(self->ports.pb_loops));
	self->loop_beats = (uint32_t)floorf(self->bpb) * (uint32_t)floorf(*(self->ports.bars));
	self->loop_samples = self->loop_beats * self->rate  * 60.0f / self->bpm;
	if (self->loop_samples > STORAGE_MEMORY) {
		self->loop_samples = STORAGE_MEMORY;
	}
	self->loop_index = 0;
	log("Loop beats: %d", self->loop_beats);
	log("BPM: %G", self->bpm);
	log("Loop_samples: %d", self->loop_samples);
	for (int i = 0; i < NUM_LOOPS; i++) {
		self->button_state[i] = (*self->ports.loops[i]) > 0.0f ? true : false;
		self->state[i] = STATE_RECORDING;
		self->phrase_start[i] = 0;
		log("STATE: RECORDING (reset) [%d]", i);
	}

    self->clickstate = STATE_OFF;
    uint32_t click = (uint32_t)floorf(*(self->ports.click));

    if (click != 0) {
        self->clickstate = STATE_SILENT;
    }
}

/**
   The `activate()` method is called by the host to initialise and prepare the
   plugin instance for running.	 The plugin must reset all internal state
   except for buffer locations set by `connect_port()`.	 Since this plugin has
   no other internal state, this method does nothing.

   This method is in the ``instantiation'' threading class, so no other
   methods on this instance will be called concurrently with it.
*/
static void
activate(LV2_Handle instance)
{
	log("Activate");
}

/**
   Update the current (midi) position based on a host message.	This is called
   by run() when a time:Position is received.
*/
static void
update_position(Alo* self, const LV2_Atom_Object* obj)
{
	AloURIs* const uris = &self->uris;

	// Received new transport position/speed
	LV2_Atom *beat = NULL, *bpm = NULL, *bpb = NULL, *speed = NULL;
	lv2_atom_object_get(obj,
			    uris->time_barBeat, &beat,
			    uris->time_beatsPerMinute, &bpm,
			    uris->time_speed, &speed,
			    uris->time_beatsPerBar, &bpb,
			    NULL);

	if (bpb && bpb->type == uris->atom_Float) {
		if (self->bpb != ((LV2_Atom_Float*)bpb)->body) {
			self->bpb = ((LV2_Atom_Float*)bpb)->body;
			reset(self);
		}
	}

	if (bpm && bpm->type == uris->atom_Float) {
		if (round(self->bpm) != round(((LV2_Atom_Float*)bpm)->body)) {
			// Tempo changed, update BPM
			self->bpm = ((LV2_Atom_Float*)bpm)->body;
			reset(self);
		}
	}
	if (speed && speed->type == uris->atom_Float) {
		if (self->speed != ((LV2_Atom_Float*)speed)->body) {
			// Speed changed, e.g. 0 (stop) to 1 (play)
			// reset the loop start
			self->speed = ((LV2_Atom_Float*)speed)->body;
			reset(self);
			log("Speed change: %G", self->speed);
			log("Loop: [%d][%d]", self->loop_beats, self->loop_samples);
		};
	}
	if (beat && beat->type == uris->atom_Float) {
		// Received a beat position, synchronise
//		const float frames_per_beat = 60.0f / self->bpm * self->rate;
		const float bar_beat = ((LV2_Atom_Float*)beat)->body;
//		const float beat_beats	    = bar_beats - floorf(bar_beats);
		if (self->current_bb != (uint32_t)bar_beat) {
			// we are onto the next beat
			self->current_bb = (uint32_t)bar_beat;
			if (self->current_lb == self->loop_beats) {
				self->current_lb = 0;
			}
			log("Beat:[%d][%d] index[%d] beat[%G]", self->current_bb, self->current_lb, self->loop_index, bar_beat);
			self->current_lb += 1;
		}
	}
}

/**
   Adjust self->state based on button presses.
*/
static void
button_logic(LV2_Handle instance, bool new_button_state, int i)
{
	Alo* self = (Alo*)instance;

	struct timeval te;
	gettimeofday(&te, NULL); // get current time
	long long milliseconds = te.tv_sec*1000LL + te.tv_usec/1000;

	log("Button logic [%d]", i);
	self->button_state[i] = new_button_state;

	int difference = milliseconds - self->button_time[i];
	self->button_time[i] = milliseconds;
	if (new_button_state == true) {
		log("button ON for loop [%d]", i);
	} else {
		log("button OFF for loop [%d]", i);
	}
	if (difference < 1000) {
		// double press, user is resetting
		// so back to recording mode
		self->state[i] = STATE_RECORDING;
		self->phrase_start[i] = 0;
		log("STATE: RECORDING (button reset) [%d]", i);
	}
}

/**
   ** Taken directly from metro.c **
   Play back audio for the range [begin..end) relative to this cycle.  This is
   called by run() in-between events to output audio up until the current time.
*/
static void
click(Alo* self, uint32_t begin, uint32_t end)
{
	float* const   output          = self->ports.output;
	const uint32_t frames_per_beat = 60.0f / self->bpm * self->rate;

	if (self->speed == 0.0f) {
		memset(output, 0, (end - begin) * sizeof(float));
		return;
	}

	for (uint32_t i = begin; i < end; ++i) {
		switch (self->clickstate) {
		case STATE_ATTACK:
			// Amplitude increases from 0..1 until attack_len
			output[i] = self->wave[self->wave_offset] *
				self->elapsed_len / (float)self->attack_len;
			if (self->elapsed_len >= self->attack_len) {
				self->clickstate = STATE_DECAY;
			}
			break;
		case STATE_DECAY:
			// Amplitude decreases from 1..0 until attack_len + decay_len
			output[i] = 0.0f;
			output[i] = self->wave[self->wave_offset] *
				(1 - ((self->elapsed_len - self->attack_len) /
				      (float)self->decay_len));
			if (self->elapsed_len >= self->attack_len + self->decay_len) {
				self->clickstate = STATE_SILENT;
			}
			break;
		case STATE_SILENT:
		case STATE_OFF:
			output[i] = 0.0f;
		}

		// We continuously play the sine wave regardless of envelope
		self->wave_offset = (self->wave_offset + 1) % self->wave_len;

		// Update elapsed time and start attack if necessary
		if (++self->elapsed_len == frames_per_beat) {
			self->clickstate       = STATE_ATTACK;
			self->elapsed_len = 0;
		}
	}
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
	Alo* self = (Alo*)instance;
	const float* const input  = self->ports.input;
	float sample = 0.0;
	float* const output = self->ports.output;
	float* const recording = self->recording;
	self->threshold = dbToFloat(*self->ports.threshold);

	uint32_t last_t = 0;

	for (uint32_t pos = 0; pos < n_samples; pos++) {
		// recording always happens
		sample = input[pos];
		output[pos] = 0;
//		log("Sample: %.9f", sample);
		recording[self->loop_index] = sample;
		for (int i = 0; i < NUM_LOOPS; i++) {

			if (self->phrase_start[i] && self->phrase_start[i] == self->loop_index) {
				if (self->button_state[i]) {
					self->state[i] = STATE_LOOP_ON;
					log("[%d]PHRASE: LOOP ON [%d]", i, self->loop_index);
                    self->clickstate = STATE_OFF;
				} else {
					if (self->state[i] == STATE_RECORDING) {
						self->phrase_start[i] = 0;
						log("[%d]PHRASE: Abandon phrase [%d]", i, self->loop_index);
					} else {
						self->state[i] = STATE_LOOP_OFF;
						log("[%d]PHRASE: LOOP OFF [%d]", i, self->loop_index);
					}
				}
			}

			if (self->loop_index % (self->loop_samples / self->loop_beats) == 0) {
				if (self->pb_loops > i && self->state[i] != STATE_RECORDING) {
					if (self->button_state[i]) {
						self->state[i] = STATE_LOOP_ON;
						log("[%d]BEAT: LOOP ON [%d]", i, self->loop_index);
					} else {
						self->state[i] = STATE_LOOP_OFF;
						log("[%d]BEAT: LOOP OFF [%d]", i, self->loop_index);
					}
				}
			}

			float* const loop = self->loops[i];
			if (self->state[i] == STATE_RECORDING && self->button_state[i]) {
				loop[self->loop_index] = sample;
				if (self->phrase_start[i] == 0 && self->speed != 0) {
					if (fabs(sample) > self->threshold) {
						self->phrase_start[i] = self->loop_index;
						log("[%d]>>> DETECTED PHRASE START [%d]<<<", i, self->loop_index);
					}
				}
			}

			if (self->state[i] == STATE_LOOP_ON && self->speed != 0) {
				output[pos] += loop[self->loop_index];
			}
		}
		self->loop_index += 1;
		if (self->loop_index >= self->loop_samples) {
			self->loop_index = 0;
		}
	}

	const LV2_Atom_Sequence* midiin = self->ports.midiin;

	for (const LV2_Atom_Event* ev = lv2_atom_sequence_begin(&midiin->body);
		!lv2_atom_sequence_is_end(&midiin->body, midiin->atom.size, ev);

		ev = lv2_atom_sequence_next(ev)) {

    	// Play the click for the time slice from last_t until now
		if (self->clickstate != STATE_OFF) {
			if (self->clickstate != STATE_SILENT) {
				click(self, last_t, ev->time.frames);
		}
		    // Update time for next iteration and move to next event
		    last_t = ev->time.frames;
        }

		if (ev->body.type == self->uris.midi_MidiEvent) {
			const uint8_t* const msg = (const uint8_t*)(ev + 1);
			int i = msg[1] - (uint32_t)floorf(*(self->ports.midi_base));
			if (i >= 0 && i < NUM_LOOPS) {
				if (lv2_midi_message_type(msg) == LV2_MIDI_MSG_NOTE_ON) {
					button_logic(self, true, i);
				}
				if (lv2_midi_message_type(msg) == LV2_MIDI_MSG_NOTE_OFF) {
					button_logic(self, false, i);
				}
				self->midi_control = true;
			}
		}
	}

	if (self->clickstate != STATE_OFF) {
        // Play for remainder of cycle
	    click(self, last_t, n_samples);
    }

	if (self->midi_control == false) {
		for (int i = 0; i < NUM_LOOPS; i++) {
			bool new_button_state = (*self->ports.loops[i]) > 0.0f ? true : false;
			if (new_button_state != self->button_state[i]) {
				button_logic(self, new_button_state, i);
			}
		}
	}

	const AloURIs* uris = &self->uris;

	// from metro.c
	// Work forwards in time frame by frame, handling events as we go
	const LV2_Atom_Sequence* in = self->ports.control;

	for (const LV2_Atom_Event* ev = lv2_atom_sequence_begin(&in->body);
	    !lv2_atom_sequence_is_end(&in->body, in->atom.size, ev);

	    ev = lv2_atom_sequence_next(ev)) {

		// Check if this event is an Object
		// (or deprecated Blank to tolerate old hosts)
		if (ev->body.type == uris->atom_Object || ev->body.type == uris->atom_Blank) {
			const LV2_Atom_Object* obj = (const LV2_Atom_Object*)&ev->body;
			if (obj->body.otype == uris->time_Position) {
				// Received position information, update
				update_position(self, obj);
			}
		}
	}
}

/**
   The `deactivate()` method is the counterpart to `activate()`, and is called by
   the host after running the plugin.  It indicates that the host will not call
   `run()` again until another call to `activate()` and is mainly useful for more
   advanced plugins with ``live'' characteristics such as those with auxiliary
   processing threads.	As with `activate()`, this plugin has no use for this
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
	Alo* self = (Alo*)instance;

	for (int i = 0; i < NUM_LOOPS; i++) {
		free(self->loops[i]);
	}
	free(self->recording);
	free(self);
}

/**
   The `extension_data()` function returns any extension data supported by the
   plugin.  Note that this is not an instance method, but a function on the
   plugin descriptor.  It is usually used by plugins to implement additional
   interfaces.	This plugin does not have any extension data, so this function
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
	case 0:	 return &descriptor;
	default: return NULL;
	}
}
