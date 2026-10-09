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
 * folders, by name, become the characters 0xA5, 0xA6, ... (include/port_chara.h). The game asks for an added
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
    char form[7][64];              /* forms.txt: the folder (or "disc:<id>") of each slot; "" = not said */
    int target[7];                 /* ... as an id: -1 not said, 0xFF not installed */
    char shown[64];                /* the name for the menus: name.txt's first line, or the folder's name */
} Chara;

enum { CHIP_SIZE = 5504, NAME_ENTRY = 128, NAME_LINES_MAX = 256 };
/* (static: the program lies below 4 GB, where the game's 32-bit pointers reach) */
static unsigned char sChip[PORT_CHARA_MAX][CHIP_SIZE] __attribute__((aligned(64)));
static unsigned char sChipHave[PORT_CHARA_MAX];
static unsigned char sNames[64 + 4 * (NAME_LINES_MAX + PORT_CHARA_MAX + 2) + (NAME_LINES_MAX + PORT_CHARA_MAX) * NAME_ENTRY] __attribute__((aligned(64)));
static int sNameFirst = -1; /* the line of the first added character in sNames */

static Chara sChara[PORT_CHARA_MAX];
static int sCount;
static int sInit;

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

static void add(const char *root, const char *name) {
    Chara *c = &sChara[sCount];
    char path[400];
    DIR *d;
    struct dirent *de;
    FILE *f;
    int k;

    snprintf(path, sizeof(path), "%s/%s", root, name);
    d = opendir(path);
    if (d == NULL) {
        return;
    }
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    while ((de = readdir(d)) != NULL) {
        k = kind_of(de->d_name);
        if (k >= 0 && c->file[k][0] == '\0') {
            snprintf(c->file[k], sizeof(c->file[k]), "%s/%s", path, de->d_name);
        }
    }
    closedir(d);
    if (c->file[0][0] == '\0') { /* no first costume: not a character's folder */
        return;
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
        char line[128], *eq;
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
        }
        fclose(f);
    }
    snprintf(c->shown, sizeof(c->shown), "%s", name);
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
    for (c->costumes = 1; c->costumes < 4 && c->file[c->costumes][0] != '\0'; c->costumes++) {
    }
    for (k = 0; k < 8; k++) { /* (a model larger than the buffer the battle reads it into would be read past its end) */
        struct stat st;
        if (c->file[k][0] != '\0' && stat(c->file[k], &st) == 0 && st.st_size > 0x100000) {
            fprintf(stderr, "bt3: characters: %s: %s is %ld bytes, more than the 1 MB a model may have; the character is left out\n", name,
                    strrchr(c->file[k], '/') + 1, (long)st.st_size);
            return;
        }
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
}

void PortCharas_Init(void) {
    static char names[64][128];
    char root[300];
    DIR *d;
    struct dirent *de;
    int n = 0, i;

    if (sInit) {
        return;
    }
    sInit = 1;
    if (!PortExtras_Dir("characters", "BT3_CHARACTERS", root, sizeof(root))) {
        return;
    }
    d = opendir(root);
    if (d == NULL) {
        return;
    }
    while ((de = readdir(d)) != NULL && n < 64) {
        if (de->d_name[0] != '.' && strlen(de->d_name) < 64) {
            snprintf(names[n++], 128, "%s", de->d_name);
        }
    }
    closedir(d);
    qsort(names, (size_t)n, 128, name_cmp); /* by name: the same order on every machine */
    for (i = 0; i < n && sCount < PORT_CHARA_MAX; i++) {
        add(root, names[i]);
    }
    for (i = 0; i < sCount; i++) { /* forms.txt: folders to ids, now that every folder has its id */
        int k, j;
        for (k = 0; k < 7; k++) {
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
            for (j = 0; j < sCount; j++) {
                if (strcmp(sChara[j].name, want) == 0) {
                    sChara[i].target[k] = PORT_CHARA_FIRST + j;
                }
            }
            if (sChara[i].target[k] == 0xFF) {
                fprintf(stderr, "bt3: characters: %s: its form %s is not installed; that transformation is left out\n", sChara[i].name, want);
            }
        }
    }
    if (sCount > 0) {
        fprintf(stderr, "bt3: characters: %d added from %s\n", sCount, root);
        for (i = 0; i < sCount; i++) {
            fprintf(stderr, "bt3: characters:   %s: id %d, %d costume%s, base character %d\n", sChara[i].name, PORT_CHARA_FIRST + i,
                    sChara[i].costumes, sChara[i].costumes == 1 ? "" : "s", sChara[i].base);
        }
    }
}

int Port_CharaCount(void) {
    PortCharas_Init();
    return sCount;
}

int Port_CharaBase(int chara) {
    PortCharas_Init();
    if (chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + sCount) {
        return sChara[chara - PORT_CHARA_FIRST].base;
    }
    return chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + PORT_CHARA_MAX ? 0 : chara; /* (an id nothing is behind) */
}

int Port_CharaCostumes(int chara) {
    PortCharas_Init();
    return chara >= PORT_CHARA_FIRST && chara < PORT_CHARA_FIRST + sCount ? sChara[chara - PORT_CHARA_FIRST].costumes : 0;
}

const char *Port_CharaName(int index) {
    PortCharas_Init();
    return index >= 0 && index < sCount ? sChara[index].name : "";
}

int PortCharas_Alias(const char *rel, char *out, unsigned n) {
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
        for (t = 0; who < sCount && t < 2; t++) {
            FILE *f;
            snprintf(out, n, "%s/voice_%s/%03d.adx", sChara[who].dir, (alt ^ t) ? "jp" : "us", line);
            if ((f = fopen(out, "rb")) != NULL) {
                fclose(f);
                return 1;
            }
        }
        snprintf(out, n, "pzs3us2/%05d.bin", (alt ? 0xCC32 : 0x8D4E) - 0xD48 + (who < sCount ? sChara[who].base : 0) * 100 + line);
        return 1;
    }
    if (index >= PORT_CHARA_FACE_INDEX && index < PORT_CHARA_FACE_INDEX + PORT_CHARA_MAX) {
        /* the large picture: the folder's, or the base character's (the disc's 0x2F9 + id) */
        who = index - PORT_CHARA_FACE_INDEX;
        if (who < sCount && sChara[who].portrait[0] != '\0') {
            snprintf(out, n, "%s", sChara[who].portrait);
        } else {
            snprintf(out, n, "pzs3us1/%05d.bin", 0x2F8 + (who < sCount ? sChara[who].base : 0));
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
    c = &sChara[who < sCount ? who : 0];
    if (who >= sCount) { /* an id nothing is behind (a save or a replay of another set-up): the first character of the disc */
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
    return index >= 0 && index < sCount && slot >= 0 && slot < 7 ? sChara[index].target[slot] : -1;
}

/* whether added character j is reached by a transformation (left, up, right) of another added one */
static int reached(int j) {
    int i, k;
    for (i = 0; i < sCount; i++) {
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
            if (t < 0 || t >= sCount) {
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
    if (index < 0 || index >= sCount || max < 1) {
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
    if (index < 0 || index >= sCount) {
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
    return index >= 0 && index < sCount && sChipHave[index] ? sChip[index] : NULL;
}

void *Port_CharaNamePack(void *pack) {
    const unsigned char *src = pack;
    unsigned count, total, head, i, k;
    unsigned *words = (unsigned *)sNames;

    PortCharas_Init();
    memcpy(&count, src, 4);
    if (sCount == 0 || count == 0 || count > NAME_LINES_MAX) {
        sNameFirst = -1;
        return pack;
    }
    total = count + (unsigned)sCount;
    head = (4 * (total + 2) + 63) & ~63u;
    memset(sNames, 0, sizeof(sNames));
    words[0] = total;
    for (i = 0; i < total; i++) {
        unsigned char *e = sNames + head + i * NAME_ENTRY;
        words[i + 1] = head + i * NAME_ENTRY;
        if (i < count) {
            unsigned a, b;
            memcpy(&a, src + 4 * (i + 1), 4);
            memcpy(&b, src + 4 * (i + 2), 4);
            a &= ~3u;
            b &= ~3u;
            if (b > a) {
                memcpy(e, src + a, b - a < NAME_ENTRY - 2 ? b - a : NAME_ENTRY - 2);
            }
        } else {
            const char *name = sChara[i - count].shown;
            e[0] = 0xFF;
            e[1] = 0xFE;
            for (k = 0; name[k] != '\0' && k < (NAME_ENTRY - 4) / 2; k++) {
                e[2 + k * 2] = (unsigned char)name[k] < 0x80 ? (unsigned char)name[k] : '?';
            }
        }
    }
    words[total + 1] = head + total * NAME_ENTRY;
    sNameFirst = (int)count;
    return sNames;
}

int Port_CharaNameLine(int chara) {
    int index = chara - PORT_CHARA_FIRST;
    return sNameFirst >= 0 && index >= 0 && index < sCount ? sNameFirst + index : Port_CharaBase(chara);
}

/* BT3_TEST_CHARA=<id>[,<id>]: the first fighter of side 0 (and of side 1) of every battle the menus set up is
   that character, whatever was chosen (testing: a recorded session then plays on an added character). */
int Port_TestChara(int side) {
    const char *e = getenv("BT3_TEST_CHARA"), *comma;
    if (e == NULL || e[0] == '\0') {
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
    return e != NULL && e[0] != '\0';
}
