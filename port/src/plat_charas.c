/*
 * Characters added from outside the disc.
 *
 * `characters/<Name>/` next to the game ($BT3_CHARACTERS names another folder) holds one character as the
 * community's packs have it, files in the game's own format named by what they are:
 *
 *   *_1p.pak .. *_4p.pak            the model of costume 1..4       (the disc's file 0x590 + id * 10 + 0..3)
 *   *_1p_dmg.pak .. *_4p_dmg.pak    the damaged model of the same   (+ 4..7)
 *   *_anm.pak, *_eff.pak            animations, effects             (+ 8, + 9)
 *   *_Voice_US.pak, *_Voice_JP.pak  the voice bank in each language (0xBDA + id, 0xC7B + id)
 *
 * (Seen in one pack, 2026-10-08: "Gokhan_0_1p.pak" and so on, eight files; the same layout as the disc's.) The
 * folders, by name, become the characters 0xC6, 0xC7, ... (include/port_chara.h). The game asks for an added
 * character's files by ids past the end of the second archive, and they are answered here: with the folder's
 * file, or where it has none with the file of the character's BASE character, one of the disc's, which also
 * stands for it in every table (health, cost, AI, what is said to whom). `base.txt` in the folder holds the base
 * character's id (0..160); without it the base is character 0.
 */
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "plat_charas.h"

extern int PortExtras_Dir(const char *kind, const char *env, char *out, unsigned size); /* plat_extras.c */

enum { F_ANM = 8, F_EFF = 9, F_VOICE = 10, F_COUNT = 12 };

typedef struct {
    char name[64];
    char file[F_COUNT][400]; /* "" where the folder has none */
    int base;
    int costumes;
    char chip[400], portrait[400]; /* "" where the folder has none */
    char dir[400];                 /* the folder */
    char form[19][64];             /* forms.txt: the folder (or "disc:<id>") of each slot; "" = not said. 0..3 the
                                      transformations, 4..6 the fusions, 7 + n * 4 + m partner m of fusion n */
    int target[19];                /* ... as an id: -1 not said, 0xFF not installed */
    char shown[64];                /* the name for the menus: name.txt's first line, or the folder's name */
    int parent;                    /* -1, or for a form kept in a folder inside its character's: that character */
    char label[96];                /* form.txt: the form's label under the name ("Super Saiyan") */
    int labelSet;                  /* there is a form.txt (an empty one: no label); without, the base's label */
} Chara;

enum { CHIP_SIZE = 5504, NAME_ENTRY = 128, NAME_LINES_MAX = 256 };
/* (static: the program lies below 4 GB, where the game's 32-bit pointers reach) */
static unsigned char sChip[PORT_CHARA_MAX][CHIP_SIZE] __attribute__((aligned(64)));
static unsigned char sChipHave[PORT_CHARA_MAX];
static unsigned char sNames[64 + 4 * (NAME_LINES_MAX + PORT_CHARA_MAX + 2) + (NAME_LINES_MAX + PORT_CHARA_MAX) * NAME_ENTRY] __attribute__((aligned(64)));
static int sNameFirst = -1; /* the line of the first added character in sNames */
static unsigned char sForms[sizeof(sNames)] __attribute__((aligned(64))); /* the same for the forms' labels */
static int sFormFirst = -1;

static Chara sChara[PORT_CHARA_MAX];
static int sCount;
static int sInit;

/* Online play does not know the added characters. In a session (Dragon Net Battle; gs/net.c) everything this file
   answers is what it answers with no `characters` folder at all: no character, no cell, no name line, no chip, no
   memory of the game's taken, the original's model buffers; so the two copies of a match compute the same game
   whatever each player has installed. A session that starts the program again does not read the folder either
   (PortCharas_Init). One that begins in the running program (gs/state.c) finds the folder read: live() hides it
   until the session is over. */
extern int Port_NetSession(void); /* gs/net.c */
static int live(void) {
    return Port_NetSession() ? 0 : sCount;
}

/* which of the twelve files a name is, by its ending; -1 if none */
static int kind_of(const char *name) {
    static const struct { const char *end; int kind; } kEnds[] = {
        {"_1p.pak", 0}, {"_2p.pak", 1}, {"_3p.pak", 2}, {"_4p.pak", 3},
        {"_1p_dmg.pak", 4}, {"_2p_dmg.pak", 5}, {"_3p_dmg.pak", 6}, {"_4p_dmg.pak", 7},
        {"_anm.pak", F_ANM}, {"_eff.pak", F_EFF}, {"_voice_us.pak", F_VOICE}, {"_voice_jp.pak", F_VOICE + 1},
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

/* Adds the folder `name` of `root` if it holds a character. `parent`: -1, or the character whose folder `root` is
   (a form of it: what its base.txt and name.txt do not say is the parent's). */
static int add(const char *root, const char *name, int parent) {
    Chara *c = &sChara[sCount];
    char path[400];
    DIR *d;
    struct dirent *de;
    FILE *f;
    int k;

    snprintf(path, sizeof(path), "%s/%s", root, name);
    d = opendir(path);
    if (d == NULL) {
        return 0;
    }
    memset(c, 0, sizeof(*c));
    c->parent = parent;
    snprintf(c->name, sizeof(c->name), "%s", name);
    while ((de = readdir(d)) != NULL) {
        k = kind_of(de->d_name);
        if (k >= 0 && c->file[k][0] == '\0') {
            snprintf(c->file[k], sizeof(c->file[k]), "%s/%s", path, de->d_name);
        }
    }
    closedir(d);
    if (c->file[0][0] == '\0') { /* no first costume: not a character's folder */
        return 0;
    }
    snprintf(c->dir, sizeof(c->dir), "%s/%s", root, name);
    snprintf(path, sizeof(path), "%s/%s/chip.bin", root, name);
    if ((f = fopen(path, "rb")) != NULL) {
        fclose(f);
        snprintf(c->chip, sizeof(c->chip), "%s", path);
    }
    snprintf(path, sizeof(path), "%s/%s/portrait.bin", root, name);
    if ((f = fopen(path, "rb")) != NULL) {
        fclose(f);
        snprintf(c->portrait, sizeof(c->portrait), "%s", path);
    }
    snprintf(path, sizeof(path), "%s/%s/forms.txt", root, name);
    if ((f = fopen(path, "r")) != NULL) {
        static const char *const kSlot[7] = {"left", "up", "right", "down", "fusion1", "fusion2", "fusion3"};
        char line[640], *eq; /* (a partner line can name four folders) */
        while (fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            eq = strchr(line, '=');
            if (eq == NULL) {
                continue;
            }
            *eq = '\0';
            for (k = 0; k < 7; k++) {
                if (strcmp(line, kSlot[k]) == 0) {
                    snprintf(c->form[k], sizeof(c->form[k]), "%s", eq + 1);
                }
            }
            if (strncmp(line, "partner", 7) == 0 && line[7] >= '1' && line[7] <= '3' && line[8] == '\0') {
                /* partnerN=Folder,Folder,...: who must be on the team for fusion N (up to four; the character's own
                   table has ids, which for an added partner are not known until the folders are counted) */
                int n = line[7] - '1', m = 0;
                char *at = eq + 1;
                while (m < 4 && *at != '\0') {
                    size_t len = strcspn(at, ",");
                    snprintf(c->form[7 + n * 4 + m], sizeof(c->form[0]), "%.*s", (int)len, at);
                    at += len + (at[len] == ',');
                    m++;
                }
            }
        }
        fclose(f);
    }
    snprintf(c->shown, sizeof(c->shown), "%s", parent >= 0 ? sChara[parent].shown : name);
    snprintf(path, sizeof(path), "%s/%s/name.txt", root, name);
    if ((f = fopen(path, "r")) != NULL) {
        char line[64];
        if (fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            if (line[0] != '\0') {
                snprintf(c->shown, sizeof(c->shown), "%s", line);
            }
        }
        fclose(f);
    }
    snprintf(path, sizeof(path), "%s/%s/form.txt", root, name);
    if ((f = fopen(path, "r")) != NULL) {
        char line[96];
        if (fgets(line, sizeof(line), f) != NULL) {
            line[strcspn(line, "\r\n")] = '\0';
            snprintf(c->label, sizeof(c->label), "%s", line);
        }
        c->labelSet = 1;
        fclose(f);
    }
    for (c->costumes = 1; c->costumes < 4 && c->file[c->costumes][0] != '\0'; c->costumes++) {
    }
    for (k = 0; k < 8; k++) { /* (a model larger than the buffer the battle reads it into would be read past its end) */
        struct stat st;
        if (c->file[k][0] != '\0' && stat(c->file[k], &st) == 0 && st.st_size > PORT_MODEL_MAX) {
            fprintf(stderr, "bt3: characters: %s: %s is %ld bytes, more than the 1.5 MB a model may have; the character is left out\n", name,
                    strrchr(c->file[k], '/') + 1, (long)st.st_size);
            return 0;
        }
    }
    if (parent >= 0) {
        c->base = sChara[parent].base;
    }
    snprintf(path, sizeof(path), "%s/%s/base.txt", root, name);
    f = fopen(path, "r");
    if (f != NULL) {
        if (fscanf(f, "%d", &c->base) != 1 || c->base < 0 || c->base > 160) {
            fprintf(stderr, "bt3: characters: %s: base.txt has no character id from 0 to 160; 0 is used\n", name);
            c->base = 0;
        }
        fclose(f);
    }
    sCount++;
    return 1;
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
        if (de->d_name[0] != '.' && strlen(de->d_name) < 64 &&
            !(skip && (strcmp(de->d_name, "voice_us") == 0 || strcmp(de->d_name, "voice_jp") == 0))) {
            snprintf(names[n++], 128, "%s", de->d_name);
        }
    }
    closedir(d);
    qsort(names, (size_t)n, 128, name_cmp);
    return n;
}

/* the id of the folder a forms.txt names, as character i's folder sees it: ".." its own character (for a form
   kept inside), a form inside its own character's folder, a character of the top, or "Character/Form"; 0xFF if
   there is none */
static int find(int i, const char *want) {
    int family = sChara[i].parent >= 0 ? sChara[i].parent : i, j;
    const char *slash = strchr(want, '/');
    if (strcmp(want, "..") == 0) {
        return sChara[i].parent >= 0 ? PORT_CHARA_FIRST + family : 0xFF;
    }
    for (j = 0; j < sCount; j++) {
        if (sChara[j].parent == family && strcmp(sChara[j].name, want) == 0) {
            return PORT_CHARA_FIRST + j;
        }
    }
    for (j = 0; j < sCount; j++) {
        if (sChara[j].parent < 0 && strcmp(sChara[j].name, want) == 0) {
            return PORT_CHARA_FIRST + j;
        }
    }
    for (j = 0; slash != NULL && j < sCount; j++) {
        int p = sChara[j].parent;
        if (p >= 0 && strlen(sChara[p].name) == (size_t)(slash - want) && strncmp(sChara[p].name, want, (size_t)(slash - want)) == 0 &&
            strcmp(sChara[j].name, slash + 1) == 0) {
            return PORT_CHARA_FIRST + j;
        }
    }
    return 0xFF;
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
    for (i = 0; i < n && sCount < PORT_CHARA_MAX; i++) {
        int owner = sCount, m, k;
        if (!add(root, names[i], -1)) {
            continue;
        }
        /* the folders inside a character's folder are its forms: characters of their own, right after it */
        m = folders(sChara[owner].dir, inner, 64, 1);
        for (k = 0; k < m && sCount < PORT_CHARA_MAX; k++) {
            add(sChara[owner].dir, inner[k], owner);
        }
    }
    for (i = 0; i < sCount; i++) { /* forms.txt: folders to ids, now that every folder has its id */
        int k;
        for (k = 0; k < 19; k++) {
            const char *want = sChara[i].form[k];
            sChara[i].target[k] = -1;
            if (want[0] == '\0') {
                continue;
            }
            sChara[i].target[k] = 0xFF;
            if (strncmp(want, "disc:", 5) == 0) {
                int id = atoi(want + 5);
                sChara[i].target[k] = id >= 0 && id <= 160 ? id : 0xFF;
                continue;
            }
            sChara[i].target[k] = find(i, want);
            if (sChara[i].target[k] == 0xFF && k < 7) {
                int seen = 0, j;
                for (j = 0; j < k; j++) { /* (said once for a folder, however many slots name it) */
                    seen |= strcmp(sChara[i].form[j], want) == 0;
                }
                if (seen) {
                    continue;
                }
                fprintf(stderr, "bt3: characters: %s: %s, named in its forms.txt, is not installed; that %s is left out\n", sChara[i].name, want,
                        k < 4 ? "transformation" : "fusion");
            }
        }
    }
    if (sCount > 0) {
        fprintf(stderr, "bt3: characters: %d added from %s\n", sCount, root);
        for (i = 0; i < sCount; i++) {
            fprintf(stderr, "bt3: characters:   %s%s%s: id %d, %d costume%s, base character %d\n",
                    sChara[i].parent >= 0 ? sChara[sChara[i].parent].name : "", sChara[i].parent >= 0 ? "/" : "", sChara[i].name, PORT_CHARA_FIRST + i,
                    sChara[i].costumes, sChara[i].costumes == 1 ? "" : "s", sChara[i].base);
        }
    }
}

/* The size of the fight's model buffers: the original's (0xCE000), or the largest model file there is to load if that
   is larger: an added character's, or one of the data folder's own (0x590 + id * 10 + 0..7, the first archive), which
   a disc with replaced characters can have. Whole pages; not over PORT_MODEL_MAX (a larger file of the data folder
   is reported once: it would be read past the buffer's end). */
int Port_ModelSize(void) {
    static int size;
    if (size == 0) {
        const char *root = getenv("BT3_DATA") != NULL ? getenv("BT3_DATA") : "gamedata";
        long most = 0xCE000;
        int i, k, over = 0;
        char path[600];
        struct stat st;
        PortCharas_Init();
        for (i = 0; i < sCount; i++) {
            for (k = 0; k < 8; k++) {
                if (sChara[i].file[k][0] != '\0' && stat(sChara[i].file[k], &st) == 0 && st.st_size > most) {
                    most = (long)st.st_size;
                }
            }
        }
        for (i = 0; i < 161; i++) {
            for (k = 0; k < 8; k++) {
                snprintf(path, sizeof(path), "%s/pzs3us1/%05d.bin", root, 0x590 + i * 10 + k - 1);
                if (stat(path, &st) == 0 && st.st_size > most) {
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

int Port_CharaBase(int chara) {
    PortCharas_Init();
    if (chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + live()) {
        return sChara[chara - PORT_CHARA_FIRST].base;
    }
    return chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + PORT_CHARA_MAX ? 0 : chara; /* (an id nothing is behind) */
}

int Port_CharaCostumes(int chara) {
    PortCharas_Init();
    return chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + live() ? sChara[chara - PORT_CHARA_FIRST].costumes : 0;
}

const char *Port_CharaName(int index) {
    PortCharas_Init();
    return index >= 0 && index < live() ? sChara[index].name : "";
}

static int alias(const char *rel, char *out, unsigned n);

/* (BT3_CHARA_LOG: every file answered for an added character, once each: the folder's own or the disc's it falls
   back on) */
int PortCharas_Alias(const char *rel, char *out, unsigned n) {
    static char said[64][24];
    static int saidCount, log = -1;
    int r = alias(rel, out, n), i;
    if (log < 0) {
        log = getenv("BT3_CHARA_LOG") != NULL;
    }
    if (r && log) {
        for (i = 0; i < saidCount && strcmp(said[i], rel) != 0; i++) {
        }
        if (i == saidCount && saidCount < 64 && strlen(rel) < sizeof(said[0])) {
            strcpy(said[saidCount++], rel);
            fprintf(stderr, "bt3: characters: file %s is %s\n", rel, out);
        }
    }
    return r;
}

static int alias(const char *rel, char *out, unsigned n) {
    int index = -1, who, k;
    const Chara *c;

    if (strncmp(rel, "pzs3us2/", 8) != 0 || sscanf(rel + 8, "%d", &index) != 1 || index < PORT_CHARA_FILE_INDEX) {
        return 0;
    }
    PortCharas_Init();
    if (index >= PORT_CHARA_LINE_INDEX && index < PORT_CHARA_LINE_INDEX + PORT_CHARA_MAX * 200) {
        /* a spoken line: the folder's voice_us/NNN.adx or voice_jp/NNN.adx (the other language's where only one is
           there), or the base character's line of the disc (0x8D4E + id * 100 + line, and 0xCC32 + ...) */
        int alt = (index - PORT_CHARA_LINE_INDEX) % 200 / 100, line = (index - PORT_CHARA_LINE_INDEX) % 100, t;
        who = (index - PORT_CHARA_LINE_INDEX) / 200;
        for (t = 0; who < live() && t < 2; t++) {
            FILE *f;
            snprintf(out, n, "%s/voice_%s/%03d.adx", sChara[who].dir, (alt ^ t) ? "jp" : "us", line);
            if ((f = fopen(out, "rb")) != NULL) {
                fclose(f);
                return 1;
            }
        }
        snprintf(out, n, "pzs3us2/%05d.bin", (alt ? 0xCC32 : 0x8D4E) - 0xD48 + (who < live() ? sChara[who].base : 0) * 100 + line);
        return 1;
    }
    if (index >= PORT_CHARA_MEMBER_INDEX && index < PORT_CHARA_MEMBER_INDEX + PORT_CHARA_MAX * 2) {
        /* the fight's member block (the team mate's small picture): the folder's member_1p.bin / member_2p.bin, or
           the base character's (the disc's 8 + id * 2 + side) */
        FILE *f;
        int side = (index - PORT_CHARA_MEMBER_INDEX) % 2;
        who = (index - PORT_CHARA_MEMBER_INDEX) / 2;
        if (who < live()) {
            snprintf(out, n, "%s/member_%dp.bin", sChara[who].dir, side + 1);
            if ((f = fopen(out, "rb")) != NULL) {
                fclose(f);
                return 1;
            }
        }
        snprintf(out, n, "pzs3us1/%05d.bin", 7 + (who < live() ? sChara[who].base : 0) * 2 + side);
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
        snprintf(out, n, k >= F_VOICE ? "pzs3us1/%05d.bin" : "pzs3us1/%05d.bin", k >= F_VOICE ? (k == F_VOICE ? 3033 : 3194) : 1423 + k);
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

int Port_CharaFormTarget(int chara, int slot) {
    int index = chara - PORT_CHARA_FIRST;
    PortCharas_Init();
    return index >= 0 && index < live() && slot >= 0 && slot < 19 ? sChara[index].target[slot] : -1;
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
    FILE *f;
    PortCharas_Init();
    if (index < 0 || index >= live()) {
        return NULL;
    }
    sChipHave[index] = 0;
    if (sChara[index].chip[0] != '\0' && (f = fopen(sChara[index].chip, "rb")) != NULL) {
        memset(sChip[index], 0, CHIP_SIZE);
        sChipHave[index] = fread(sChip[index], 1, CHIP_SIZE, f) >= 64;
        fclose(f);
    }
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

void PortChips_Load(void) {
    int i;
    if (Port_NetSession()) {
        return;
    }
    PortCharas_Init();
    PortChips_Free();
    if (live() == 0) {
        return;
    }
    sChipMem = Heap_Alloc(live() * CHIP_SIZE, 0x40, 0, 2);
    for (i = 0; sChipMem != NULL && i < live(); i++) {
        void *file = Port_CharaChipLoad(i);
        if (file != NULL) {
            void *res = sChipMem + i * CHIP_SIZE;
            memcpy(res, file, CHIP_SIZE);
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

/* A copy of one of the select's text packs (the names, or the forms' labels) with a line more for every added
   character: `form` 0 its name; 1 its label (form.txt), or without one its base character's line as before. */
static void *text_pack(void *pack, unsigned char *out, unsigned size, int form, int *first) {
    const unsigned char *src = pack;
    unsigned count, total, head, i;
    unsigned *words = (unsigned *)out;

    PortCharas_Init();
    memcpy(&count, src, 4);
    if (live() == 0 || count == 0 || count > NAME_LINES_MAX) {
        *first = -1;
        return pack;
    }
    total = count + (unsigned)live();
    head = (4 * (total + 2) + 63) & ~63u;
    memset(out, 0, size);
    words[0] = total;
    for (i = 0; i < total; i++) {
        unsigned char *e = out + head + i * NAME_ENTRY;
        const Chara *c = i < count ? NULL : &sChara[i - count];
        unsigned line = i;
        words[i + 1] = head + i * NAME_ENTRY;
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
        }
    }
    words[total + 1] = head + total * NAME_ENTRY;
    *first = (int)count;
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

/* BT3_TEST_CELL: the character select starts with side 0's cursor on the first added character (testing). */
int Port_TestCell(void) {
    const char *e = getenv("BT3_TEST_CELL");
    return e != NULL && e[0] != '\0' && !Port_NetSession();
}
