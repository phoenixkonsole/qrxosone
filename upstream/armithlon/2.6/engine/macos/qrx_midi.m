/* MIDI from guest programs, played by macOS's built-in DLS synthesizer. */
#import <AudioToolbox/AudioToolbox.h>
#import <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include "qrx_hostmidi.h"

static AUGraph graph;
static AudioUnit synth;
static bool tried, ready;
static atomic_ullong messages;
static atomic_uint peak;

static OSStatus measure(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts,
                        UInt32 bus, UInt32 frames, AudioBufferList *data)
{
    (void)ref; (void)ts; (void)bus;
    if (!(*flags & kAudioUnitRenderAction_PostRender) || !data->mNumberBuffers) return noErr;
    const Float32 *p = data->mBuffers[0].mData;
    if (!p) return noErr;
    unsigned top = atomic_load_explicit(&peak, memory_order_relaxed);
    for (UInt32 i = 0; i < frames; i++) {
        float v = p[i] < 0 ? -p[i] : p[i];
        unsigned u = (unsigned)(v * 32767.0f);
        if (u > top) top = u;
    }
    atomic_store_explicit(&peak, top, memory_order_relaxed);
    return noErr;
}
static bool open_synth(void)
{
    if (tried) return ready;
    tried = true;
    AudioComponentDescription sd = { kAudioUnitType_MusicDevice, kAudioUnitSubType_DLSSynth, kAudioUnitManufacturer_Apple, 0, 0 };
    AudioComponentDescription od = { kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0 };
    AUNode sn, on;
    if (NewAUGraph(&graph) != noErr) return false;
    if (AUGraphAddNode(graph, &sd, &sn) != noErr || AUGraphAddNode(graph, &od, &on) != noErr) return false;
    if (AUGraphOpen(graph) != noErr) return false;
    if (AUGraphConnectNodeInput(graph, sn, 0, on, 0) != noErr) return false;
    if (AUGraphNodeInfo(graph, sn, NULL, &synth) != noErr) return false;
    if (AUGraphInitialize(graph) != noErr) return false;
    AudioUnitAddRenderNotify(synth, measure, NULL);
    if (AUGraphStart(graph) != noErr) return false;
    ready = true;
    if (getenv("QRX_MIDI_LOG")) fprintf(stderr, "[MIDI] DLS synthesizer running\n");
    return true;
}
void qrx_host_midi_short(uint32_t msg)
{
    if (!open_synth()) return;
    atomic_fetch_add(&messages, 1);
    MusicDeviceMIDIEvent(synth, msg & 0xFF, (msg >> 8) & 0x7F, (msg >> 16) & 0x7F, 0);
    if (getenv("QRX_MIDI_LOG") && !(atomic_load(&messages) % 500))
        fprintf(stderr, "[MIDI] %llu messages, peak %u of 32767\n", (unsigned long long)atomic_load(&messages), atomic_load(&peak));
}
void qrx_host_midi_sysex(const uint8_t *data, uint32_t len)
{
    if (!open_synth()) return;
    MusicDeviceSysEx(synth, data, len);
}
void qrx_host_midi_reset(void)
{
    if (!ready) return;
    for (uint32_t ch = 0; ch < 16; ch++) {
        MusicDeviceMIDIEvent(synth, 0xB0 | ch, 123, 0, 0);   /* all notes off */
        MusicDeviceMIDIEvent(synth, 0xB0 | ch, 121, 0, 0);   /* reset controllers */
    }
}
