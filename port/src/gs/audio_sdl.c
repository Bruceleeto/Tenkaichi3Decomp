/* The sound output on SDL3 (audio_out.h): one device, one of SDL's streams per AudioStream (SDL changes the rate). */
#include <SDL3/SDL.h>
#include "audio_out.h"

static SDL_AudioDeviceID sDevice;
static int sTried;

int AudioOut_Open(void) {
    if (!sTried) {
        sTried = 1;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            sDevice = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, NULL);
            if (sDevice != 0) {
                SDL_ResumeAudioDevice(sDevice);
            }
        }
    }
    return sDevice != 0;
}

const char *AudioOut_Driver(void) { return SDL_GetCurrentAudioDriver(); }
const char *AudioOut_Error(void) { return SDL_GetError(); }

struct AudioStream {
    SDL_AudioStream *s;
    AudioFeed feed;
    void *user;
};

AudioStream *AudioOut_StreamCreate(int rate) {
    SDL_AudioSpec spec;
    AudioStream *a;

    if (sDevice == 0) {
        return NULL;
    }
    spec.format = SDL_AUDIO_S16;
    spec.channels = 2;
    spec.freq = rate;
    a = SDL_calloc(1, sizeof(*a));
    a->s = SDL_CreateAudioStream(&spec, NULL);
    if (a->s == NULL || !SDL_BindAudioStream(sDevice, a->s)) {
        SDL_DestroyAudioStream(a->s);
        SDL_free(a);
        return NULL;
    }
    return a;
}

static void SDLCALL feed_cb(void *user, SDL_AudioStream *stream, int additional, int total) {
    AudioStream *a = user;
    (void)stream; (void)total;
    a->feed(a->user, a, additional);
}

void AudioOut_StreamFeed(AudioStream *a, AudioFeed feed, void *user) {
    if (a != NULL) {
        a->feed = feed;
        a->user = user;
        SDL_SetAudioStreamGetCallback(a->s, feed_cb, a);
    }
}

void AudioOut_StreamDestroy(AudioStream *a) {
    if (a != NULL) {
        SDL_DestroyAudioStream(a->s); /* waits for a running callback */
        SDL_free(a);
    }
}

void AudioOut_StreamPut(AudioStream *a, const void *data, int bytes) { if (a != NULL) { SDL_PutAudioStreamData(a->s, data, bytes); } }
void AudioOut_StreamClear(AudioStream *a) { if (a != NULL) { SDL_ClearAudioStream(a->s); } }
void AudioOut_StreamGain(AudioStream *a, float gain) { if (a != NULL) { SDL_SetAudioStreamGain(a->s, gain); } }
void AudioOut_StreamLock(AudioStream *a) { if (a != NULL) { SDL_LockAudioStream(a->s); } }
void AudioOut_StreamUnlock(AudioStream *a) { if (a != NULL) { SDL_UnlockAudioStream(a->s); } }
