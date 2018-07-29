//
// MIT License
//
// Copyright 2018 devcurmudgeon
//
// Permission is hereby granted, free of charge, to any person obtaining a
// copy of this software and associated documentation files (the "Software"),
// to deal in the Software without restriction, including without limitation
// the rights to use, copy, modify, merge, publish, distribute, sublicense
// and/or sell copies of the Software, and to permit persons to whom the
// Software is furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

#include <math.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include <functional>

#include "lv2/lv2plug.in/ns/ext/atom/atom.h"
#include "lv2/lv2plug.in/ns/ext/atom/util.h"
#include "lv2/lv2plug.in/ns/ext/time/time.h"
#include "lv2/lv2plug.in/ns/ext/urid/urid.h"
#include "lv2/lv2plug.in/ns/lv2core/lv2.h"

static const char* ALO_URI = "http://devcurmudgeon.com/alo";
static const size_t STORAGE_SECONDS = 360;
static const size_t NR_OF_BLEND_SAMPLES = 64;
static const bool LOGGING_ENABLED = true;

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


typedef enum
{
    ALO_STATE_IDLE,
    ALO_STATE_RECORDING,
    ALO_STATE_PLAYING
} State;

enum PortIndex
{
    ALO_INPUT = 0,
    ALO_OUTPUT = 1,
    ALO_BUTTON = 2,
    ALO_CONTROL = 3
};

///
/// Convert an input parameter expressed as db into a linear float value
///
static float dbToFloat(float db)
{
    if (db <= -90.0f)
        return 0.0f;
    return powf(10.0f, db * 0.05f);
}

class Loop
{
public:
    /// Loop's audio memory start point in the global audio storage
    size_t m_storageOffset = 0;
    /// The length of the loop
    size_t m_length = 0;
    size_t m_startIndex = 0;
};

class MomentaryButton
{
public:
    // Connect the button to an input and set the callback.
    void connect(void* input, std::function<void (bool, double, bool)> callback)
    {
        m_input = static_cast<const float*>(input);
        m_callback = callback;
    }

    /// To be called every run call of the plugin. Will check the state of the
    /// button and call the callback if necessary.
    ///
    /// \param now The current time. Used for checking for double clicks.
    void run(double now)
    {
        if (m_input == NULL)
            return;

        bool state = (*m_input) > 0.0f ? true : false;
        if (state == m_lastState)
            return;
        m_lastState = state;
        bool doubleClick = false;
        if (state)
        {
            if (now - m_lastClickTime < 1)
                doubleClick = true;
            m_lastClickTime = now;
        }
        m_callback(state, now - m_lastChangeTime, doubleClick);
        m_lastChangeTime = now;
    }

    /// The callback
    /// \param bool pressed Is the button pressed or released?
    /// \param double How long since the last state change?
    /// \param doubleClick Was this pressed twice within a second?
    std::function<void (bool, double, bool)> m_callback;
    /// The input it is connected to.
    const float* m_input = NULL;
    /// The last state, used for supressing multiple callbacks.
    bool m_lastState = false;
    /// When was the last change
    double m_lastChangeTime = 0;
    /// Used for detecting double clicks.
    double m_lastClickTime = 0;
};

///
/// The looper class
///
class Looper
{
public:
    /// Constructor
    /// \param sampleRate The sample rate is used for calculation of the storage needed
    ///                   and also the current time.
    Looper(double sampleRate)
        : m_sampleRate(sampleRate)
    {
        // Allocate the needed memory
        m_storageSize = sampleRate * STORAGE_SECONDS * 2;
        m_storage = new float[m_storageSize];

        if (LOGGING_ENABLED)
            m_logFile = fopen("/root/alo.log", "wb");
		log("Hello");
    }

    // Destructor
    ~Looper()
    {
        delete[] m_storage;
        if (m_logFile != NULL)
            fclose(m_logFile);
    }

    /// Called by the host for each port to connect it to the looper.
    /// \param port The index of the port to be connected.
    /// \param data A pointer to the data where the parameter will be written to.
    void connectPort(PortIndex port, void* data)
    {
        log("ConnectPort %d", port);
        // Install the ports
        switch (port)
        {
            case ALO_INPUT: m_input = (const float*)data; return;
            case ALO_OUTPUT: m_output = (float*)data; return;
            case ALO_CONTROL: m_control = (LV2_Atom_Sequence*)data; return;
            default: break;
        }

        // Install the buttons and set their callback functions.
        if (port == ALO_BUTTON)
        {
            m_Button.connect(data, [this](bool pressed, double interval, bool doubleClick)
            {
                if (!pressed)
                    log("ConnectPort pressed");
                    return;
                if (doubleClick)
                {
                    log("ConnectPort doubleClick");
                    reset();
                    return;
                }

                log("ConnectPort check recording");
                if (m_state == ALO_STATE_RECORDING)
                    finishRecording();
                else
                    startRecording();
            });
        }
    }

    /// Run the looper. Called for a bunch of samples at a time. Parameters will not change within this
    /// call!
    /// \param The number of samples to be read from the input and writte to the output.
    void run(uint32_t nrOfSamples)
    {
        updateParameters();

        m_now += double(nrOfSamples) / m_sampleRate;
        if (m_state == ALO_STATE_IDLE)
        {
            for (uint32_t s = 0; s < nrOfSamples; ++s)
            {
                m_output[s] = m_input[s];
            }
            return;
        }

        for (uint32_t s = 0; s < nrOfSamples; ++s)
        {
            // Use the live input
            float in = m_input[s];
            Loop& loop = m_loops[m_nrOfLoops];
            loop.m_startIndex = m_currentLoopIndex;
            m_state = ALO_STATE_RECORDING;

            // If we are recoding do the record.
            if (m_state == ALO_STATE_RECORDING)
            {
                m_storage[m_nrOfUsedSamples] = in;
                m_nrOfUsedSamples++;
                Loop& loop = m_loops[m_nrOfLoops];
                loop.m_length++;
            }

            // Playback all active loops.
            float out = m_dryAmount * in;
            for (size_t t = 0; t < m_nrOfLoops; t++)
            {
                Loop& loop = m_loops[t];
                if (m_currentLoopIndex < loop.m_startIndex)
                    continue;
                if (m_currentLoopIndex >= loop.m_startIndex + loop.m_length)
                    continue;
                size_t index = loop.m_storageOffset + (m_currentLoopIndex - loop.m_startIndex);
                out += m_storage[index];
            }

            // Store accumulated output.
            m_output[s] = out;

            if (m_nrOfLoops > 0)
                // Only once we are actually playing anything the loop length is known.
                m_currentLoopIndex++;

            // At the end increment the loop index and check if we are at the end
            // of the loop. The first loop governs the length of the whole loop.
            // So if still recording when we reach the end of the loop, we stop
            // the recording! Note that if we don't have a loop, yet, then m_loopLength
            // is 0, so no extra check is needed.
            if (m_currentLoopIndex > m_loopLength || m_nrOfUsedSamples >= m_storageSize)
            {
                // Reached the end of the loop, either because we exhausted storage
                // or the end of the loop is there.
                m_currentLoopIndex = 0;

                if (m_state == ALO_STATE_RECORDING)
                    // Stop the recording only, if we did not have the threshold, yet.
                    // That allows to start recording right at the start of the loop.
                    finishRecording();
            }
        }
    }

private:
    //
    // Input parameters
    //

    /// Threshold parameter
    const float* m_thresholdParameter = NULL;

    /// Dry amount parameter
    const float* m_dryAmountParameter = NULL;

    /// All-purpose button
    MomentaryButton m_Button;

    const float* m_input = NULL;
    float* m_output = NULL;
	LV2_Atom_Sequence* m_control = NULL;

	LV2_URID_Map* map;   // URID map feature
	AloURIs     uris;  // Cache of mapped URIDs

    //
    // Internal state
    //

    /// The stored sample rate
    uint32_t m_sampleRate = 48000;
    /// The current looper state
    State m_state = ALO_STATE_IDLE;
    /// The stored threshold as a linear value
    float m_threshold = 0.0f;
    /// The stored dry amount
    float m_dryAmount = 1.0f;
    /// Where are we with the first (main) loop. The first loop governs all the loops!
    size_t m_currentLoopIndex = 0;
    /// The lenght of the main loop
    size_t m_loopLength = 0;
    /// Current time, sample accurate used for buttons
    double m_now = 0;

    //
    // Storage memory for audio
    //

    /// Overall storage size for audio (number of floats per channel)
    size_t m_storageSize = 0;
    /// Number of samples already used
    size_t m_nrOfUsedSamples = 0;
    float* m_storage = NULL;

    /// The number of loops currently active
    size_t m_nrOfLoops = 0;
    /// The number of loops which we recorded and thus could be redone
    size_t m_maxUsedLoops = 0;
    /// The loops
    Loop m_loops[1];

    /// If we want to log to a file, we can use this.
    FILE* m_logFile = NULL;

    /// Log function (printf-style)
    void log(const char *formatString, ...)
    {
        if (!LOGGING_ENABLED)
            return;
        if (m_logFile == NULL)
            return;

        char buffer[2048];
        va_list argumentList;
        va_start(argumentList, formatString);
        vsnprintf(&buffer[0], sizeof(buffer), formatString, argumentList);
        va_end(argumentList);
        fwrite(buffer, 1, strlen(buffer), m_logFile);
        fprintf(m_logFile, "\n");
        fflush(m_logFile);
    }

    /// Reset everything to initial state.
    void reset()
    {
        m_nrOfLoops = 0;
        m_maxUsedLoops = 0;
        m_nrOfUsedSamples = 0;
        m_state = ALO_STATE_IDLE;
        m_currentLoopIndex = 0;
        m_loopLength = 0;
        m_nrOfUsedSamples = 0;
    }

    /// Start recording a loop if possible (a loop and memory for audio left).
    void startRecording()
    {
        log("startRecording");
        if (m_nrOfUsedSamples >= m_storageSize)
            // Memory full, cannot start recording.
            return;

        // Prepare the loop.
        Loop& loop = m_loops[m_nrOfLoops];
        loop.m_storageOffset = m_nrOfUsedSamples;
        loop.m_length = 0;

        // Now start the recording.
        m_state = ALO_STATE_RECORDING;
    }

    /// Finish the recording.
    void finishRecording()
    {
        log("finishRecording");
        // We did record something, so make sure we will use it.
        m_state = ALO_STATE_PLAYING;
        Loop& loop = m_loops[m_nrOfLoops];
        if (m_nrOfLoops == 0)
        {
            // This was the first loop which governs the loop length.
            m_loopLength = loop.m_length;
            m_currentLoopIndex = 0;
        }

        // Fixup the start and the end of the loop. We simply fade in and out over
        // 32 samples for now. Not sure if that's good for everything, seems to work
        // nicely enough, though.
        size_t length = loop.m_length > NR_OF_BLEND_SAMPLES ? NR_OF_BLEND_SAMPLES : loop.m_length;
        size_t startIndex = loop.m_storageOffset;
        size_t endIndex = loop.m_storageOffset + loop.m_length - 1;
        for (size_t s = 0; s < length; s++)
        {
            float factor = float(s) / length;
            m_storage[startIndex] *= factor;
            startIndex++;
            m_storage[endIndex] *= factor;
            endIndex--;
        }

        // Now the loop is officially ready for playing...
        m_nrOfLoops++;

        // Note that when recording a new loop we need to reset max loops as well, even if
        // once had more loops: They have been overwritten and cannot be redone!
        m_maxUsedLoops = m_nrOfLoops;
    }

    /// Update all the parameters from the inputs.
    void updateParameters()
    {
        m_Button.run(m_now);
    }
};

//
// Functions required by the LV2 interface. Forward to the Looper class.
//
static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, const char* bundlePath,
    const LV2_Feature* const* features)
{
    return (LV2_Handle)new Looper(rate);
}
static void activate(LV2_Handle instance) {}
static void deactivate(LV2_Handle instance) {}
static void cleanup(LV2_Handle instance) { delete static_cast<Looper*>(instance); }
static const void* extensionData(const char* uri) { return NULL; }
static void connectPort(LV2_Handle instance, uint32_t port, void* data)
{
    Looper* looper = static_cast<Looper*>(instance);
    looper->connectPort(static_cast<PortIndex>(port), data);
}
static void run(LV2_Handle instance, uint32_t nrOfSamples)
{
    Looper* looper = static_cast<Looper*>(instance);
    looper->run(nrOfSamples);
}

///
/// Descriptors for the various functions called by the LV2 host
///
static const LV2_Descriptor descriptor =
{
    /// The URI which identifies the plugin
    ALO_URI,
    /// Instantiate the plugin.
    instantiate,
    /// Connect a port, called once for each port.
    connectPort,
    /// Activate the plugin (unused).
    activate,
    /// Process a bunch of samples.
    run,
    /// Deactivate the plugin (unused).
    deactivate,
    /// Cleanup, will destroy the plugin.
    cleanup,
    /// Get information about used extensions (unused).
    extensionData
};

///
/// DLL entry point which is called with index 0.. until it returns NULL.
///
LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index)
{
    switch (index)
    {
        case 0:  return &descriptor;
        default: return NULL;
    }
}

