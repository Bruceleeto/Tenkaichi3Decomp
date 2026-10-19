/*
 * Movies: Sony's libmpeg (sceMpeg*) as the game's movie player uses it (src/sys/movie.c).
 *
 * On the PS2 the game reads the .PSS file itself, hands the bytes to the library to split off the video stream,
 * and asks for one decoded picture per frame. A picture comes back as 16 x 16 blocks of RGBA, columns first, which
 * the game uploads straight into the frame buffer. The sound is a separate ADX file (port/src/gs/snd_adx.c).
 *
 * Where the renderer has the movie in a texture of its own (Rnd_MovieOn: the PVR's, on the Dreamcast and the PC's
 * simulation of it), the pictures come from pl_mpeg (port/third_party/pl_mpeg) reading the movie converted to MPEG-1
 * (port/tools/dc_movie.sh, `make -f Makefile.dc dc-assets`: NAME.mpg beside tex.pak): each sceMpegGetPicture decodes
 * one into that texture, and the game draws it as a record (Movie_Run). Elsewhere they come from the port's own
 * decoder (plat_mpeg2.c: both movies are intra-only MPEG-2) reading the PSS file, each rearranged into the block order
 * the game uploads. Either way the movie file is the one the game opened last (the path from plat_file.c) and the
 * bytes the game feeds to sceMpegDemuxPss are ignored. Without a window a movie ends at once, except when recorded
 * input is played back (the movie has to take the same number of frames).
 *
 * BT3_NO_MOVIE (the Dreamcast's build unless `make -f Makefile.dc DC_MOVIE=1`): no pl_mpeg; where the renderer would
 * have the movie, a movie ends at once (saying so once).
 */
extern int Port_NetSession(void); /* gs/net.c: no movies in an online session */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOVIE_W 512
#define MOVIE_H 448

#ifndef BT3_NO_MOVIE
#define PL_MPEG_IMPLEMENTATION
#ifdef __DREAMCAST__
#include <kos.h>
#include <malloc.h>
/* the hooks the Dreamcast's pl_mpeg takes from its player (Quake2-main's client/mpeg.h) */
#define MPEG_MALLOC(sz) malloc(sz)
#define MPEG_FREE(p) free(p)
#define MPEG_REALLOC(p, sz) realloc((p), (sz))
#define MPEG_MEMALIGN(a, sz) memalign((a), (sz))
#define MPEG_MEMZERO(p, sz) memset((p), 0, (sz))
#define MPEG_FILE_TYPE file_t
#define MPEG_FILE_INVALID_HANDLE FILEHND_INVALID
#define MPEG_FILE_OPEN(fn) fs_open((fn), O_RDONLY)
#define MPEG_FILE_CLOSE(fh) fs_close((fh))
#define MPEG_FILE_SEEK(fh, off, st) fs_seek((fh), (off), (st))
#define MPEG_FILE_READ(fh, buf, size) fs_read((fh), (buf), (size))
#define MPEG_FILE_TELL(fh) fs_tell((fh))
#include "../third_party/pl_mpeg/pl_mpeg_dc.h"
extern const char *Port_FileRoot(void); /* plat_file.c: /cd, else /pc */
#define MOVIE_DIR Port_FileRoot(), "/"
extern void GsPvr_MovieMacroblocks(const uint32_t *mb);
#else
#include "../third_party/pl_mpeg/pl_mpeg.h"
#define MOVIE_DIR "port/build/dc/assets", "/"
extern void GsPvr_MovieYuv(const uint8_t *py, const uint8_t *pcb, const uint8_t *pcr, int ys, int cs);
#endif
extern int GsPvr_MovieBegin(int w, int h); /* gs/gs_pvr.c: the movie's texture */
extern void GsPvr_MovieEnd(void);
static plm_t *sPlm;
#endif
extern int Rnd_MovieOn(void);      /* gs/gs_draw.c */

extern char gPortMoviePath[512];
extern int GsGpu_Enabled(void);

typedef struct Mpeg2 Mpeg2;
extern Mpeg2 *Mpeg2_Open(const char *path);
extern int Mpeg2_Next(Mpeg2 *m, uint8_t *rgba);
extern int Mpeg2_Width(const Mpeg2 *m);
extern int Mpeg2_Height(const Mpeg2 *m);
extern void Mpeg2_Close(Mpeg2 *m);

static Mpeg2 *sMovie;
static int sStarted, sEnd;
#ifndef __DREAMCAST__
static uint8_t sFrame[MOVIE_W * MOVIE_H * 4];
#endif

static void movie_close(void) {
#ifndef BT3_NO_MOVIE
    if (sPlm != NULL) {
        plm_destroy(sPlm);
        sPlm = NULL;
        GsPvr_MovieEnd();
    }
#endif
    if (sMovie != NULL) {
        Mpeg2_Close(sMovie);
        sMovie = NULL;
    }
    sStarted = 0;
    sEnd = 0;
}

static void movie_start(void) {
    sStarted = 1;
    sEnd = 1;
    /* without a window too when recorded input is played back: the movie has to take the same number of frames */
    if ((!GsGpu_Enabled() && getenv("BT3_PAD_PLAY") == NULL) || gPortMoviePath[0] == '\0' || getenv("BT3_NOMOVIE") != NULL || Port_NetSession()) {
        return;
    }
    if (Rnd_MovieOn()) { /* NAME.mpg in MOVIE_DIR for the disc's NAME.PSS */
#ifdef BT3_NO_MOVIE
        static int said;
        if (!said) {
            said = 1;
            fprintf(stderr, "bt3: movies are off in this build (make -f Makefile.dc DC_MOVIE=1 plays them)\n");
        }
        return;
#else
        char path[600];
        const char *name = strrchr(gPortMoviePath, '/') != NULL ? strrchr(gPortMoviePath, '/') + 1 : gPortMoviePath;
        size_t n = strcspn(name, ".");
        snprintf(path, sizeof(path), "%s%s%.*s.mpg", MOVIE_DIR, (int)n, name);
        sPlm = plm_create_with_filename(path);
        if (sPlm == NULL || !plm_has_headers(sPlm) || !GsPvr_MovieBegin(plm_get_width(sPlm), plm_get_height(sPlm))) {
            fprintf(stderr, "bt3: ERROR: the movie %s is not played: %s\n", path,
                    sPlm == NULL ? "not there (make -f Makefile.dc dc-assets converts the disc's)" :
                    !plm_has_headers(sPlm) ? "not an MPEG-1 program stream" : plm_get_width(sPlm) > 512 || plm_get_height(sPlm) > 512 ||
                    (plm_get_width(sPlm) | plm_get_height(sPlm)) & 15 ? "not multiples of 16 up to 512 x 512" : "no PVR memory for its texture");
            if (sPlm != NULL) {
                plm_destroy(sPlm);
                sPlm = NULL;
            }
            return;
        }
        plm_set_audio_enabled(sPlm, 0); /* (its sound is the game's ADX) */
        fprintf(stderr, "bt3: movie %s: %d x %d\n", path, plm_get_width(sPlm), plm_get_height(sPlm));
        sEnd = 0;
        return;
#endif
    }
#ifndef __DREAMCAST__
    sMovie = Mpeg2_Open(gPortMoviePath);
    sEnd = sMovie == NULL;
#endif
}

int sceMpegInit(void) { return 0; }
int sceMpegCreate(void *mp, void *work, int size) { (void)mp; (void)work; (void)size; movie_close(); return 0; }
int sceMpegDelete(void *mp) { (void)mp; movie_close(); return 0; }
int sceMpegReset(void *mp) { (void)mp; movie_close(); return 0; }
void *sceMpegAddCallback(void *mp, int type, void *cb, void *data) { (void)mp; (void)type; (void)cb; (void)data; return NULL; }
void *sceMpegAddStrCallback(void *mp, int type, int ch, void *cb, void *data) { (void)mp; (void)type; (void)ch; (void)cb; (void)data; return NULL; }
int sceMpegDemuxPss(void *mp, void *data, int size) { (void)mp; (void)data; (void)size; return 0; }

int sceMpegIsEnd(void *mp) {
    (void)mp;
    if (!sStarted) {
        movie_start();
    }
    return sEnd;
}

/* One picture into `rgb`: `blocks` 16 x 16 RGBA blocks of 0x400 bytes, columns first (the order the game's upload
   chain expects); alpha 0x80. */
int sceMpegGetPicture(void *mp, void *rgb, int blocks) {
    uint8_t *out = rgb;
#ifndef __DREAMCAST__
    int cols = MOVIE_W / 16, rows = MOVIE_H / 16, i, j, y, x;
#endif

    (void)mp;
    if (!sStarted) {
        movie_start();
    }
#ifndef BT3_NO_MOVIE
    if (sPlm != NULL) { /* into the renderer's texture */
        plm_frame_t *f = sEnd ? NULL : plm_decode_video(sPlm);
        if (f == NULL) {
            sEnd = 1;
            return -1;
        }
#ifdef __DREAMCAST__
        GsPvr_MovieMacroblocks(f->display);
#else
        GsPvr_MovieYuv(f->y.data, f->cb.data, f->cr.data, (int)f->y.width, (int)f->cb.width);
#endif
        return 0;
    }
#endif
#ifdef __DREAMCAST__
    (void)out;
    (void)blocks;
    sEnd = 1;
    return -1;
#else
    if (sMovie == NULL || !Mpeg2_Next(sMovie, sFrame) || Mpeg2_Width(sMovie) != MOVIE_W || Mpeg2_Height(sMovie) != MOVIE_H) {
        sEnd = 1;
        return -1;
    }
    for (i = 0; i < cols; i++) {
        for (j = 0; j < rows && i * rows + j < blocks; j++) {
            uint8_t *b = out + (size_t)(i * rows + j) * 0x400;
            for (y = 0; y < 16; y++) {
                const uint8_t *s = &sFrame[((j * 16 + y) * MOVIE_W + i * 16) * 4];
                for (x = 0; x < 16; x++) {
                    b[(y * 16 + x) * 4 + 0] = s[x * 4 + 0];
                    b[(y * 16 + x) * 4 + 1] = s[x * 4 + 1];
                    b[(y * 16 + x) * 4 + 2] = s[x * 4 + 2];
                    b[(y * 16 + x) * 4 + 3] = 0x80;
                }
            }
        }
    }
    return 0;
#endif
}
