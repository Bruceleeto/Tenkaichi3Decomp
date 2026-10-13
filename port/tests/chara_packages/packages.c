/* The reader of character packages (port/src/plat_charas.c) on its own, against files a player may drop in: empty,
 * huge, binary and hostile manifests, and folders made to trip the scan. None of them may crash it or make it read
 * or write outside a buffer. Built with the address and undefined-behaviour sanitizers, which stop the program at
 * the first such read or write.
 *
 *   gcc -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -Iport/include \
 *       port/tests/chara_packages/packages.c -o packages_test
 *   packages_test --cases                 manifests with a known reading; ends with "cases OK"
 *   packages_test --fuzz <rounds> <seed>  made-up and damaged manifests; ends with "fuzz OK"
 *   packages_test --make <folder>         writes a `characters` folder of awkward packages into <folder> (new)
 *   packages_test --scan <folder>         reads a `characters` folder as the game does, asks for every file number
 *                                         a character has, for the texts and the chips, in the player's own game
 *                                         and as in an online session (where nothing may be answered); prints the
 *                                         packages and ends with "scan OK"
 *
 * Nothing of the game is needed: the few things plat_charas.c asks of it are stood in for here. */
#define _GNU_SOURCE
#include <stdint.h>
#include <unistd.h>
#include "../../src/plat_charas.c"

static int sSession;
int Port_NetSession(void) { return sSession; }
static const char *sRoot = "";
int PortExtras_Dir(const char *kind, const char *env, char *out, unsigned size) {
    (void)kind;
    (void)env;
    snprintf(out, size, "%s", sRoot);
    return sRoot[0] != '\0';
}
int Port_Setting(const char *name, int def) { (void)name; return def; }
void Port_SettingSave(const char *name, int value) { (void)name; (void)value; }
void *Heap_Alloc(int size, int align, int fromTail, int heap) { (void)align; (void)fromTail; (void)heap; return malloc((size_t)size); }
void Heap_Free(void *ptr) { free(ptr); }
/* as the game's (src/sys/gfxm_b_b.c) follows a texture file's offsets and writes the pointers */
void Res_RelocateOffsets(void *out, void *base, void *hdr) {
    unsigned char *b = base, *h = hdr;
    uint32_t count, ent, i, zero = 0;
    (void)out;
    memcpy(&count, h, 4);
    memcpy(&ent, h + 4, 4);
    memcpy(h + 0x10, &zero, 4);
    for (i = 0; i < count; i++) {
        unsigned char *e = b + ent * 4 + i * 0x40;
        uint32_t pix, clut;
        memcpy(&pix, e, 4);
        memcpy(&clut, e + 4, 4);
        memcpy(e + 0x38, &zero, 4);
        memcpy(e + 0x3C, &zero, 4);
        if (pix != 0) {
            zero += b[pix * 4];
        }
        if (clut != 0) {
            zero += b[clut * 4];
        }
        zero = 0;
    }
}

static uint64_t sSeed = 88172645463325252ull;
static uint32_t rnd(void) {
    sSeed ^= sSeed << 13;
    sSeed ^= sSeed >> 7;
    sSeed ^= sSeed << 17;
    return (uint32_t)(sSeed >> 16);
}

static int sFail;
static void expect(const char *what, const char *text, size_t len, const char *part) {
    static char out[2048];
    PortCharas_ParseTest(text, len, out, sizeof(out));
    if (strstr(out, part) == NULL) {
        printf("FAIL %s: wanted [%s] in\n     %s\n", what, part, out);
        sFail++;
    }
}
#define EXPECT(what, text, part) expect(what, text, sizeof(text) - 1, part)

static void cases(void) {
    static char big[TEXT_MAX * 4];
    size_t i;
    EXPECT("empty", "", "name=[test] form=[]0 base=0 replaces=-1 health=-1 ki=-1 blast=-1");
    EXPECT("plain", "format=1\nname=Gokhan\nform=Super Saiyan\nbase=12\n", "name=[Gokhan] form=[Super Saiyan]1 base=12 replaces=-1");
    EXPECT("spaces, case, crlf, comment", "\xEF\xBB\xBF# a comment\r\n  Name =  Two Words  \r\nBASE= 160\r\n#base=3\r\n", "name=[Two Words] form=[]0 base=160");
    EXPECT("empty form is a label", "form=\n", "form=[]1");
    EXPECT("empty name is no name", "name=\n", "name=[test]");
    EXPECT("unknown keys", "future=1\nname=A\n=x\nno equals sign\n", "name=[A]");
    EXPECT("replaces", "replaces=0\n", "replaces=0 ");
    EXPECT("replaces 160", "replaces=160", "replaces=160 ");
    EXPECT("replaces out of range", "replaces=161\n", "replaces=-2 ");
    EXPECT("replaces not a number", "replaces=Goku\n", "problem=[replaces has no character id from 0 to 160]");
    EXPECT("replaces with rubbish after", "replaces=3x\n", "replaces=-2 ");
    EXPECT("base out of range", "base=-1\n", "base=0 replaces=-1 health=-1 ki=-1 blast=-1 author=[] version=[] about=[] left=[] partner1=[|||] problem=[base has no");
    EXPECT("base huge", "base=99999999999999999999999999\n", "base=0 ");
    EXPECT("hex", "base=0x10\n", "base=16 ");
    EXPECT("stats", "health=50000\nki=0\nblast=7\n", "health=50000 ki=0 blast=7");
    EXPECT("stats out of range", "health=1\nki=100001\nblast=8\n", "health=-1 ki=-1 blast=-1");
    EXPECT("stats taken back", "health=50000\nhealth=\n", "health=-1");
    EXPECT("links", "left= Form A \npartner1= X , Y,,Z ,W,V\n", "left=[Form A] partner1=[X|Y||Z]");
    EXPECT("last one counts", "name=A\nname=B\n", "name=[B]");
    EXPECT("newer format", "format=7\nname=A\n", "made for package format 7");
    EXPECT("texts", "author=Somebody\nversion=1.2\nabout=What it is\n", "author=[Somebody] version=[1.2] about=[What it is]");
    EXPECT("control characters", "name=A\x01\x1b[31mB\tC\n", "name=[A  [31mB C]");
    EXPECT("a NUL in a line", "name=AB\0CD\nbase=5\n", "name=[AB] form=[]0 base=5");
    EXPECT("no newline at the end", "base=7", "base=7 ");
    EXPECT("utf-8 cut on a character", "name=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xE3\x81\x82\xE3\x81\x82\n",
           "name=[aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa]");
    /* one line longer than everything that is read; a key and a value longer than their fields */
    memset(big, 'x', sizeof(big));
    expect("one huge line", big, sizeof(big), "name=[test]");
    memcpy(big, "name=", 5);
    expect("huge value", big, sizeof(big), "name=[xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx]");
    memcpy(big, "left=", 5);
    expect("huge link", big, sizeof(big), "left=[xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx]");
    memcpy(big, "about", 5);
    big[TEXT_MAX / 2] = '=';
    expect("huge key", big, sizeof(big), "about=[]");
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (char)(i % 7 == 0 ? '\n' : i % 3 == 0 ? '=' : 'a' + i % 26);
    }
    expect("thousands of lines", big, sizeof(big), "replaces=-1");
    for (i = 0; i < sizeof(big); i++) {
        big[i] = (char)(i * 2654435761u >> 24);
    }
    expect("binary", big, sizeof(big), "replaces=");
    memset(big, '\n', sizeof(big));
    expect("only newlines", big, sizeof(big), "name=[test]");
    memset(big, '=', sizeof(big));
    expect("only equals signs", big, sizeof(big), "name=[test]");
    memset(big, 0, sizeof(big));
    expect("only NULs", big, sizeof(big), "name=[test]");
    memset(big, 0xFF, sizeof(big));
    expect("only 0xFF", big, sizeof(big), "name=[test]");
    printf(sFail == 0 ? "cases OK\n" : "%d cases FAILED\n", sFail);
}

static void fuzz(int rounds, uint64_t seed) {
    static const char *const kKeys[] = {"format", "name", "form", "base", "replaces", "author", "version", "about", "left", "up", "right", "down",
                                        "fusion1", "fusion2", "fusion3", "partner1", "partner2", "partner3", "health", "ki", "blast", "partner4",
                                        "partner", "", "#", "NAME", " base "};
    static const char *const kValues[] = {"", "0", "160", "161", "-1", "0x7fffffff", "99999999999999999999", "disc:5", "disc:999", "..", "A/B", ",,,,,,,",
                                          "a,b,c,d,e,f", "\xE3\x81", "\xF0\x9F\x98\x80", " ", "=", "\r", "1e9", "+5", "0x", "07"};
    static char text[TEXT_MAX * 2], out[2048];
    int r;
    sSeed = seed != 0 ? seed : 1;
    for (r = 0; r < rounds; r++) {
        size_t n = 0, want = rnd() % 5 == 0 ? rnd() % sizeof(text) : rnd() % 400;
        while (n < want) {
            unsigned kind = rnd() % 10;
            if (kind < 5) {
                n += (size_t)snprintf(text + n, sizeof(text) - n, "%s%s%s%s", kKeys[rnd() % (sizeof(kKeys) / sizeof(kKeys[0]))], rnd() % 8 != 0 ? "=" : "",
                                      kValues[rnd() % (sizeof(kValues) / sizeof(kValues[0]))], rnd() % 6 != 0 ? "\n" : rnd() % 2 != 0 ? "\r\n" : "");
            } else if (kind < 7) {
                unsigned k, run = rnd() % 300;
                for (k = 0; k < run && n < sizeof(text) - 1; k++) {
                    text[n++] = (char)rnd();
                }
            } else {
                unsigned k, run = rnd() % 200, c = rnd();
                for (k = 0; k < run && n < sizeof(text) - 1; k++) {
                    text[n++] = (char)c;
                }
            }
            if (n >= sizeof(text) - 1) {
                n = sizeof(text) - 1;
                break;
            }
        }
        PortCharas_ParseTest(text, n, out, sizeof(out));
    }
    printf("fuzz OK (%d rounds)\n", rounds);
}

/* ---- a folder of awkward packages --------------------------------------------------------------------------- */

static void file(const char *dir, const char *name, const void *data, size_t n) {
    char path[1200];
    FILE *f;
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(data, 1, n, f);
        fclose(f);
    }
}
static void filled(const char *dir, const char *name, size_t n, int byte) {
    char *d = malloc(n + 1);
    memset(d, byte, n + 1);
    file(dir, name, d, n);
    free(d);
}
static const char *sub(const char *root, const char *name) {
    static char path[8][1200];
    static int at;
    char *p = path[at++ & 7];
    snprintf(p, 1200, "%s/%s", root, name);
    mkdir(p, 0777);
    return p;
}
#define TEXT(dir, name, text) file(dir, name, text, sizeof(text) - 1)

static void chip_file(const char *dir, const char *name, uint32_t count, uint32_t ent, uint32_t pix, uint32_t clut, size_t size) {
    unsigned char d[CHIP_SIZE + 64];
    memset(d, 0, sizeof(d));
    memcpy(d, &count, 4);
    memcpy(d + 4, &ent, 4);
    if ((size_t)ent * 4 + 8 <= CHIP_SIZE) {
        memcpy(d + ent * 4, &pix, 4);
        memcpy(d + ent * 4 + 4, &clut, 4);
    }
    file(dir, name, d, size);
}

static void make(const char *root) {
    const char *d, *e;
    char longName[300];
    int i;
    mkdir(root, 0777);
    d = sub(root, "A plain added one"); /* (the "models" are not models: nothing here loads them) */
    filled(d, "x_1p.pak", 5000, 1);
    filled(d, "x_2P.PAK", 5000, 1);
    filled(d, "x_1p_dmg.pak", 5000, 1);
    filled(d, "X_ANM.pak", 5000, 1);
    filled(d, "x_eff.pak", 5000, 1);
    filled(d, "x_Voice_US.pak", 5000, 1);
    filled(d, "portrait.bin", 5000, 1);
    filled(d, "Member_1P.bin", 3000, 1);
    chip_file(d, "chip.bin", 1, 8, 0x20, 0x30, CHIP_SIZE);
    TEXT(d, "character.txt", "format=1\nname=Plain\nbase=3\nauthor=A\nversion=1\nabout=about\nleft=Form\nright=Nobody\nfusion1=disc:4\npartner1=Form,..,disc:999\n");
    e = sub(d, "voice_us");
    filled(e, "000.adx", 100, 2);
    e = sub(d, "Form");
    filled(e, "f_1p.pak", 5000, 1);
    TEXT(e, "character.txt", "down=..\nform=The Form\n");
    e = sub(d, "ReplForm");
    filled(e, "f_1p.pak", 5000, 1);
    TEXT(e, "character.txt", "replaces=7\nname=Inner replacement\n");
    d = sub(root, "B old files");
    filled(d, "b_1p.pak", 5000, 1);
    TEXT(d, "name.txt", "Old Name\nsecond line\n");
    TEXT(d, "form.txt", "");
    TEXT(d, "base.txt", "  55  \n");
    TEXT(d, "forms.txt", "left=A plain added one\nup=A plain added one/Form\npartner2=x\n");
    d = sub(root, "C old and new");
    filled(d, "c_1p.pak", 5000, 1);
    TEXT(d, "name.txt", "Old");
    TEXT(d, "base.txt", "9");
    TEXT(d, "character.txt", "name=New\nbase=10\n");
    d = sub(root, "D skin");
    filled(d, "skin_1p.pak", 9000, 1);
    TEXT(d, "character.txt", "replaces=0\n");
    d = sub(root, "E second for 0");
    filled(d, "skin_1p.pak", 9000, 1);
    TEXT(d, "character.txt", "replaces=0\n");
    d = sub(root, "F full replacement");
    for (i = 0; i < 4; i++) {
        char n[32];
        snprintf(n, sizeof(n), "f_%dp.pak", i + 1);
        filled(d, n, 4000, 1);
    }
    filled(d, "f_anm.pak", 4000, 1);
    filled(d, "f_eff.pak", 4000, 1);
    filled(d, "f_voice_jp.pak", 4000, 1);
    filled(d, "f_voice_us.pak", 4000, 1);
    filled(d, "portrait.bin", 4000, 1);
    filled(d, "member_1p.bin", 4096, 1);
    filled(d, "member_2p.bin", 4096, 1);
    chip_file(d, "chip.bin", 2, 8, 0x40, 0, CHIP_SIZE - 100);
    e = sub(d, "voice_jp");
    filled(e, "000.adx", 100, 2);
    filled(e, "099.adx", 100, 2);
    sub(e, "050.adx"); /* (a folder where a line would be) */
    e = sub(d, "VOICE_US");
    filled(e, "001.adx", 100, 2);
    TEXT(d, "character.txt", "replaces=160\nname=Last \xE3\x81\x82\nform=\nhealth=50000\nki=100000\nblast=7\nleft=A plain added one\n");
    d = sub(root, "G too large");
    filled(d, "g_1p.pak", 5000, 1);
    filled(d, "g_anm.pak", 0x160800 + 1, 1);
    filled(d, "g_eff.pak", 0xCE800 + 2048, 1);
    filled(d, "g_voice_us.pak", 0x90800 + 1, 1);
    filled(d, "portrait.bin", 0x16800 + 1, 1);
    filled(d, "member_1p.bin", 0x1001, 1);
    filled(d, "member_2p.bin", 0, 1);
    filled(d, "g_2p.pak", 3, 1);
    filled(d, "chip.bin", CHIP_SIZE, 0xFF);
    d = sub(root, "H model too large");
    filled(d, "h_1p.pak", PORT_MODEL_MAX + 1, 1);
    d = sub(root, "I replacement too large");
    filled(d, "i_1p.pak", PORT_MODEL_MAX + 1, 1);
    filled(d, "i_2p.pak", PORT_MODEL_MAX, 1);
    TEXT(d, "character.txt", "replaces=5\n");
    d = sub(root, "J bad chips");
    filled(d, "j_1p.pak", 5000, 1);
    chip_file(d, "chip.bin", 1, 0x7FFFFFFF, 0, 0, CHIP_SIZE);
    d = sub(root, "J2");
    filled(d, "j_1p.pak", 5000, 1);
    chip_file(d, "chip.bin", 0x40000000, 8, 0, 0, CHIP_SIZE);
    d = sub(root, "J3");
    filled(d, "j_1p.pak", 5000, 1);
    chip_file(d, "chip.bin", 1, 8, 0x3FFFFFFF, 0xFFFFFFFFu, CHIP_SIZE);
    d = sub(root, "J4");
    filled(d, "j_1p.pak", 5000, 1);
    chip_file(d, "chip.bin", 1, CHIP_SIZE / 4 - 4, 0, 0, CHIP_SIZE);
    d = sub(root, "J5");
    TEXT(d, "character.txt", "replaces=9\n");
    chip_file(d, "chip.bin", 1, 8, 0x20, 0x30, 10);
    d = sub(root, "K folders for files");
    sub(d, "k_1p.pak");
    sub(d, "chip.bin");
    sub(d, "portrait.bin");
    sub(d, "character.txt");
    sub(d, "name.txt");
    d = sub(root, "L manifest only");
    TEXT(d, "character.txt", "name=Nothing here\n");
    d = sub(root, "M replaces nothing");
    TEXT(d, "character.txt", "replaces=12\n");
    d = sub(root, "N bad replaces");
    filled(d, "n_1p.pak", 5000, 1);
    TEXT(d, "character.txt", "replaces=four\n");
    d = sub(root, "O binary manifest");
    filled(d, "o_1p.pak", 5000, 1);
    filled(d, "character.txt", 100000, 0xC3);
    d = sub(root, "P huge manifest");
    filled(d, "p_1p.pak", 5000, 1);
    filled(d, "character.txt", 3000000, '=');
    filled(d, "forms.txt", 3000000, 'a');
    filled(d, "name.txt", 3000000, 'n');
    filled(d, "base.txt", 3000000, '9');
    d = sub(root, "Q name only replacement");
    TEXT(d, "character.txt", "replaces=20\nname=Renamed\nform=Relabelled\n");
    d = sub(root, "R two of a kind");
    filled(d, "b_1p.pak", 5000, 1);
    filled(d, "a_1p.pak", 6000, 1);
    filled(d, "CHIP.BIN", 10, 1);
    d = sub(root, "S links to itself");
    filled(d, "s_1p.pak", 5000, 1);
    TEXT(d, "character.txt", "left=S links to itself\nup=..\nright=/\ndown=S links to itself/\nfusion1=disc:\nfusion2=disc:-1\npartner3=,,,\n");
    d = sub(root, "T not a package");
    TEXT(d, "readme.txt", "hello");
    memset(longName, 'L', sizeof(longName));
    longName[60] = '\0';
    d = sub(root, longName);
    longName[60] = 'L';
    memcpy(longName + 240, "_1p.pak", 8);
    filled(d, longName, 5000, 1);
    longName[62] = '\0';
    e = sub(d, longName);
    longName[62] = 'L';
    filled(e, longName, 5000, 1);
    {
        char link[1200];
        snprintf(link, sizeof(link), "%s/U loop", root);
        if (symlink("U loop", link) != 0) {
            perror("symlink");
        }
        snprintf(link, sizeof(link), "%s/V up", root);
        if (symlink("..", link) != 0) {
            perror("symlink");
        }
    }
    for (i = 0; i < 70; i++) { /* more than the game takes */
        char n[64];
        snprintf(n, sizeof(n), "Z many %02d", i);
        d = sub(root, n);
        filled(d, "m_1p.pak", 4000, 1);
        if (i % 10 == 0) {
            chip_file(d, "chip.bin", 1, 8, 0x20, 0x30, CHIP_SIZE);
        }
    }
    printf("made %s\n", root);
}

/* a text pack as a select's is: `lines` entries of 64 bytes */
static unsigned char *text_pack_made(unsigned lines, unsigned stated) {
    unsigned head = 4 * (lines + 2), i;
    unsigned char *p = calloc(1, head + lines * 64 + 64);
    uint32_t *w = (uint32_t *)p;
    w[0] = stated;
    for (i = 0; i <= lines; i++) {
        w[i + 1] = head + i * 64;
    }
    for (i = 0; i < lines; i++) {
        p[head + i * 64] = 0xFF;
        p[head + i * 64 + 1] = 0xFE;
        p[head + i * 64 + 2] = (unsigned char)('A' + i % 26);
    }
    return p;
}

static unsigned ask_all(void) {
    char rel[64], out[512];
    unsigned answered = 0;
    int i, n;
    for (i = 0; i < 3400; i++) {
        snprintf(rel, sizeof(rel), "pzs3us1/%05d.bin", i);
        answered += PortCharas_Alias(rel, out, sizeof(out)) && strncmp(out, "pzs3us", 6) != 0; /* (a file of a folder) */
    }
    for (i = 30000; i < 96000; i++) {
        snprintf(rel, sizeof(rel), "pzs3us2/%05d.bin", i);
        answered += PortCharas_Alias(rel, out, sizeof(out)) && strncmp(out, "pzs3us", 6) != 0; /* (a file of a folder) */
    }
    for (n = 1; n < 40; n += 7) { /* (answers into small buffers) */
        PortCharas_Alias("pzs3us1/01423.bin", out, (unsigned)n);
        PortCharas_Alias("pzs3us2/71000.bin", out, (unsigned)n);
        PortCharas_Alias("pzs3us2/74000.bin", out, (unsigned)n);
        PortCharas_Alias("pzs3us2/52274.bin", out, (unsigned)n);
    }
    PortCharas_Alias("pzs3us1/", out, sizeof(out));
    PortCharas_Alias("pzs3us2/-5.bin", out, sizeof(out));
    PortCharas_Alias("pzs3us2/99999999999999999999.bin", out, sizeof(out));
    PortCharas_Alias("pzs3us9/00001.bin", out, sizeof(out));
    PortCharas_Alias("disc/DATA/X.PSS", out, sizeof(out));
    PortCharas_Alias("", out, sizeof(out));
    return answered;
}

static void scan_all(const char *root) {
    int i, k, forms[64], session;
    unsigned char *names, *chips;
    unsigned own = 0, online = 0;
    sRoot = root;
    PortCharas_Init();
    for (i = 0; i < Port_CharaPkgCount(); i++) {
        const PortCharaPkg *p = Port_CharaPkg(i);
        printf("%-9s %-4d [%s] name [%s] by [%s] [%s] about [%s]%s%s\n", p->kind == PORT_PKG_ADDS ? "adds" : p->kind == PORT_PKG_REPLACES ? "replaces" : "not used",
               p->id, p->folder, p->name, p->author, p->version, p->about, p->problem[0] != '\0' ? "\n          ! " : "", p->problem);
    }
    printf("model buffers: %d\n", Port_ModelSize());
    for (session = 0; session < 2; session++) {
        unsigned answered, lines[5] = {0, 1, 165, 168, 256};
        sSession = session;
        answered = ask_all();
        *(session ? &online : &own) = answered;
        for (i = -3; i < 0x140; i++) {
            Port_CharaBase(i);
            Port_CharaCostumes(i);
            Port_CharaForms(i, forms, 64);
            Port_CharaForms(i, forms, 1);
            Port_CharaNameLine(i);
            Port_CharaFormLine(i);
            PortChips_Get(i);
            Port_CharaChip(i);
            for (k = -1; k < 21; k++) {
                int t = Port_CharaFormTarget(i, k);
                if (session && t != -1) {
                    printf("FAIL: a link answered in a session\n");
                    sFail++;
                }
            }
            for (k = -1; k < 4; k++) {
                if (Port_CharaStat(i, k, 12345) != 12345 && session) {
                    printf("FAIL: a number answered in a session\n");
                    sFail++;
                }
            }
        }
        for (i = -2; i < PORT_CHARA_MAX + 2; i++) {
            Port_CharaChipLoad(i);
            Port_CharaName(i);
        }
        for (k = 0; k < 5; k++) { /* text packs of several sizes, and one that states more lines than it has */
            void *got;
            names = text_pack_made(lines[k] > 0 ? lines[k] : 1, lines[k]);
            got = Port_CharaNamePack(names);
            if (session && got != names) {
                printf("FAIL: a text pack changed in a session\n");
                sFail++;
            }
            Port_CharaFormPack(names);
            free(names);
        }
        names = text_pack_made(4, 400);
        Port_CharaNamePack(names);
        free(names);
        /* chip packs: the disc's shape (165 entries of 5504 bytes), a short one, one with small entries */
        for (k = 0; k < 3; k++) {
            unsigned count = k == 1 ? 3 : 165, size = k == 2 ? 64 : CHIP_SIZE, head = (4 * (count + 2) + 63) & ~63u, j;
            uint32_t *w;
            unsigned char *copy;
            chips = calloc(1, head + count * size);
            copy = malloc(head + count * size);
            w = (uint32_t *)chips;
            w[0] = count;
            for (j = 0; j <= count; j++) {
                w[j + 1] = head + j * size;
            }
            memcpy(copy, chips, head + count * size);
            PortChips_Load(chips);
            if (session && memcmp(copy, chips, head + count * size) != 0) {
                printf("FAIL: a chip pack changed in a session\n");
                sFail++;
            }
            PortChips_Free();
            free(chips);
            free(copy);
        }
        PortChips_Load(NULL);
        PortChips_Free();
    }
    printf("file numbers answered from a folder: %u in the player's own game, %u in a session\n", own, online);
    if (online != 0) {
        printf("FAIL: a file answered in a session\n");
        sFail++;
    }
    printf(sFail == 0 ? "scan OK\n" : "scan FAILED\n");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--cases") == 0) {
        cases();
    } else if (argc == 4 && strcmp(argv[1], "--fuzz") == 0) {
        fuzz(atoi(argv[2]), strtoull(argv[3], NULL, 0));
    } else if (argc == 3 && strcmp(argv[1], "--make") == 0) {
        make(argv[2]);
    } else if (argc == 3 && strcmp(argv[1], "--scan") == 0) {
        scan_all(argv[2]);
    } else {
        fprintf(stderr, "packages_test --cases | --fuzz <rounds> <seed> | --make <folder> | --scan <folder>\n");
        return 2;
    }
    return sFail != 0;
}
