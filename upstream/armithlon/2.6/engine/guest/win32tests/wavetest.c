/* wavetest: waveOut plays one second of a 440 Hz tone (22050 Hz, 16-bit stereo) and reports
 * whether it finished. On QRX OS One 64 the tone goes through qrx-win32's mixer (sound_ec64.c) to
 * the 68100EC64 board; qrx_ec64_run --audio-wav records it. */
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

int main(void)
{
    WAVEFORMATEX f = { WAVE_FORMAT_PCM, 2, 22050, 22050 * 4, 4, 16, 0 };
    static short pcm[22050 * 2];
    HWAVEOUT h;
    WAVEHDR hdr;
    int i;
    for (i = 0; i < 22050; i++) {
        /* a triangle wave, no floating point needed */
        int p = (i * 440 * 4 / 22050) % 4, frac = (i * 440 * 4) % 22050;
        int v = p == 0 ? frac : p == 1 ? 22050 - frac : p == 2 ? -frac : frac - 22050;
        pcm[i * 2] = pcm[i * 2 + 1] = (short)(v * 12000 / 22050);
    }
    if (waveOutOpen(&h, WAVE_MAPPER, &f, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) { printf("wavetest: waveOutOpen failed\n"); return 1; }
    memset(&hdr, 0, sizeof hdr);
    hdr.lpData = (LPSTR)pcm;
    hdr.dwBufferLength = sizeof pcm;
    waveOutPrepareHeader(h, &hdr, sizeof hdr);
    waveOutWrite(h, &hdr, sizeof hdr);
    for (i = 0; i < 300 && !(hdr.dwFlags & WHDR_DONE); i++) Sleep(10);
    printf("wavetest: %s after %d ms\n", (hdr.dwFlags & WHDR_DONE) ? "played" : "not done", i * 10);
    waveOutUnprepareHeader(h, &hdr, sizeof hdr);
    waveOutClose(h);
    return (hdr.dwFlags & WHDR_DONE) ? 0 : 2;
}
