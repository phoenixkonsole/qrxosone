#ifndef QRX_HOSTMIDI_H
#define QRX_HOSTMIDI_H
#include <stdbool.h>
#include <stdint.h>
/* msg as in midiOutShortMsg: status | data1 << 8 | data2 << 16. Empty off macOS. */
void qrx_host_midi_short(uint32_t msg);
void qrx_host_midi_sysex(const uint8_t *data, uint32_t len);
void qrx_host_midi_reset(void);
#endif
