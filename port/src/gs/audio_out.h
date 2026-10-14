/* The sound output: what the port's sound code (snd_adx.c, snd_se.c, a11y.c) plays through, so that none of it
   needs a particular library. Streams of 16-bit stereo at a rate of their own, mixed by the back end:
     audio_sdl.c       SDL3's audio streams (the PC)
     null/audio_null.c none (AUDIO=none: a build without sound; every Open fails and the players stay silent)
   A stream is either fed by the back end's thread asking for more (AudioOut_StreamFeed) or put to (Put). */
#ifndef AUDIO_OUT_H
#define AUDIO_OUT_H

typedef struct AudioStream AudioStream;

/* Called on the back end's thread with the stream locked: put `additional` more bytes (AudioOut_StreamPut). */
typedef void (*AudioFeed)(void *user, AudioStream *stream, int additional);

int AudioOut_Open(void);            /* the device; 1 = there is one (called again: what the first call found) */
const char *AudioOut_Driver(void);  /* for the log */
const char *AudioOut_Error(void);

AudioStream *AudioOut_StreamCreate(int rate);  /* 16-bit stereo at `rate`, playing on the device; NULL: none */
void AudioOut_StreamFeed(AudioStream *s, AudioFeed feed, void *user); /* (before it is first heard) */
void AudioOut_StreamDestroy(AudioStream *s);   /* waits for a feed under way */
void AudioOut_StreamPut(AudioStream *s, const void *data, int bytes);
void AudioOut_StreamClear(AudioStream *s);     /* drops what is queued */
void AudioOut_StreamGain(AudioStream *s, float gain);
void AudioOut_StreamLock(AudioStream *s);      /* keeps the feed out (NULL: nothing) */
void AudioOut_StreamUnlock(AudioStream *s);

#endif
