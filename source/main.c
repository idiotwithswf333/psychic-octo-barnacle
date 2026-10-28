// Chaotix-style proof of concept for DS / DSi (libnds, devkitARM)
// Original code + placeholder art. Tethered duo, rings, scrolling stage.
#include <nds.h>
#include <fat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef REG_SCFG_CLK
#define REG_SCFG_CLK (*(vu16*)0x4004004)
#endif

#define SW 256
#define SH 192
#define FIX 8
#define WORLD_W 2400
#define GOAL_X 2330
#define RGB(r,g,b) (RGB15(r,g,b) | BIT(15))

#define GRAV 56
#define MAXFALL 1024
#define ACC 40
#define MAXV 640
#define MAXV_HARD 1280
#define JUMPV (-1150)

typedef struct { int x, y, vx, vy, w, h; bool ground, wall; int face; u16 col; } Actor;
typedef struct { int x, y, w, h; } Rect;

static const Rect plat[] = {
    {0,160,420,32}, {480,160,300,32}, {840,160,200,32}, {1100,140,160,52},
    {1320,160,420,32}, {1800,160,300,32}, {2160,160,240,32},
    {200,112,80,10}, {600,112,90,10}, {900,104,70,10}, {1400,112,80,10},
    {1560,112,100,12}, {1960,112,80,10}
};
#define NPLAT ((int)(sizeof(plat)/sizeof(plat[0])))

#define NRING 37
static int ringX[NRING], ringY[NRING];
static bool ringGot[NRING];

static u16 *fb[2];
static u16 *buf;

// ---------- drawing ----------
static inline void putpx(int x, int y, u16 c) {
    if ((unsigned)x < SW && (unsigned)y < SH) buf[y * SW + x] = c;
}
static void rect(int x, int y, int w, int h, u16 c) {
    int x1 = x + w, y1 = y + h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x1 > SW) x1 = SW;
    if (y1 > SH) y1 = SH;
    for (int j = y; j < y1; j++) {
        u16 *p = buf + j * SW;
        for (int i = x; i < x1; i++) p[i] = c;
    }
}
static void line(int x0, int y0, int x1, int y1, u16 c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        putpx(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

// ---------- physics ----------
static bool hit(int x, int y, int w, int h, const Rect *r) {
    return x < r->x + r->w && x + w > r->x && y < r->y + r->h && y + h > r->y;
}
static void moveActor(Actor *a) {
    a->wall = false; a->ground = false;
    a->x += a->vx;
    if (a->x < 0) { a->x = 0; a->vx = 0; }
    int px = a->x >> FIX, py = a->y >> FIX;
    for (int i = 0; i < NPLAT; i++) {
        if (a->vx != 0 && hit(px, py, a->w, a->h, &plat[i])) {
            px = (a->vx > 0) ? plat[i].x - a->w : plat[i].x + plat[i].w;
            a->x = px << FIX; a->vx = 0; a->wall = true;
        }
    }
    a->y += a->vy;
    py = a->y >> FIX;
    for (int i = 0; i < NPLAT; i++) {
        if (hit(px, py, a->w, a->h, &plat[i])) {
            if (a->vy > 0) { py = plat[i].y - a->h; a->ground = true; }
            else if (a->vy < 0) { py = plat[i].y + plat[i].h; }
            a->y = py << FIX; a->vy = 0;
        }
    }
}
static void clampSpeed(Actor *a) {
    if (a->vx > MAXV_HARD) a->vx = MAXV_HARD;
    if (a->vx < -MAXV_HARD) a->vx = -MAXV_HARD;
}

// elastic ring: pulls both toward each other once past its rest length
static void tether(Actor *l, Actor *f) {
    int dx = f->x - l->x, dy = f->y - l->y;
    int rx = 44 << FIX, ry = 30 << FIX;
    int ex = dx > rx ? dx - rx : (dx < -rx ? dx + rx : 0);
    int ey = dy > ry ? dy - ry : (dy < -ry ? dy + ry : 0);
    l->vx += ex / 40; f->vx -= ex / 14;
    l->vy += ey / 60; f->vy -= ey / 24;
    if (ex == 0 && f->ground) f->vx -= f->vx / 8;
    // follower hops obstacles / climbs toward the leader
    if (f->ground && (f->wall || (l->y < f->y - (20 << FIX) && abs(dx) < (50 << FIX)))) f->vy = JUMPV;
}

// ---------- state ----------
static Actor kn, es;
static int ctl, score, cam, frame, clearTimer;
static char romName[64];
static long romSize;

static void resetStage(void) {
    memset(&kn, 0, sizeof kn); memset(&es, 0, sizeof es);
    kn.x = 40 << FIX; es.x = 10 << FIX;
    kn.y = es.y = 140 << FIX;
    kn.w = es.w = 12; kn.h = es.h = 16;
    kn.col = RGB(28, 4, 4); es.col = RGB(20, 6, 26);
    kn.face = es.face = 1;
    memset(ringGot, 0, sizeof ringGot);
    score = 0; clearTimer = 0;
}
static void initRings(void) {
    for (int i = 0; i < NRING; i++) {
        int x = 100 + i * 60, y = 96;
        for (int p = 0; p < NPLAT; p++)
            if (x >= plat[p].x && x < plat[p].x + plat[p].w && plat[p].y - 24 < y + 200 &&
                (y == 96 || plat[p].y - 24 < y)) y = plat[p].y - 24;
        ringX[i] = x; ringY[i] = y;
    }
}
static void scanRom(void) {
    romName[0] = 0; romSize = -1;
    mkdir("/32xKC", 0777);                 // make the folder if it isn't there
    DIR *d = opendir("/32xKC");
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *dot = strrchr(e->d_name, '.');
        if (dot && (!strcasecmp(dot, ".32x") || !strcasecmp(dot, ".bin") || !strcasecmp(dot, ".md"))) {
            char path[300]; struct stat st;
            snprintf(path, sizeof path, "/32xKC/%s", e->d_name);
            strncpy(romName, e->d_name, sizeof romName - 1);
            romSize = (stat(path, &st) == 0) ? (long)st.st_size : 0;
            break;
        }
    }
    closedir(d);
}
static void hud(void) {
    iprintf("\x1b[2J\x1b[0;0H");
    iprintf("KNUCKLES' CHAOTIX\n DSi proof of concept\n\n");
    if (romSize >= 0) iprintf("32xKC: %.22s\n       %ld KB\n", romName, romSize / 1024);
    else iprintf("32xKC: (no ROM in folder)\n\n");
    iprintf("\nRings: %2d / %d\n", score, NRING);
    iprintf("Leading: %s\n\n", ctl ? "Espio  " : "Knuckles");
    iprintf("D-Pad  move\nA      jump\nX      swap leader\nSTART  restart\n");
    if (clearTimer) iprintf("\n*** STAGE CLEAR! ***\n");
}

// ---------- render ----------
static void drawActor(const Actor *a, int camx) {
    int x = (a->x >> FIX) - camx, y = a->y >> FIX;
    rect(x, y, a->w, a->h, a->col);
    rect(x + 1, y + 1, a->w - 2, 5, RGB(31, 24, 16));            // face
    rect(x + (a->face > 0 ? 7 : 2), y + 2, 3, 2, RGB(31, 31, 31)); // eye
    rect(x + (a->face > 0 ? 9 : -1), y + 9, 4, 4, RGB(31, 31, 31)); // fist
}
static void render(void) {
    // sky
    for (int y = 0; y < SH; y++) {
        u16 c = RGB(4 + y / 14, 12 + y / 10, 31);
        u16 *p = buf + y * SW;
        for (int x = 0; x < SW; x++) p[x] = c;
    }
    // parallax hills
    for (int x = 0; x < SW; x++) {
        int t = (x + (cam >> 2)) % 96; if (t > 48) t = 96 - t;
        rect(x, 130 - t, 1, SH - (130 - t), RGB(6, 20, 14));
    }
    // platforms
    for (int i = 0; i < NPLAT; i++) {
        int x = plat[i].x - cam;
        if (x > SW || x + plat[i].w < 0) continue;
        rect(x, plat[i].y, plat[i].w, plat[i].h, RGB(18, 11, 5));
        for (int cx = 0; cx < plat[i].w; cx += 16)
            for (int cy = 4; cy < plat[i].h; cy += 16)
                rect(x + cx, plat[i].y + cy, 8, 8, RGB(14, 8, 3));
        rect(x, plat[i].y, plat[i].w, 4, RGB(6, 26, 6));
    }
    // goal post
    rect(GOAL_X - cam, 112, 3, 48, RGB(28, 28, 28));
    rect(GOAL_X + 3 - cam, 112, 14, 10, RGB(31, 6, 6));
    // rings
    int fr = (frame / 6) & 3, rw = (int[]){8, 6, 3, 6}[fr];
    for (int i = 0; i < NRING; i++) {
        if (ringGot[i]) continue;
        int x = ringX[i] - cam, y = ringY[i] - 4;
        if (x < -10 || x > SW + 10) continue;
        u16 g = RGB(31, 25, 2);
        int ox = x - rw / 2;
        rect(ox, y, rw, 2, g); rect(ox, y + 6, rw, 2, g);
        rect(ox, y, 2, 8, g); rect(ox + rw - 2, y, 2, 8, g);
    }
    // tether (the ring-band) + bobbing ring in the middle
    int ax = (kn.x >> FIX) - cam + 6, ay = (kn.y >> FIX) + 8;
    int bx = (es.x >> FIX) - cam + 6, by = (es.y >> FIX) + 8;
    line(ax, ay, bx, by, RGB(31, 25, 2));
    line(ax, ay + 1, bx, by + 1, RGB(24, 18, 0));
    int mx = (ax + bx) / 2, my = (ay + by) / 2;
    rect(mx - 3, my - 3, 7, 7, RGB(31, 25, 2)); rect(mx - 1, my - 1, 3, 3, RGB(24, 18, 0));
    // characters (leader drawn last)
    if (ctl) { drawActor(&kn, cam); drawActor(&es, cam); }
    else     { drawActor(&es, cam); drawActor(&kn, cam); }
    // progress bar
    rect(8, 4, 240, 3, RGB(8, 8, 8));
    rect(8, 4, (((ctl ? es.x : kn.x) >> FIX) * 240) / WORLD_W, 3, RGB(31, 25, 2));
}

int main(void) {
    if (isDSiMode()) REG_SCFG_CLK |= 1;          // DSi: 133 MHz ARM9

    videoSetMode(MODE_FB0);
    vramSetBankA(VRAM_A_LCD);
    vramSetBankB(VRAM_B_LCD);
    fb[0] = (u16 *)VRAM_A; fb[1] = (u16 *)VRAM_B;
    consoleDemoInit();                            // bottom screen text

    fatInitDefault();
    scanRom();
    initRings();
    resetStage();
    hud();

    int back = 1, lastScore = -1;
    while (1) {
        scanKeys();
        u32 down = keysDown(), held = keysHeld();
        if (down & KEY_START) { resetStage(); hud(); }
        if (down & KEY_X) { ctl ^= 1; hud(); }

        Actor *l = ctl ? &es : &kn, *f = ctl ? &kn : &es;

        if (!clearTimer) {
            if (held & KEY_RIGHT)     { l->vx += ACC; if (l->vx > MAXV) l->vx = MAXV; l->face = 1; }
            else if (held & KEY_LEFT) { l->vx -= ACC; if (l->vx < -MAXV) l->vx = -MAXV; l->face = -1; }
            else if (l->ground)       { l->vx -= l->vx / 6; if (abs(l->vx) < 16) l->vx = 0; }
            if ((down & KEY_A) && l->ground) l->vy = JUMPV;
            if (!(held & KEY_A) && l->vy < -400) l->vy = -400;
        }
        f->face = (l->x > f->x) ? 1 : -1;

        kn.vy += GRAV; es.vy += GRAV;
        if (kn.vy > MAXFALL) kn.vy = MAXFALL;
        if (es.vy > MAXFALL) es.vy = MAXFALL;
        tether(l, f);
        clampSpeed(&kn); clampSpeed(&es);
        moveActor(&kn); moveActor(&es);

        // fell in a pit
        if ((kn.y >> FIX) > 260 || (es.y >> FIX) > 260) {
            int keep = score; resetStage(); score = keep; hud();
        }
        // rings
        for (int i = 0; i < NRING; i++) {
            if (ringGot[i]) continue;
            Rect r = { ringX[i] - 4, ringY[i] - 4, 8, 8 };
            if (hit(kn.x >> FIX, kn.y >> FIX, kn.w, kn.h, &r) ||
                hit(es.x >> FIX, es.y >> FIX, es.w, es.h, &r)) { ringGot[i] = true; score++; }
        }
        // goal
        if (!clearTimer && (l->x >> FIX) > GOAL_X) { clearTimer = 180; hud(); }
        if (clearTimer && --clearTimer == 0) { resetStage(); hud(); }
        if (score != lastScore) { lastScore = score; hud(); }

        // camera follows the leader
        int target = (l->x >> FIX) - 120;
        if (target < 0) target = 0;
        if (target > WORLD_W - SW) target = WORLD_W - SW;
        cam += (target - cam) / 6;
        frame++;

        buf = fb[back];
        render();
        swiWaitForVBlank();
        videoSetMode(back ? MODE_FB1 : MODE_FB0);
        back ^= 1;
    }
    return 0;
}
