/*
 * Character packages: characters from outside the disc. The standard is docs/port/character_packages.md.
 *
 * `characters/<Name>/` next to the game ($BT3_CHARACTERS names another folder) holds one character, files in the
 * game's own format named by what they are (the endings and names without regard to case):
 *
 *   *_1p.pak .. *_4p.pak            the model of costume 1..4       (the disc's file 0x590 + id * 10 + 0..3)
 *   *_1p_dmg.pak .. *_4p_dmg.pak    the damaged model of the same   (+ 4..7)
 *   *_anm.pak, *_eff.pak            animations, effects             (+ 8, + 9)
 *   *_voice_jp.pak, *_voice_us.pak  the voice bank in each language (0xBDA + id, 0xC7B + id)
 *   voice_jp/NNN.adx, voice_us/NNN.adx   the spoken lines           (0x8D4E + id * 100 + line, 0xCC32 + ...)
 *   portrait.bin                    the large picture of the menus  (0x2F9 + id)
 *   member_1p.bin, member_2p.bin    the fight's member block        (8 + id * 2 + side)
 *   chip.bin                        the select's chip (one entry of the select's chip pack)
 *   character.txt                   the manifest: key=value lines (name, form, base, replaces, links, ...)
 *   name.txt, form.txt, base.txt, forms.txt   the same things in files of their own (the older way)
 *
 * A folder either ADDS a character or, with `replaces=<id>` in its manifest, REPLACES one of the disc's:
 *
 * - Added: the folders, by name, become the characters 0xC6, 0xC7, ... (include/port_chara.h). The game asks for
 *   an added character's files by ids past the end of the second archive, and they are answered here: with the
 *   folder's file, or where it has none with the file of the character's BASE character, one of the disc's,
 *   which also stands for it in every table (cost, AI, what is said to whom).
 * - Replacing: the folder's files are answered for the disc character's own file numbers, ahead of
 *   gamedata/mods/; what the folder does not have stays the disc's. It takes no id.
 *
 * Nothing here is part of the game's saved state, and in an online session all of it answers as if there were no
 * folder (live()).
 */
#include <ctype.h>
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "plat_charas.h"

extern int PortExtras_Dir(const char *kind, const char *env, char *out, unsigned size); /* plat_extras.c */

/* F_VOICE is the bank 0xBDA + id, the Japanese one ("_voice_jp"); F_VOICE + 1 is 0xC7B + id, the English one
   ("_voice_us"), which the game takes when SAVE_FLAG_VOICE is set, as it is in a new save. (The names are the
   community's: its file lists call the disc's file 0xBDA + id "<Name>_Voice_JP.pak" and 0xC7B + id
   "<Name>_Voice_US.pak"; until 2026-10-13 this file had them the other way round.) */
enum { F_ANM = 8, F_EFF = 9, F_VOICE = 10, F_COUNT = 12 };
enum { SLOTS = 19 };
enum { DISC_CHARAS = 161 };

typedef struct {
    char name[64];             /* the folder's name */
    char owner[64];            /* "" or, for a folder inside a character's folder, that folder's name */
    int ownerId;               /* ... and its character id (an added one's, or the disc id it replaces); -1 */
    char file[F_COUNT][400];   /* "" where the folder has none */
    int base;
    int costumes;
    int replaces;              /* -1: an added character; 0..160: the disc character whose place it takes */
    char chip[400], portrait[400]; /* "" where the folder has none */
    char member[2][400];
    char lines[2][400];        /* the folders of spoken lines: [0] voice_jp, [1] voice_us */
    char dir[400];             /* the folder */
    char form[SLOTS][64];      /* the links: the folder (or "disc:<id>") of each slot; "" = not said. 0..3 the
                                  transformations, 4..6 the fusions, 7 + n * 4 + m partner m of fusion n */
    int target[SLOTS];         /* ... as an id: -1 not said, 0xFF not installed */
    char shown[64];            /* the name for the menus: the manifest's, or the folder's name */
    int nameSet;               /* a name was given (a replacement without one keeps the disc's line) */
    char label[96];            /* the form's label under the name ("Super Saiyan") */
    int labelSet;              /* a label was given (an empty one: no label); without, the base's label */
    int stat[PORT_STAT_COUNT]; /* health, ki, blast stocks: -1 not said */
    int pkg;                   /* its entry in sPkg, or -1 */
} Chara;

enum { CHIP_SIZE = 5504, NAME_ENTRY = 128, NAME_LINES_MAX = 256, PKG_MAX = 320, TEXT_MAX = 16384 };
/* (static: the program lies below 4 GB, where the game's 32-bit pointers reach) */
static unsigned char sChip[PORT_CHARA_MAX][CHIP_SIZE] __attribute__((aligned(64)));
static unsigned char sChipHave[PORT_CHARA_MAX];
static unsigned char sNames[64 + 4 * (NAME_LINES_MAX + PORT_CHARA_MAX + 2) + (NAME_LINES_MAX + PORT_CHARA_MAX) * NAME_ENTRY] __attribute__((aligned(64)));
static int sNameFirst = -1; /* the line of the first added character in sNames */
static unsigned char sForms[sizeof(sNames)] __attribute__((aligned(64))); /* the same for the forms' labels */
static int sFormFirst = -1;

static Chara sChara[PORT_CHARA_MAX]; /* the added characters: sChara[n] is character PORT_CHARA_FIRST + n */
static int sCount;
static Chara sRepl[DISC_CHARAS];     /* the replacements */
static int sReplCount;
static short sReplOf[DISC_CHARAS];   /* disc id -> its entry of sRepl + 1; 0 not replaced */
static PortCharaPkg sPkg[PKG_MAX];   /* every folder looked at that is, or was meant to be, a package */
static int sPkgCount;
static int sInit;

/* Online play does not know character packages. In a session (Dragon Net Battle; gs/net.c) everything this file
   answers is what it answers with no `characters` folder at all: no character, no cell, no name line, no chip, no
   file of a folder, no number of a manifest, no memory of the game's taken, the original's model buffers; so the
   two copies of a match compute the same game whatever each player has installed. A session that starts the
   program again does not read the folder either (PortCharas_Init). One that begins in the running program
   (gs/state.c) finds the folder read: live() and rlive() hide it until the session is over. */
extern int Port_NetSession(void); /* gs/net.c */
static int live(void) {
    return Port_NetSession() ? 0 : sCount;
}
static int rlive(void) {
    return Port_NetSession() ? 0 : sReplCount;
}

/* the package that replaces disc character `id`, NULL if none (always NULL in a session) */
static const Chara *repl_of(int id) {
    return id >= 0 && id < DISC_CHARAS && rlive() > 0 && sReplOf[id] != 0 ? &sRepl[sReplOf[id] - 1] : NULL;
}

/* the package behind a character id: an added character, or the replacement of a disc character; NULL if none */
static const Chara *chara_of(int chara) {
    if (chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + live()) {
        return &sChara[chara - PORT_CHARA_FIRST];
    }
    return repl_of(chara);
}

static int id_of(const Chara *c) {
    return c->replaces >= 0 ? c->replaces : PORT_CHARA_FIRST + (int)(c - sChara);
}

/* 1 while what the game's file numbers answer may differ from the data folder's: flips with a session's start and
   end, and the open files are then looked up again (plat_fcache.c). */
int PortCharas_Epoch(void) {
    return Port_NetSession() ? 1 : 0;
}

/* ---- the folder scan ---------------------------------------------------------------------------------------- */

static int same_nocase(const char *a, const char *b) {
    for (; *a != '\0' && tolower((unsigned char)*a) == tolower((unsigned char)*b); a++, b++) {
    }
    return *a == '\0' && *b == '\0';
}

/* which of the twelve files a name is, by its ending; -1 if none */
static int kind_of(const char *name) {
    static const struct { const char *end; int kind; } kEnds[] = {
        {"_1p.pak", 0}, {"_2p.pak", 1}, {"_3p.pak", 2}, {"_4p.pak", 3},
        {"_1p_dmg.pak", 4}, {"_2p_dmg.pak", 5}, {"_3p_dmg.pak", 6}, {"_4p_dmg.pak", 7},
        {"_anm.pak", F_ANM}, {"_eff.pak", F_EFF}, {"_voice_jp.pak", F_VOICE}, {"_voice_us.pak", F_VOICE + 1},
    };
    size_t l = strlen(name), i, k;
    for (i = 0; i < sizeof(kEnds) / sizeof(kEnds[0]); i++) {
        size_t el = strlen(kEnds[i].end);
        if (l < el) {
            continue;
        }
        for (k = 0; k < el && tolower((unsigned char)name[l - el + k]) == kEnds[i].end[k]; k++) {
        }
        if (k == el) {
            return kEnds[i].kind;
        }
    }
    return -1;
}

static int name_cmp(const void *a, const void *b) {
    const unsigned char *x = a, *y = b;
    for (; *x != '\0' && tolower(*x) == tolower(*y); x++, y++) {
    }
    return tolower(*x) != tolower(*y) ? tolower(*x) - tolower(*y) : strcmp(a, b);
}

/* `dir`/`name` into out; 0 if it does not fit (a path cut short would be another file's) */
static int join(char *out, size_t size, const char *dir, const char *name) {
    int n = snprintf(out, size, "%s/%s", dir, name);
    if (n < 0 || (size_t)n >= size) {
        out[0] = '\0';
        return 0;
    }
    return 1;
}

/* A text of a player's file into a field: cut to the field (never in the middle of a UTF-8 character), control
   characters made spaces. */
static void put(char *dst, size_t size, const char *src) {
    size_t n = 0;
    while (src[n] != '\0' && n + 1 < size) {
        dst[n] = (unsigned char)src[n] < 0x20 || src[n] == 0x7F ? ' ' : src[n];
        n++;
    }
    while (n > 0 && src[n] != '\0' && ((unsigned char)src[n] & 0xC0) == 0x80) {
        n--; /* (the cut fell inside a character) */
    }
    dst[n] = '\0';
}

/* What is wrong with a package: said in the log, and kept for the Mods tab. */
static void note(const Chara *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void note(const Chara *c, const char *fmt, ...) {
    char msg[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    fprintf(stderr, "bt3: characters: %s%s%s: %s\n", c->owner, c->owner[0] != '\0' ? "/" : "", c->name, msg);
    if (c->pkg >= 0) {
        PortCharaPkg *p = &sPkg[c->pkg];
        size_t at = strlen(p->problem);
        if (at + 3 < sizeof(p->problem)) {
            snprintf(p->problem + at, sizeof(p->problem) - at, "%s%s", at > 0 ? "; " : "", msg);
        }
    }
}

/* a file to read, and not a folder, a pipe or a device under a file's name */
static int regular(const char *path) {
    struct stat st;
    return path[0] != '\0' && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* a whole number in a manifest's value, with nothing else after it; 0 if it is none or outside lo..hi */
static int number(const char *v, int lo, int hi, int *out) {
    char *end;
    long x;
    if (*v == '\0' || strlen(v) > 12) {
        return 0;
    }
    x = strtol(v, &end, v[0] == '0' && (v[1] == 'x' || v[1] == 'X') ? 16 : 10);
    if (end == v || *end != '\0' || x < lo || x > hi) {
        return 0;
    }
    *out = (int)x;
    return 1;
}

/* One `key=value` of a package (character.txt, and what the older files say, each as its key). */
static void apply(Chara *c, const char *key, const char *value) {
    static const char *const kSlot[7] = {"left", "up", "right", "down", "fusion1", "fusion2", "fusion3"};
    static const char *const kStat[PORT_STAT_COUNT] = {"health", "ki", "blast"};
    static const int kStatLo[PORT_STAT_COUNT] = {10000, 0, 0}, kStatHi[PORT_STAT_COUNT] = {150000, 100000, 7};
    PortCharaPkg *p = c->pkg >= 0 ? &sPkg[c->pkg] : NULL;
    int k, x;

    if (strcmp(key, "name") == 0) {
        if (value[0] != '\0') {
            put(c->shown, sizeof(c->shown), value);
            c->nameSet = 1;
        }
    } else if (strcmp(key, "form") == 0) {
        put(c->label, sizeof(c->label), value);
        c->labelSet = 1;
    } else if (strcmp(key, "base") == 0) {
        if (number(value, 0, DISC_CHARAS - 1, &x)) {
            c->base = x;
        } else {
            note(c, "base has no character id from 0 to 160; 0 is used");
            c->base = 0;
        }
    } else if (strcmp(key, "replaces") == 0) {
        if (number(value, 0, DISC_CHARAS - 1, &x)) {
            c->replaces = x;
        } else {
            note(c, "replaces has no character id from 0 to 160");
            c->replaces = -2; /* (meant as a replacement: not taken as an added character instead) */
        }
    } else if (strcmp(key, "format") == 0) {
        if (number(value, 0, 1000000, &x) && x > PORT_CHARA_FORMAT) {
            note(c, "made for package format %d; this game knows format %d and reads what it knows", x, PORT_CHARA_FORMAT);
        }
    } else if (strcmp(key, "author") == 0) {
        if (p != NULL) {
            put(p->author, sizeof(p->author), value);
        }
    } else if (strcmp(key, "version") == 0) {
        if (p != NULL) {
            put(p->version, sizeof(p->version), value);
        }
    } else if (strcmp(key, "about") == 0) {
        if (p != NULL) {
            put(p->about, sizeof(p->about), value);
        }
    } else if (strncmp(key, "partner", 7) == 0 && key[7] >= '1' && key[7] <= '3' && key[8] == '\0') {
        /* partnerN=Folder,Folder,...: who must be on the team for fusion N (up to four; the character's own
           table has ids, which for an added partner are not known until the folders are counted) */
        int n = key[7] - '1', m;
        const char *at = value;
        for (m = 0; m < 4; m++) {
            char one[64];
            size_t len = strcspn(at, ","), b = 0, e = len;
            while (b < e && isspace((unsigned char)at[b])) {
                b++;
            }
            while (e > b && isspace((unsigned char)at[e - 1])) {
                e--;
            }
            snprintf(one, sizeof(one), "%.*s", (int)(e - b < sizeof(one) - 1 ? e - b : sizeof(one) - 1), at + b);
            put(c->form[7 + n * 4 + m], sizeof(c->form[0]), one);
            at += len + (at[len] == ',');
        }
    } else {
        for (k = 0; k < 7; k++) {
            if (strcmp(key, kSlot[k]) == 0) {
                put(c->form[k], sizeof(c->form[k]), value);
                return;
            }
        }
        for (k = 0; k < PORT_STAT_COUNT; k++) {
            if (strcmp(key, kStat[k]) == 0) {
                if (value[0] == '\0') {
                    c->stat[k] = -1;
                } else if (number(value, kStatLo[k], kStatHi[k], &x)) {
                    c->stat[k] = x;
                } else {
                    note(c, "%s is not a number from %d to %d; the character's own is used", key, kStatLo[k], kStatHi[k]);
                }
                return;
            }
        }
        /* (any other key: a later format's, or a note of the maker's; ignored) */
    }
}

/* The lines of a text of key=value lines (`text` is changed). `#` starts a comment line; spaces around the key and
   the value do not count; the keys without regard to case; a line without `=` says nothing. */
static void parse(Chara *c, char *text, size_t len) {
    size_t at = 0;
    if (len >= 3 && memcmp(text, "\xEF\xBB\xBF", 3) == 0) {
        at = 3; /* (the mark some editors put at the start of a UTF-8 file) */
    }
    while (at < len) {
        char *line = text + at, *eq, *key, *value, *e;
        size_t n = 0;
        while (at + n < len && line[n] != '\n') {
            n++;
        }
        at += n + (at + n < len);
        line[n] = '\0'; /* (the newline's place, or the terminator's behind the text) */
        while (*line != '\0' && isspace((unsigned char)*line)) {
            line++;
        }
        if (*line == '#' || (eq = strchr(line, '=')) == NULL) {
            continue;
        }
        *eq = '\0';
        key = line;
        for (e = eq; e > key && isspace((unsigned char)e[-1]); e--) {
        }
        *e = '\0';
        for (e = key; *e != '\0'; e++) {
            *e = (char)tolower((unsigned char)*e);
        }
        value = eq + 1;
        while (*value != '\0' && isspace((unsigned char)*value)) {
            value++;
        }
        for (e = value + strlen(value); e > value && isspace((unsigned char)e[-1]); e--) {
        }
        *e = '\0';
        apply(c, key, value);
    }
}

/* Reads a text file of a package, at most TEXT_MAX - 1 bytes of it (what lies behind is not looked at); the
   length, or -1 if there is no such file. `text` holds TEXT_MAX bytes and is terminated. */
static long read_text(const char *path, char *text) {
    FILE *f = regular(path) ? fopen(path, "rb") : NULL;
    size_t n;
    if (f == NULL) {
        return -1;
    }
    n = fread(text, 1, TEXT_MAX - 1, f);
    fclose(f);
    text[n] = '\0';
    return (long)n;
}

/* For the test of the reader (port/tools/test_chara_manifest.c): a manifest's text as the game reads it, into a
   package of its own; the result in a line of text. Not used by the game. */
int PortCharas_ParseTest(const char *bytes, unsigned long len, char *out, unsigned size) {
    static char text[TEXT_MAX];
    static Chara c;
    int k, n;
    memset(&c, 0, sizeof(c));
    c.replaces = -1;
    c.pkg = sPkgCount < PKG_MAX ? sPkgCount : PKG_MAX - 1;
    memset(&sPkg[c.pkg], 0, sizeof(sPkg[0]));
    snprintf(c.name, sizeof(c.name), "test");
    snprintf(c.shown, sizeof(c.shown), "test");
    for (k = 0; k < PORT_STAT_COUNT; k++) {
        c.stat[k] = -1;
    }
    if (len > TEXT_MAX - 1) {
        len = TEXT_MAX - 1;
    }
    memcpy(text, bytes, len);
    text[len] = '\0';
    parse(&c, text, len);
    n = snprintf(out, size, "name=[%s] form=[%s]%d base=%d replaces=%d health=%d ki=%d blast=%d author=[%s] version=[%s] about=[%s] left=[%s] partner1=[%s|%s|%s|%s] problem=[%s]",
                 c.shown, c.label, c.labelSet, c.base, c.replaces, c.stat[0], c.stat[1], c.stat[2], sPkg[c.pkg].author, sPkg[c.pkg].version,
                 sPkg[c.pkg].about, c.form[0], c.form[7], c.form[8], c.form[9], c.form[10], sPkg[c.pkg].problem);
    return n;
}

/* The buffers the game reads a character's files into, in bytes (a file is read in whole 2048-byte sectors, and
   nothing in the game looks at a file's size before it reads): a file that does not fit is not taken.
   Models: BtlRes_ModelSize (btl_obj.c; the original's 0xCE000 grows to the largest there is, up to PORT_MODEL_MAX).
   Animations and effects: BTL_RES_ANIM0_SIZE, BTL_RES_ANIM1_SIZE (the two fixed slots of BtlRes_Init). Voice
   bank: BTL_BANK_BUF_SIZE (the reload buffer of battle_load.c). Portrait: 0x16800 in every screen. Member block:
   BTL_MEMBER_BUF_SIZE. The spoken lines are streamed and have no buffer. */
static long limit_of(int kind) {
    return kind < 8 ? PORT_MODEL_MAX : kind == F_ANM ? 0x160800 : kind == F_EFF ? 0xCE800 : 0x90800;
}
enum { PORTRAIT_MAX = 0x16800, MEMBER_MAX = 0x1000 };

/* Whether the file at `path` can be given to the game for a buffer of `limit` bytes; if not, `path` is emptied
   and the reason noted. */
static int fits(const Chara *c, char *path, long limit, const char *what) {
    struct stat st;
    long padded;
    if (path[0] == '\0') {
        return 1;
    }
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        note(c, "%s cannot be read; that file is left out", strrchr(path, '/') + 1);
        path[0] = '\0';
        return 0;
    }
    padded = (long)((st.st_size + 2047) / 2048 * 2048);
    if (st.st_size < 16 || st.st_size > 0x7FFFFFF || padded > limit) {
        if (st.st_size < 16) {
            note(c, "%s is empty; that file is left out", strrchr(path, '/') + 1);
        } else {
            note(c, "%s is %ld bytes, more than the %ld the game has room for (%s); that file is left out", strrchr(path, '/') + 1, (long)st.st_size, limit, what);
        }
        path[0] = '\0';
        return 0;
    }
    return 1;
}

/* Whether a chip file is what the game takes: a texture file whose offsets stay inside its 5,504 bytes (the game
   turns them into pointers and follows them). */
static int chip_ok(const unsigned char *d, size_t got) {
    unsigned count, ent, i;
    if (got < 64) {
        return 0;
    }
    memcpy(&count, d, 4);
    memcpy(&ent, d + 4, 4);
    if (count == 0 || count > 8 || ent > CHIP_SIZE / 4 || (size_t)ent * 4 + (size_t)count * 0x40 > CHIP_SIZE || ent * 4 < 0x14) {
        return 0;
    }
    for (i = 0; i < count; i++) {
        unsigned pix, clut;
        int pixSize, clutSize;
        memcpy(&pix, d + ent * 4 + i * 0x40, 4);
        memcpy(&clut, d + ent * 4 + i * 0x40 + 4, 4);
        memcpy(&pixSize, d + ent * 4 + i * 0x40 + 8, 4);
        memcpy(&clutSize, d + ent * 4 + i * 0x40 + 12, 4);
        if (pix > CHIP_SIZE / 4 || clut > CHIP_SIZE / 4 || pixSize < 0 || clutSize < 0 || (size_t)pix * 4 + (size_t)pixSize > CHIP_SIZE ||
            (size_t)clut * 4 + (size_t)clutSize > CHIP_SIZE) {
            return 0;
        }
    }
    return 1;
}

static int chip_read(const char *path, unsigned char *out) {
    FILE *f = regular(path) ? fopen(path, "rb") : NULL;
    size_t got;
    if (f == NULL) {
        return 0;
    }
    memset(out, 0, CHIP_SIZE);
    got = fread(out, 1, CHIP_SIZE, f);
    fclose(f);
    return chip_ok(out, got);
}

/* Looks at the folder `name` of `root`. If it holds a package it is kept: among the added characters, or among the
   replacements when its manifest says `replaces`. `owner`: NULL, or the package whose folder `root` is (what the
   folder's own files do not say about base and name is the owner's). Returns the kept package, NULL if none. */
static Chara *scan(const char *root, const char *name, const Chara *owner) {
    static Chara t;
    static char text[TEXT_MAX];
    static unsigned char chip[CHIP_SIZE];
    char path[400], side[5][400]; /* name.txt, form.txt, base.txt, forms.txt, character.txt */
    static const char *const kSide[5] = {"name.txt", "form.txt", "base.txt", "forms.txt", "character.txt"};
    Chara *c = &t, *kept;
    PortCharaPkg *p = NULL;
    DIR *d;
    struct dirent *de;
    long len;
    int k, files = 0;

    if (!join(path, sizeof(path), root, name) || (d = opendir(path)) == NULL) {
        return NULL;
    }
    memset(c, 0, sizeof(*c));
    memset(side, 0, sizeof(side));
    c->replaces = -1;
    c->ownerId = -1;
    c->pkg = -1;
    for (k = 0; k < PORT_STAT_COUNT; k++) {
        c->stat[k] = -1;
    }
    snprintf(c->name, sizeof(c->name), "%s", name);
    snprintf(c->dir, sizeof(c->dir), "%s", path);
    if (owner != NULL) {
        snprintf(c->owner, sizeof(c->owner), "%s", owner->name);
        c->ownerId = id_of(owner);
        c->base = owner->base;
    }
    snprintf(c->shown, sizeof(c->shown), "%s", owner != NULL ? owner->shown : name);
    while ((de = readdir(d)) != NULL) {
        /* (of two files of one kind the first by name is taken: the same on every machine) */
        char *slot = NULL;
        const char *n = de->d_name;
        k = kind_of(n);
        if (k >= 0) {
            slot = c->file[k];
        } else if (same_nocase(n, "chip.bin")) {
            slot = c->chip;
        } else if (same_nocase(n, "portrait.bin")) {
            slot = c->portrait;
        } else if (same_nocase(n, "member_1p.bin")) {
            slot = c->member[0];
        } else if (same_nocase(n, "member_2p.bin")) {
            slot = c->member[1];
        } else if (same_nocase(n, "voice_jp")) {
            slot = c->lines[0];
        } else if (same_nocase(n, "voice_us")) {
            slot = c->lines[1];
        } else {
            for (k = 0; k < 5; k++) {
                if (same_nocase(n, kSide[k])) {
                    slot = side[k];
                }
            }
        }
        if (slot != NULL && (slot[0] == '\0' || name_cmp(n, strrchr(slot, '/') + 1) < 0)) {
            join(slot, 400, path, n);
        }
    }
    closedir(d);
    for (k = 0; k < F_COUNT; k++) {
        files += c->file[k][0] != '\0';
    }
    files += (c->chip[0] != '\0') + (c->portrait[0] != '\0') + (c->member[0][0] != '\0') + (c->member[1][0] != '\0') + (c->lines[0][0] != '\0') +
             (c->lines[1][0] != '\0');
    if (files == 0 && side[4][0] == '\0') {
        return NULL; /* nothing of a package in it: some other folder */
    }
    if (sPkgCount < PKG_MAX) {
        c->pkg = sPkgCount++;
        p = &sPkg[c->pkg];
        memset(p, 0, sizeof(*p));
        snprintf(p->folder, sizeof(p->folder), "%s%s%s", c->owner, c->owner[0] != '\0' ? "/" : "", c->name);
        p->kind = PORT_PKG_OFF;
        p->id = -1;
    }
    /* what the older files say, then the manifest, which has the last word */
    if (read_text(side[0], text) >= 0) {
        text[strcspn(text, "\r\n")] = '\0';
        apply(c, "name", text);
    }
    if (read_text(side[1], text) >= 0) {
        text[strcspn(text, "\r\n")] = '\0';
        apply(c, "form", text);
    }
    if (read_text(side[2], text) >= 0) {
        char *v = text;
        while (isspace((unsigned char)*v)) {
            v++;
        }
        v[strcspn(v, " \t\r\n")] = '\0';
        apply(c, "base", v);
    }
    if ((len = read_text(side[3], text)) >= 0) {
        parse(c, text, (size_t)len);
    }
    if ((len = read_text(side[4], text)) >= 0) {
        parse(c, text, (size_t)len);
    }
    if (p != NULL) {
        put(p->name, sizeof(p->name), c->shown);
    }

    /* files the game has no room for are not taken */
    for (k = 0; k < F_COUNT; k++) {
        if (c->replaces == -1 && k < 4 && c->file[k][0] != '\0') {
            struct stat st; /* (an added character's costume: without it the character is left out, as before) */
            if (stat(c->file[k], &st) == 0 && st.st_size > PORT_MODEL_MAX) {
                note(c, "%s is %ld bytes, more than the 1.5 MB a model may have; the character is left out", strrchr(c->file[k], '/') + 1,
                     (long)st.st_size);
                return NULL;
            }
        }
        fits(c, c->file[k], limit_of(k), k < 8 ? "a model" : k == F_ANM ? "the animations" : k == F_EFF ? "the effects" : "a voice bank");
    }
    fits(c, c->portrait, PORTRAIT_MAX, "the large picture");
    fits(c, c->member[0], MEMBER_MAX, "the member block");
    fits(c, c->member[1], MEMBER_MAX, "the member block");
    if (c->chip[0] != '\0' && !chip_read(c->chip, chip)) {
        note(c, "chip.bin is not a chip of the game (one entry of the select's chip pack, 5504 bytes); the chip is left out");
        c->chip[0] = '\0';
    }

    if (c->replaces == -2) {
        return NULL; /* (said above) */
    }
    if (c->replaces >= 0) {
        int has = c->nameSet || c->labelSet || c->chip[0] != '\0' || c->portrait[0] != '\0' || c->member[0][0] != '\0' || c->member[1][0] != '\0' ||
                  c->lines[0][0] != '\0' || c->lines[1][0] != '\0';
        for (k = 0; k < F_COUNT; k++) {
            has |= c->file[k][0] != '\0';
        }
        for (k = 0; k < PORT_STAT_COUNT; k++) {
            has |= c->stat[k] >= 0;
        }
        for (k = 0; k < SLOTS; k++) {
            has |= c->form[k][0] != '\0';
        }
        if (!has) {
            note(c, "replaces character %d but holds nothing to put in its place", c->replaces);
            return NULL;
        }
        if (sReplOf[c->replaces] != 0) {
            const Chara *first = &sRepl[sReplOf[c->replaces] - 1];
            note(c, "character %d is already replaced by %s%s%s (the first by name counts)", c->replaces, first->owner,
                 first->owner[0] != '\0' ? "/" : "", first->name);
            return NULL;
        }
        c->base = c->replaces;
        c->costumes = 0; /* (counted from the disc's own number when asked: Port_CharaCostumes) */
        kept = &sRepl[sReplCount];
        *kept = *c;
        sReplOf[c->replaces] = (short)(++sReplCount);
        if (p != NULL) {
            p->kind = PORT_PKG_REPLACES;
            p->id = c->replaces;
        }
        return kept;
    }
    if (c->file[0][0] == '\0') {
        /* no first costume: not an added character's folder. (A folder with a manifest and no `replaces` is said;
           one without a manifest that holds only other files is passed over, as before.) */
        if (side[4][0] != '\0') {
            note(c, "no model of the first costume (*_1p.pak) and no `replaces`: not a character");
        } else if (c->pkg >= 0 && c->pkg == sPkgCount - 1 && p->problem[0] == '\0') {
            sPkgCount--;
        }
        return NULL;
    }
    if (sCount >= PORT_CHARA_MAX) {
        note(c, "there are already %d added characters, the most the game takes", PORT_CHARA_MAX);
        return NULL;
    }
    for (c->costumes = 1; c->costumes < 4 && c->file[c->costumes][0] != '\0'; c->costumes++) {
    }
    kept = &sChara[sCount++];
    *kept = *c;
    if (p != NULL) {
        p->kind = PORT_PKG_ADDS;
        p->id = id_of(kept);
    }
    return kept;
}

/* the folders inside `dir`, by name (the same order on every machine); `skip`: the two voice folders */
static int folders(const char *dir, char names[][128], int max, int skip) {
    DIR *d = opendir(dir);
    struct dirent *de;
    int n = 0;
    if (d == NULL) {
        return 0;
    }
    while ((de = readdir(d)) != NULL && n < max) {
        if (de->d_name[0] != '.' && strlen(de->d_name) < 64 && !(skip && (same_nocase(de->d_name, "voice_us") || same_nocase(de->d_name, "voice_jp")))) {
            snprintf(names[n++], 128, "%s", de->d_name);
        }
    }
    closedir(d);
    qsort(names, (size_t)n, 128, name_cmp);
    return n;
}

static const Chara *entry(int j) { /* every kept package: the added ones, then the replacements */
    return j < sCount ? &sChara[j] : &sRepl[j - sCount];
}

/* the id of the folder a link names, as package c's folder sees it: ".." the character whose folder it lies in, a
   folder beside it inside the same character's folder (or, from the character itself, one inside its own), a
   folder of the top, or "Character/Form"; 0xFF if there is none. A folder that replaces a disc character is that
   disc character. */
static int find(const Chara *c, const char *want) {
    const char *family = c->owner[0] != '\0' ? c->owner : c->name;
    const char *slash = strchr(want, '/');
    int j, all = sCount + sReplCount;
    if (strcmp(want, "..") == 0) {
        return c->ownerId >= 0 ? c->ownerId : 0xFF;
    }
    for (j = 0; j < all; j++) {
        if (strcmp(entry(j)->owner, family) == 0 && strcmp(entry(j)->name, want) == 0) {
            return id_of(entry(j));
        }
    }
    for (j = 0; j < all; j++) {
        if (entry(j)->owner[0] == '\0' && strcmp(entry(j)->name, want) == 0) {
            return id_of(entry(j));
        }
    }
    for (j = 0; slash != NULL && j < all; j++) {
        const char *o = entry(j)->owner;
        if (o[0] != '\0' && strlen(o) == (size_t)(slash - want) && strncmp(o, want, (size_t)(slash - want)) == 0 && strcmp(entry(j)->name, slash + 1) == 0) {
            return id_of(entry(j));
        }
    }
    return 0xFF;
}

/* the links: folders to ids, once every folder has its id */
static void resolve(Chara *c) {
    int k;
    for (k = 0; k < SLOTS; k++) {
        const char *want = c->form[k];
        c->target[k] = -1;
        if (want[0] == '\0') {
            continue;
        }
        c->target[k] = 0xFF;
        if (strncmp(want, "disc:", 5) == 0) {
            int id;
            c->target[k] = number(want + 5, 0, DISC_CHARAS - 1, &id) ? id : 0xFF;
            continue;
        }
        c->target[k] = find(c, want);
        if (c->target[k] == 0xFF && k < 7) {
            int seen = 0, j;
            for (j = 0; j < k; j++) { /* (said once for a folder, however many slots name it) */
                seen |= strcmp(c->form[j], want) == 0;
            }
            if (!seen) {
                note(c, "%s, named as a link, is not installed; that %s is left out", want, k < 4 ? "transformation" : "fusion");
            }
        }
    }
}

void PortCharas_Init(void) {
    static char names[256][128], inner[64][128];
    char root[300];
    int n = 0, i;

    if (sInit) {
        return;
    }
    sInit = 1;
    if (getenv("BT3_NET_SESSION") != NULL && (getenv("BT3_NET_HOST") != NULL || getenv("BT3_NET_JOIN") != NULL)) {
        return; /* the program was started for an online session: the folder is not read */
    }
    if (!PortExtras_Dir("characters", "BT3_CHARACTERS", root, sizeof(root))) {
        return;
    }
    n = folders(root, names, 256, 0);
    for (i = 0; i < n; i++) {
        int m, k;
        const Chara *owner = scan(root, names[i], NULL);
        if (owner == NULL) {
            continue;
        }
        /* the folders inside a package's folder are packages too: the character's forms (added characters of their
           own, right after it), or with `replaces` the replacements of the disc character's forms */
        m = folders(owner->dir, inner, 64, 1);
        for (k = 0; k < m; k++) {
            scan(owner->dir, inner[k], owner);
        }
    }
    for (i = 0; i < sCount; i++) {
        resolve(&sChara[i]);
    }
    for (i = 0; i < sReplCount; i++) {
        resolve(&sRepl[i]);
    }
    if (sCount > 0) {
        fprintf(stderr, "bt3: characters: %d added from %s\n", sCount, root);
        for (i = 0; i < sCount; i++) {
            fprintf(stderr, "bt3: characters:   %s%s%s: id %d, %d costume%s, base character %d\n", sChara[i].owner, sChara[i].owner[0] != '\0' ? "/" : "",
                    sChara[i].name, PORT_CHARA_FIRST + i, sChara[i].costumes, sChara[i].costumes == 1 ? "" : "s", sChara[i].base);
        }
    }
    if (sReplCount > 0) {
        fprintf(stderr, "bt3: characters: %d of the disc's replaced from %s\n", sReplCount, root);
        for (i = 0; i < DISC_CHARAS; i++) {
            const Chara *c = sReplOf[i] != 0 ? &sRepl[sReplOf[i] - 1] : NULL;
            int k, files = 0;
            for (k = 0; c != NULL && k < F_COUNT; k++) {
                files += c->file[k][0] != '\0';
            }
            if (c != NULL) {
                fprintf(stderr, "bt3: characters:   %s%s%s: replaces character %d (%d of its twelve files%s%s%s%s%s)\n", c->owner, c->owner[0] != '\0' ? "/" : "",
                        c->name, i, files, c->portrait[0] != '\0' ? ", portrait" : "", c->chip[0] != '\0' ? ", chip" : "", c->nameSet ? ", name" : "",
                        c->lines[0][0] != '\0' || c->lines[1][0] != '\0' ? ", spoken lines" : "",
                        c->stat[0] >= 0 || c->stat[1] >= 0 || c->stat[2] >= 0 ? ", numbers" : "");
            }
        }
    }
}

/* The size of the fight's model buffers: the original's (0xCE000), or the largest model file there is to load if that
   is larger: a package's, or one of the data folder's own (0x590 + id * 10 + 0..7, the first archive; the file under
   mods/ where there is one, which is the one loaded), which a disc with replaced characters can have. Whole pages;
   not over PORT_MODEL_MAX (a larger file of the data folder is reported once: it would be read past the buffer's
   end). */
int Port_ModelSize(void) {
    static int size;
    if (size == 0) {
        const char *root = getenv("BT3_DATA") != NULL ? getenv("BT3_DATA") : "gamedata";
        long most = 0xCE000;
        int i, k, over = 0;
        char path[600];
        struct stat st;
        PortCharas_Init();
        for (i = 0; i < sCount + sReplCount; i++) {
            for (k = 0; k < 8; k++) {
                if (entry(i)->file[k][0] != '\0' && stat(entry(i)->file[k], &st) == 0 && st.st_size > most) {
                    most = (long)st.st_size;
                }
            }
        }
        for (i = 0; i < DISC_CHARAS; i++) {
            for (k = 0; k < 8; k++) {
                snprintf(path, sizeof(path), "%s/mods/pzs3us1/%05d.bin", root, 0x590 + i * 10 + k - 1);
                if (stat(path, &st) != 0) {
                    snprintf(path, sizeof(path), "%s/pzs3us1/%05d.bin", root, 0x590 + i * 10 + k - 1);
                    if (stat(path, &st) != 0) {
                        continue;
                    }
                }
                if (st.st_size > most) {
                    if (st.st_size > PORT_MODEL_MAX) {
                        over++;
                    } else {
                        most = (long)st.st_size;
                    }
                }
            }
        }
        size = (int)((most + 0xFFF) & ~0xFFFL);
        if (over > 0) {
            fprintf(stderr, "bt3: %d model files of the game data are larger than the 1.5 MB a model may have\n", over);
        }
        if (size > 0xCE000) {
            fprintf(stderr, "bt3: the fight's model buffers are %d KB (the largest model there is to load)\n", size / 1024);
        }
    }
    return size;
}

int Port_CharaCount(void) {
    PortCharas_Init();
    return live();
}

/* How many the folder holds, in a session too (for the settings window, which says they are not used there). */
int Port_CharaInstalled(void) {
    PortCharas_Init();
    return sCount;
}

int Port_CharaPkgCount(void) {
    PortCharas_Init();
    return sPkgCount;
}

const PortCharaPkg *Port_CharaPkg(int index) {
    PortCharas_Init();
    return index >= 0 && index < sPkgCount ? &sPkg[index] : NULL;
}

int Port_CharaBase(int chara) {
    PortCharas_Init();
    if (chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + live()) {
        return sChara[chara - PORT_CHARA_FIRST].base;
    }
    return chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + PORT_CHARA_MAX ? 0 : chara; /* (an id nothing is behind) */
}

/* An added character: the costumes its folder has; any other id: 0. */
int Port_CharaCostumes(int chara) {
    PortCharas_Init();
    return chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + live() ? sChara[chara - PORT_CHARA_FIRST].costumes : 0;
}

/* A number of a package's manifest (health, ki, blast) for character `chara`, an added one or a replaced one of
   the disc; `value`, the table's own, where the package says nothing, there is none, or in a session. */
int Port_CharaStat(int chara, int which, int value) {
    const Chara *c;
    PortCharas_Init();
    c = chara_of(chara);
    return c != NULL && which >= 0 && which < PORT_STAT_COUNT && c->stat[which] >= 0 ? c->stat[which] : value;
}

const char *Port_CharaName(int index) {
    PortCharas_Init();
    return index >= 0 && index < live() ? sChara[index].name : "";
}

static int alias(const char *rel, char *out, unsigned n);

/* (BT3_CHARA_LOG: every file answered from or for a package, once each: the folder's own or the disc's it falls
   back on) */
int PortCharas_Alias(const char *rel, char *out, unsigned n) {
    static char said[256][24];
    static int saidCount, log = -1;
    int r = alias(rel, out, n), i;
    if (log < 0) {
        log = getenv("BT3_CHARA_LOG") != NULL;
    }
    if (r && log) {
        for (i = 0; i < saidCount && strcmp(said[i], rel) != 0; i++) {
        }
        if (i == saidCount && saidCount < 256 && strlen(rel) < sizeof(said[0])) {
            strcpy(said[saidCount++], rel);
            fprintf(stderr, "bt3: characters: file %s is %s\n", rel, out);
        }
    }
    return r;
}

static int have(const char *path) {
    FILE *f = regular(path) ? fopen(path, "rb") : NULL;
    if (f != NULL) {
        fclose(f);
    }
    return f != NULL;
}

/* A file number of the disc that is a replaced character's own: the package's file, if it has that one. */
static int alias_replaced(int arc, int index, char *out, unsigned n) {
    const Chara *c;
    const char *path = NULL;
    if (arc == 1) {
        int id = index + 1; /* (the first archive's file id N is its file N - 1) */
        if (id >= 8 && id < 8 + DISC_CHARAS * 2) {
            c = repl_of((id - 8) / 2);
            path = c != NULL ? c->member[(id - 8) % 2] : NULL;
        } else if (id >= 0x2F9 && id < 0x2F9 + DISC_CHARAS) {
            c = repl_of(id - 0x2F9);
            path = c != NULL ? c->portrait : NULL;
        } else if (id >= 0x590 && id < 0x590 + DISC_CHARAS * 10) {
            int k = (id - 0x590) % 10;
            c = repl_of((id - 0x590) / 10);
            if (c != NULL) {
                if (c->file[k][0] == '\0' && k >= 4 && k < 8 && c->file[k - 4][0] != '\0') {
                    k -= 4; /* its costume without a damaged model of its own: the costume as it is */
                }
                if (c->file[k][0] == '\0' && k < 8 && c->file[0][0] != '\0' && (c->file[F_ANM][0] != '\0' || c->file[F_EFF][0] != '\0')) {
                    /* A folder with animations or effects of its own is a whole character, and a model must fit
                       those (its table names them by number: the disc character's other costume on this folder's
                       effects is a crash of the game's). A costume it does not have is its first. A folder
                       without them, a skin, leaves the disc's other costumes alone. */
                    k = 0;
                }
                path = c->file[k];
            }
        } else if (id >= 0xBDA && id < 0xBDA + DISC_CHARAS) {
            c = repl_of(id - 0xBDA);
            path = c != NULL ? c->file[F_VOICE] : NULL;
        } else if (id >= 0xC7B && id < 0xC7B + DISC_CHARAS) {
            c = repl_of(id - 0xC7B);
            path = c != NULL ? c->file[F_VOICE + 1] : NULL;
        }
    } else {
        int id = index + 0xD48, set = id >= 0xCC32, at = id - (set ? 0xCC32 : 0x8D4E);
        if (id >= 0x8D4E && at >= 0 && at < DISC_CHARAS * 100 && (set || id < 0x8D4E + DISC_CHARAS * 100)) {
            c = repl_of(at / 100);
            if (c != NULL && c->lines[set][0] != '\0') {
                int w = snprintf(out, n, "%s/%03d.adx", c->lines[set], at % 100);
                return w > 0 && (unsigned)w < n && have(out);
            }
        }
        return 0;
    }
    if (path == NULL || path[0] == '\0') {
        return 0;
    }
    snprintf(out, n, "%s", path);
    return 1;
}

static int alias(const char *rel, char *out, unsigned n) {
    int index = -1, who, k, arc;
    const Chara *c;

    if (strncmp(rel, "pzs3us1/", 8) != 0 && strncmp(rel, "pzs3us2/", 8) != 0) {
        return 0;
    }
    arc = rel[6] - '0';
    if (!isdigit((unsigned char)rel[8]) || sscanf(rel + 8, "%d", &index) != 1) {
        return 0;
    }
    if (arc == 1 || index < PORT_CHARA_FILE_INDEX) {
        PortCharas_Init();
        return rlive() > 0 ? alias_replaced(arc, index, out, n) : 0;
    }
    PortCharas_Init();
    if (index >= PORT_CHARA_LINE_INDEX && index < PORT_CHARA_LINE_INDEX + PORT_CHARA_MAX * 200) {
        /* a spoken line: the folder's voice_jp/NNN.adx or voice_us/NNN.adx (the other language's where only one is
           there), or the base character's line of the disc (0x8D4E + id * 100 + line, and 0xCC32 + ...) */
        int alt = (index - PORT_CHARA_LINE_INDEX) % 200 / 100, line = (index - PORT_CHARA_LINE_INDEX) % 100, t;
        who = (index - PORT_CHARA_LINE_INDEX) / 200;
        for (t = 0; who < live() && t < 2; t++) {
            const char *dir = sChara[who].lines[alt ^ t];
            int w = dir[0] != '\0' ? snprintf(out, n, "%s/%03d.adx", dir, line) : 0;
            if (w > 0 && (unsigned)w < n && have(out)) {
                return 1;
            }
        }
        snprintf(out, n, "pzs3us2/%05d.bin", (alt ? 0xCC32 : 0x8D4E) - 0xD48 + (who < live() ? sChara[who].base : 0) * 100 + line);
        return 1;
    }
    if (index >= PORT_CHARA_MEMBER_INDEX && index < PORT_CHARA_MEMBER_INDEX + PORT_CHARA_MAX * 2) {
        /* the fight's member block (the team mate's small picture): the folder's member_1p.bin / member_2p.bin, or
           the base character's (the disc's 8 + id * 2 + side) */
        int side = (index - PORT_CHARA_MEMBER_INDEX) % 2;
        who = (index - PORT_CHARA_MEMBER_INDEX) / 2;
        if (who < live() && sChara[who].member[side][0] != '\0') {
            snprintf(out, n, "%s", sChara[who].member[side]);
        } else {
            snprintf(out, n, "pzs3us1/%05d.bin", 7 + (who < live() ? sChara[who].base : 0) * 2 + side);
        }
        return 1;
    }
    if (index >= PORT_CHARA_FACE_INDEX && index < PORT_CHARA_FACE_INDEX + PORT_CHARA_MAX) {
        /* the large picture: the folder's, or the base character's (the disc's 0x2F9 + id) */
        who = index - PORT_CHARA_FACE_INDEX;
        if (who < live() && sChara[who].portrait[0] != '\0') {
            snprintf(out, n, "%s", sChara[who].portrait);
        } else {
            snprintf(out, n, "pzs3us1/%05d.bin", 0x2F8 + (who < live() ? sChara[who].base : 0));
        }
        return 1;
    }
    if (index < PORT_CHARA_FILE_INDEX + PORT_CHARA_MAX * 10) {
        who = (index - PORT_CHARA_FILE_INDEX) / 10;
        k = (index - PORT_CHARA_FILE_INDEX) % 10;
    } else if (index >= PORT_CHARA_VOICE_INDEX && index < PORT_CHARA_VOICE_INDEX + PORT_CHARA_MAX * 2) {
        who = (index - PORT_CHARA_VOICE_INDEX) / 2;
        k = F_VOICE + (index - PORT_CHARA_VOICE_INDEX) % 2;
    } else {
        return 0;
    }
    c = &sChara[who < live() ? who : 0];
    if (who >= live()) { /* an id nothing is behind (a save or a replay of another set-up): the first character of the disc */
        snprintf(out, n, "pzs3us1/%05d.bin", k >= F_VOICE ? (k == F_VOICE ? 3033 : 3194) : 1423 + k);
        return 1;
    }
    if (c->file[k][0] == '\0' && k >= 4 && k < 8 && c->file[k - 4][0] != '\0') {
        k -= 4; /* no damaged model: the costume as it is */
    }
    if (c->file[k][0] == '\0' && k >= F_VOICE && c->file[F_VOICE + (F_VOICE + 1 - k)][0] != '\0') {
        k = F_VOICE + (F_VOICE + 1 - k); /* one language only: that one for both */
    }
    if (c->file[k][0] != '\0') {
        snprintf(out, n, "%s", c->file[k]);
    } else if (k >= F_VOICE) { /* the base character's: the disc's 0xBDA + id and 0xC7B + id */
        snprintf(out, n, "pzs3us1/%05d.bin", (k == F_VOICE ? 3033 : 3194) + c->base);
    } else {                   /* the disc's 0x590 + id * 10 + k */
        snprintf(out, n, "pzs3us1/%05d.bin", 1423 + c->base * 10 + k);
    }
    return 1;
}

/* A link of a package's manifest as a character id, for an added character or a replaced one of the disc: -1
   where nothing is said (the character's own table counts). */
int Port_CharaFormTarget(int chara, int slot) {
    const Chara *c;
    PortCharas_Init();
    c = chara_of(chara);
    return c != NULL && slot >= 0 && slot < SLOTS ? c->target[slot] : -1;
}

/* whether added character j is reached by a transformation (left, up, right) of another added one */
static int reached(int j) {
    int i, k;
    for (i = 0; i < live(); i++) {
        for (k = 0; k < 3; k++) {
            if (i != j && sChara[i].target[k] == PORT_CHARA_FIRST + j) {
                return 1;
            }
        }
    }
    return 0;
}

/* the added characters reached from `head` through transformations (any slot), `head` first */
static int closure(int head, int *out, int max) {
    int n = 0, at, k, i;
    out[n++] = head;
    for (at = 0; at < n; at++) {
        for (k = 0; k < 4; k++) {
            int t = sChara[out[at]].target[k] - PORT_CHARA_FIRST;
            if (t < 0 || t >= live()) {
                continue;
            }
            for (i = 0; i < n && out[i] != t; i++) {
            }
            if (i == n && n < max) {
                out[n++] = t;
            }
        }
    }
    return n;
}

int Port_CharaForms(int chara, int *out, int max) {
    int index = chara - PORT_CHARA_FIRST, group[PORT_CHARA_MAX], n, i, head = -1, lowest = PORT_CHARA_MAX;
    PortCharas_Init();
    if (index < 0 || index >= live() || max < 1) {
        return 0;
    }
    if (reached(index)) {
        /* a form: it has no cell, unless its whole group has no head (forms that only lead to each other), in
           which case the first of them in the folder order is taken as the head */
        n = closure(index, group, PORT_CHARA_MAX);
        for (i = 0; i < n; i++) {
            if (!reached(group[i])) {
                head = group[i];
            }
            if (group[i] < lowest) {
                lowest = group[i];
            }
        }
        if (head >= 0 || lowest != index) {
            return -1;
        }
    }
    n = closure(index, group, max < PORT_CHARA_MAX ? max : PORT_CHARA_MAX);
    for (i = 0; i < n; i++) {
        out[i] = PORT_CHARA_FIRST + group[i];
    }
    return n;
}

void *Port_CharaChipLoad(int index) {
    PortCharas_Init();
    if (index < 0 || index >= live()) {
        return NULL;
    }
    sChipHave[index] = sChara[index].chip[0] != '\0' && chip_read(sChara[index].chip, sChip[index]);
    return sChipHave[index] ? sChip[index] : NULL;
}

void *Port_CharaChip(int chara) {
    int index = chara - PORT_CHARA_FIRST;
    return index >= 0 && index < live() && sChipHave[index] ? sChip[index] : NULL;
}

extern int Port_Setting(const char *name, int def); /* plat_settings.c */
extern void Port_SettingSave(const char *name, int value);
static int sPool = -1;

int Port_CharaPool(void) {
    if (Port_NetSession()) {
        return 0;
    }
    if (sPool < 0) {
        sPool = getenv("BT3_CHARA_POOL") != NULL ? atoi(getenv("BT3_CHARA_POOL")) != 0 : Port_Setting("chara_pool", 0) != 0;
    }
    return sPool;
}

void Port_CharaPoolSet(int on) {
    sPool = on != 0;
    Port_SettingSave("chara_pool", sPool);
}

void Port_CharaCellLog(int cell, int chara, int forms) {
    if (getenv("BT3_CHARA_LOG") != NULL) {
        fprintf(stderr, "bt3: characters: cell %d (row %d, column %d): character %d with %d form%s\n", cell, cell / 7, cell % 7, chara, forms,
                forms > 1 ? "s" : "");
    }
}

extern void *Heap_Alloc(int size, int align, int fromTail, int heap); /* the game's (src/sys/heap.c) */
extern void Heap_Free(void *ptr);
extern void Res_RelocateOffsets(void *out, void *base, void *hdr); /* src/sys/gfxm_b_b.c */
static unsigned char *sChipMem;
static void *sChipRes[PORT_CHARA_MAX];

/* (In a session neither does anything: the chips a screen of the player's own game had loaded when the session was
   asked for lie in that game's memory, which comes back as it was when the session is over.) */
void PortChips_Free(void) {
    if (Port_NetSession()) {
        return;
    }
    if (sChipMem != NULL) {
        Heap_Free(sChipMem);
        sChipMem = NULL;
    }
    memset(sChipRes, 0, sizeof(sChipRes));
}

/* `pack`: the screen's own chip pack (a count, the entries' offsets, the entries), its entries made ready. An
   added character's chip is kept beside it, in memory of the game's taken for it; a replaced character's is put
   over the disc's entry in the pack itself (the same 5,504 bytes), so it takes none. */
void PortChips_Load(void *pack) {
    static unsigned char file[CHIP_SIZE];
    unsigned *words = pack;
    int i;
    if (Port_NetSession()) {
        return;
    }
    PortCharas_Init();
    PortChips_Free();
    for (i = 0; pack != NULL && i < rlive(); i++) {
        const Chara *c = &sRepl[i];
        unsigned id = (unsigned)c->replaces, a, b;
        if (c->chip[0] == '\0' || id + 1 >= words[0]) {
            continue;
        }
        a = words[id + 1] & ~3u;
        b = words[id + 2] & ~3u;
        if (b >= a + CHIP_SIZE && chip_read(c->chip, file)) {
            void *res = (unsigned char *)pack + a;
            memcpy(res, file, CHIP_SIZE);
            Res_RelocateOffsets(&res, res, res);
            if (getenv("BT3_CHARA_LOG") != NULL) {
                fprintf(stderr, "bt3: characters: the chip of character %u is %s\n", id, c->chip);
            }
        }
    }
    if (live() == 0) {
        return;
    }
    sChipMem = Heap_Alloc(live() * CHIP_SIZE, 0x40, 0, 2);
    for (i = 0; sChipMem != NULL && i < live(); i++) {
        void *chip = Port_CharaChipLoad(i);
        if (chip != NULL) {
            void *res = sChipMem + i * CHIP_SIZE;
            memcpy(res, chip, CHIP_SIZE);
            sChipRes[i] = res;
            Res_RelocateOffsets(&res, res, res);
        }
    }
}

void *PortChips_Get(int chara) {
    int index = chara - PORT_CHARA_FIRST;
    return index >= 0 && index < live() ? sChipRes[index] : NULL;
}

/* A text of the folders' as the game's lines are: UTF-16 after the mark FF FE. (UTF-8 in; a character past the
   16-bit range becomes '?'.) */
static void text16(unsigned char *e, const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    unsigned k = 0;
    e[0] = 0xFF;
    e[1] = 0xFE;
    while (*p != '\0' && k < (NAME_ENTRY - 4) / 2) {
        unsigned c = *p++;
        if (c >= 0xF0) {
            c = '?';
            while ((*p & 0xC0) == 0x80) {
                p++;
            }
        } else if (c >= 0xE0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80) {
            c = (c & 0x0F) << 12 | (p[0] & 0x3F) << 6 | (p[1] & 0x3F);
            p += 2;
        } else if (c >= 0xC0 && (p[0] & 0xC0) == 0x80) {
            c = (c & 0x1F) << 6 | (p[0] & 0x3F);
            p += 1;
        } else if (c >= 0x80) {
            c = '?';
        }
        e[2 + k * 2] = (unsigned char)c;
        e[3 + k * 2] = (unsigned char)(c >> 8);
        k++;
    }
}

/* A copy of one of the select's text packs (the names, or the forms' labels) with the packages' texts in it: a
   line more for every added character (`form` 0 its name; 1 its label, or without one its base character's line),
   and a replaced character's own line changed where its package gives a name (a label). The pack itself where no
   package says anything. */
static void *text_pack(void *pack, unsigned char *out, unsigned size, int form, int *first) {
    const unsigned char *src = pack;
    unsigned count, total, head, i;
    unsigned *words = (unsigned *)out;
    int any, k;

    PortCharas_Init();
    any = live() > 0;
    for (k = 0; k < rlive(); k++) {
        any |= form ? sRepl[k].labelSet : sRepl[k].nameSet;
    }
    memcpy(&count, src, 4);
    *first = -1;
    if (!any || count == 0 || count > NAME_LINES_MAX) {
        return pack;
    }
    total = count + (unsigned)live();
    head = (4 * (total + 2) + 63) & ~63u;
    memset(out, 0, size);
    words[0] = total;
    for (i = 0; i < total; i++) {
        unsigned char *e = out + head + i * NAME_ENTRY;
        const Chara *c = i < count ? repl_of((int)i) : &sChara[i - count];
        unsigned line = i;
        words[i + 1] = head + i * NAME_ENTRY;
        if (c != NULL && i < count && !(form ? c->labelSet : c->nameSet)) {
            c = NULL; /* (a replacement that does not say: the disc's line) */
        }
        if (c != NULL && form && !c->labelSet) {
            line = (unsigned)c->base; /* (no label of its own: its base character's) */
            c = NULL;
        }
        if (c == NULL) {
            unsigned a, b;
            if (line >= count) {
                continue;
            }
            memcpy(&a, src + 4 * (line + 1), 4);
            memcpy(&b, src + 4 * (line + 2), 4);
            a &= ~3u;
            b &= ~3u;
            if (b > a) {
                memcpy(e, src + a, b - a < NAME_ENTRY - 2 ? b - a : NAME_ENTRY - 2);
            }
        } else {
            text16(e, form ? c->label : c->shown);
            if (i < count && getenv("BT3_CHARA_LOG") != NULL) {
                fprintf(stderr, "bt3: characters: the %s of character %u is \"%s\" (%s)\n", form ? "label" : "name", i, form ? c->label : c->shown, c->name);
            }
        }
    }
    words[total + 1] = head + total * NAME_ENTRY;
    *first = live() > 0 ? (int)count : -1;
    return out;
}

void *Port_CharaNamePack(void *pack) {
    return text_pack(pack, sNames, sizeof(sNames), 0, &sNameFirst);
}

void *Port_CharaFormPack(void *pack) {
    return text_pack(pack, sForms, sizeof(sForms), 1, &sFormFirst);
}

int Port_CharaFormLine(int chara) {
    int index = chara - PORT_CHARA_FIRST;
    return sFormFirst >= 0 && index >= 0 && index < live() ? sFormFirst + index : Port_CharaBase(chara);
}

int Port_CharaNameLine(int chara) {
    int index = chara - PORT_CHARA_FIRST;
    return sNameFirst >= 0 && index >= 0 && index < live() ? sNameFirst + index : Port_CharaBase(chara);
}

/* BT3_TEST_CHARA=<id>[,<id>]: the first fighter of side 0 (and of side 1) of every battle the menus set up is
   that character, whatever was chosen (testing: a recorded session then plays on an added character). */
int Port_TestChara(int side) {
    const char *e = getenv("BT3_TEST_CHARA"), *comma;
    if (e == NULL || e[0] == '\0' || Port_NetSession()) {
        return -1;
    }
    comma = strchr(e, ',');
    if (side == 0) {
        return (int)strtol(e, NULL, 0);
    }
    return comma != NULL ? (int)strtol(comma + 1, NULL, 0) : -1;
}

/* BT3_TEST_CELL: the character select starts with side 0's cursor on the first added character (testing);
   BT3_TEST_CELL=disc:<id>: on that character of the disc. */
int Port_TestCell(void) {
    const char *e = getenv("BT3_TEST_CELL");
    return e != NULL && e[0] != '\0' && !Port_NetSession();
}

int Port_TestCellIs(int id) {
    const char *e = getenv("BT3_TEST_CELL");
    int want;
    if (e != NULL && strncmp(e, "disc:", 5) == 0) {
        return number(e + 5, 0, DISC_CHARAS - 1, &want) && id == want;
    }
    return id >= PORT_CHARA_FIRST && id < PORT_CHARA_FIRST + PORT_CHARA_MAX;
}
