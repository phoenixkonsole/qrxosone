/* QRX OS One 22.5.12 - stand-ins for the parts compiled out of the qrx-win32
 * PrBoom-plus build.  Each one reports "not available", so PrBoom-plus takes its
 * normal no-sound / no-joystick / no-music / no-ENDOOM paths:
 *   SDL audio + joystick (SDL built with SDL_AUDIO_DISABLED, SDL_JOYSTICK_DISABLED),
 *   music players (no fluidsynth/mad/vorbis/portmidi/dumb/OPL), PC speaker sound,
 *   textscreen (ENDOOM). */
#include <stddef.h>
#include "SDL.h"
#include "MUSIC/musicplayer.h"

/* SDL audio */
int SDL_OpenAudio(SDL_AudioSpec *desired, SDL_AudioSpec *obtained) { (void)desired; (void)obtained; SDL_SetError("audio not available in QRX"); return -1; }
void SDL_CloseAudio(void) {}
void SDL_PauseAudio(int pause_on) { (void)pause_on; }
void SDL_LockAudio(void) {}
void SDL_UnlockAudio(void) {}
void SDL_Audio_SetCaption(const char *caption) { (void)caption; }

/* SDL joystick: none present */
int SDL_NumJoysticks(void) { return 0; }
const char *SDL_JoystickName(int i) { (void)i; return NULL; }
SDL_Joystick *SDL_JoystickOpen(int i) { (void)i; return NULL; }
Sint16 SDL_JoystickGetAxis(SDL_Joystick *j, int a) { (void)j; (void)a; return 0; }
Uint8 SDL_JoystickGetButton(SDL_Joystick *j, int b) { (void)j; (void)b; return 0; }
int SDL_JoystickNumAxes(SDL_Joystick *j) { (void)j; return 0; }
int SDL_JoystickNumButtons(SDL_Joystick *j) { (void)j; return 0; }
void SDL_JoystickClose(SDL_Joystick *j) { (void)j; }
void SDL_JoystickUpdate(void) {}
int SDL_JoystickEventState(int s) { (void)s; return 0; }

/* music players: all fail their init, PrBoom-plus then plays no music */
static const char *np_name(void) { return "none (QRX)"; }
static int np_init(int sr) { (void)sr; return 0; }
static void np_void(void) {}
static void np_vol(int v) { (void)v; }
static const void *np_reg(const void *d, unsigned n) { (void)d; (void)n; return NULL; }
static void np_unreg(const void *h) { (void)h; }
static void np_play(const void *h, int l) { (void)h; (void)l; }
static void np_render(void *d, unsigned n) { (void)d; (void)n; }
#define NOPLAYER(n) const music_player_t n = { np_name, np_init, np_void, np_vol, np_void, np_void, np_reg, np_unreg, np_play, np_void, np_render }
NOPLAYER(vorb_player); NOPLAYER(mp_player); NOPLAYER(db_player); NOPLAYER(fl_player); NOPLAYER(opl_synth_player); NOPLAYER(pm_player);

/* PC speaker sound */
int I_PCS_StartSound(int id, int channel, int vol, int sep, int pitch, int priority) { (void)id; (void)channel; (void)vol; (void)sep; (void)pitch; (void)priority; return -1; }
void I_PCS_StopSound(int handle) { (void)handle; }
int I_PCS_SoundIsPlaying(int handle) { (void)handle; return 0; }
void I_PCS_InitSound(void) {}
void PCSound_Mix_Callback(void *udata, Uint8 *stream, int len) { (void)udata; (void)stream; (void)len; }

/* textscreen (ENDOOM): not shown */
int TXT_Init(void) { return 0; }
void TXT_Shutdown(void) {}
unsigned char *TXT_GetScreenData(void) { static unsigned char s[80 * 25 * 2]; return s; }
void TXT_UpdateScreen(void) {}
int TXT_GetChar(void) { return -1; }
void TXT_Sleep(int timeout) { (void)timeout; }
