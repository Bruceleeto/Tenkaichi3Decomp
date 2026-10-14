/* No sound output (audio_out.h; the Makefile's AUDIO=none): there is no device, so no stream is ever made and the
   players behave as with BT3_NOSOUND. */
#include <stddef.h>
#include "../gs/audio_out.h"

int AudioOut_Open(void) { return 0; }
const char *AudioOut_Driver(void) { return "none"; }
const char *AudioOut_Error(void) { return "built without sound (AUDIO=none)"; }
AudioStream *AudioOut_StreamCreate(int rate) { (void)rate; return NULL; }
void AudioOut_StreamFeed(AudioStream *s, AudioFeed feed, void *user) { (void)s; (void)feed; (void)user; }
void AudioOut_StreamDestroy(AudioStream *s) { (void)s; }
void AudioOut_StreamPut(AudioStream *s, const void *data, int bytes) { (void)s; (void)data; (void)bytes; }
void AudioOut_StreamClear(AudioStream *s) { (void)s; }
void AudioOut_StreamGain(AudioStream *s, float gain) { (void)s; (void)gain; }
void AudioOut_StreamLock(AudioStream *s) { (void)s; }
void AudioOut_StreamUnlock(AudioStream *s) { (void)s; }
