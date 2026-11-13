/*
 * ttxclient.cpp  -  Teletext viewer client for Windows (Win32 GUI)
 * ----------------------------------------------------------------
 * Connects to ttxserver and displays Teletext pages: 8 colours,
 * contiguous/separated mosaic graphics, hold graphics, double height,
 * flash, conceal/reveal, Fastext links and subpage carousels.
 *
 * Usage:  ttxclient [host] [port]      (default 127.0.0.1 8023)
 *   or put settings in ttxclient.cfg next to the .exe:
 *        host=192.168.1.20
 *        port=8023
 *        page=100
 *        scaling=smooth      (or sharp for hard-edged pixels)
 *
 * Keys:  0-9 page number    Up/Down next/prev page   Left/Right subpage
 *        F1-F4 Fastext      F5 or I index            R reveal   H hold
 *        Esc cancel entry / dismiss newsflash     F6 go to newsflash story
 *        Mouse: click a page number, a Fastext link or the newsflash strip
 *
 * Dev-C++: Windows Application.
 *   Project > Project Options > Parameters > Linker:   -lws2_32 -lgdi32
 */

#include <winsock2.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>

#define WM_SOCKET    (WM_APP + 1)
#define TIMER_FLASH  1
#define TIMER_RETRY  2
#define TIMER_TICKER 3

#define ROWS      25
#define COLS      40
#define CW        12                 /* cell width in pixels  (as SAA5050) */
#define CH        20                 /* cell height in pixels (as SAA5050) */
#define SCREEN_W  (COLS * CW)        /* 480 */
#define SCREEN_H  (ROWS * CH)        /* 500 */

#include "saa5050.h"                 /* g_saa5050[96][10] character ROM */

/* ------------------------------------------------------------------ */
/*  State                                                              */
/* ------------------------------------------------------------------ */

static unsigned char g_screen[ROWS][COLS];
static int   g_pageNum   = 0x100;
static int   g_subNum    = 0, g_subCount = 0;
static int   g_links[5]  = { 0, 0, 0, 0, 0x100 };   /* R G Y C index */
static char  g_entry[4];
static int   g_entryLen  = 0;
static bool  g_reveal    = false;
static bool  g_hold      = false;
static bool  g_flashOn   = true;
static bool  g_waiting   = false;     /* a GET is outstanding */

/* newsflash strip across the bottom row (sent by the server) */
static std::string g_flashText;       /* headline, "" = none           */
static int         g_flashPage = 0;   /* story page                    */
static std::string g_flashDismissed;  /* headline the viewer closed    */
static int         g_flashPos  = 0;   /* ticker scroll position        */

static bool FlashShowing() { return !g_flashText.empty() && g_flashText != g_flashDismissed; }

static SOCKET      g_sock = INVALID_SOCKET;
static bool        g_connected = false;
static std::string g_rx;

static char  g_host[128]     = "127.0.0.1";
static int   g_port          = 8023;
static bool  g_smooth        = true;    /* scaling=smooth|sharp in cfg */

static HWND         g_hwnd;
static unsigned int g_pix[SCREEN_H][SCREEN_W];   /* the screen, 0x00RRGGBB */
static unsigned short g_glyph[96][20];           /* rounded 12x20 glyphs   */

static const unsigned int g_pal[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};

/*
 * Build 12x20 glyphs from the 6x10 ROM, the way the SAA5050 does:
 * every ROM dot becomes 2x2 pixels, then "character rounding" adds a
 * half-dot wherever two dots touch only diagonally, smoothing the steps.
 * Bit 11 of g_glyph[][] is the leftmost pixel.
 */
static void BuildGlyphs()
{
    for (int c = 0; c < 96; c++) {
        const unsigned char *rom = g_saa5050[c];
        unsigned short *g = g_glyph[c];
        memset(g, 0, 20 * sizeof(unsigned short));
        for (int r = 0; r < 10; r++)
            for (int x = 0; x < 6; x++)
                if (rom[r] & (0x20 >> x)) {
                    unsigned short two = (unsigned short)(0xC00 >> (2 * x));
                    g[2 * r] |= two;
                    g[2 * r + 1] |= two;
                }
        for (int r = 0; r < 9; r++) {
            for (int x = 0; x < 5; x++) {
                bool a0 = (rom[r]     & (0x20 >> x))       != 0;
                bool a1 = (rom[r]     & (0x20 >> (x + 1))) != 0;
                bool b0 = (rom[r + 1] & (0x20 >> x))       != 0;
                bool b1 = (rom[r + 1] & (0x20 >> (x + 1))) != 0;
                unsigned short pL = (unsigned short)(0x800 >> (2 * x + 1)); /* right half of x  */
                unsigned short pR = (unsigned short)(0x800 >> (2 * x + 2)); /* left half of x+1 */
                if (a0 && b1 && !a1 && !b0) {        /* diagonal  \  */
                    g[2 * r + 1] |= pR;
                    g[2 * r + 2] |= pL;
                }
                if (a1 && b0 && !a0 && !b1) {        /* diagonal  /  */
                    g[2 * r + 1] |= pL;
                    g[2 * r + 2] |= pR;
                }
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                            */
/* ------------------------------------------------------------------ */

static void Redraw() { InvalidateRect(g_hwnd, NULL, FALSE); }

static void UpdateTitle()
{
    char t[160];
    if (g_subCount > 1)
        sprintf(t, "Teletext - P%03X  %d/%d%s - %s:%d%s", g_pageNum, g_subNum, g_subCount,
                g_hold ? "  HOLD" : "", g_host, g_port, g_connected ? "" : " (offline)");
    else
        sprintf(t, "Teletext - P%03X%s - %s:%d%s", g_pageNum,
                g_hold ? "  HOLD" : "", g_host, g_port, g_connected ? "" : " (offline)");
    SetWindowTextA(g_hwnd, t);
}

static void PutText(int row, int col, const char *s)
{
    while (*s && col < COLS) g_screen[row][col++] = (unsigned char)(*s++ & 0x7F);
}

static void ClearBody()
{
    for (int r = 1; r < ROWS; r++) memset(g_screen[r], ' ', COLS);
}

static void ShowOffline(const char *why)
{
    memset(g_screen, ' ', sizeof(g_screen));
    char buf[64];
    PutText(1, 0, "\x04\x1D\x0D\x03" " TELETEXT CLIENT");
    PutText(5, 0, "\x01");
    PutText(5, 1, why);
    sprintf(buf, "\x07" "Server: %s:%d", g_host, g_port);
    PutText(7, 0, buf);
    PutText(9, 0, "\x06" "Retrying every 5 seconds...");
    PutText(12, 0, "\x07" "Start ttxserver.exe, or set host= and");
    PutText(13, 0, "\x07" "port= in ttxclient.cfg");
    g_subCount = 0;
    UpdateTitle();
    Redraw();
}

/* ------------------------------------------------------------------ */
/*  Networking                                                         */
/* ------------------------------------------------------------------ */

static void NetClose()
{
    if (g_sock != INVALID_SOCKET) {
        WSAAsyncSelect(g_sock, g_hwnd, 0, 0);
        closesocket(g_sock);
    }
    g_sock = INVALID_SOCKET;
    g_connected = false;
    g_rx.clear();
}

static void NetRetryLater(const char *why)
{
    NetClose();
    ShowOffline(why);
    SetTimer(g_hwnd, TIMER_RETRY, 5000, NULL);
}

static void NetConnect()
{
    NetClose();
    g_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_sock == INVALID_SOCKET) { NetRetryLater("Cannot create socket"); return; }

    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port   = htons((u_short)g_port);

    unsigned long ip = inet_addr(g_host);
    if (ip == INADDR_NONE) {
        hostent *he = gethostbyname(g_host);
        if (!he) { NetRetryLater("Cannot find server name"); return; }
        memcpy(&ip, he->h_addr_list[0], 4);
    }
    a.sin_addr.s_addr = ip;

    /* async: Windows posts WM_SOCKET to our window for connect/read/close */
    WSAAsyncSelect(g_sock, g_hwnd, WM_SOCKET, FD_CONNECT | FD_READ | FD_CLOSE);
    connect(g_sock, (sockaddr *)&a, sizeof(a));
}

static void NetSend(const char *s)
{
    if (g_connected) send(g_sock, s, (int)strlen(s), 0);
}

static void RequestPage(int page, int sub)
{
    if (page < 0x100 || page > 0x8FF) return;
    if (page != g_pageNum) g_reveal = false;
    g_pageNum = page;
    g_waiting = true;
    char buf[32];
    if (sub > 0) sprintf(buf, "GET %03X %d\n", page, sub);
    else         sprintf(buf, "GET %03X\n", page);
    NetSend(buf);
    UpdateTitle();
    Redraw();
}

/* Handle everything we've received so far. Frames may arrive in pieces,
 * so we only consume a frame once all of it is in the buffer. */
static void ProcessRx()
{
    for (;;) {
        size_t nl = g_rx.find('\n');
        if (nl == std::string::npos) {
            if (g_rx.size() > 8192) g_rx.clear();
            return;
        }
        std::string line = g_rx.substr(0, nl);
        const unsigned char *data = (const unsigned char *)g_rx.data() + nl + 1;
        size_t avail = g_rx.size() - nl - 1;

        if (line.compare(0, 5, "PAGE ") == 0) {
            if (avail < ROWS * COLS) return;              /* wait for the rest */
            int pn = 0, sub = 0, cnt = 0, l[5] = { 0, 0, 0, 0, 0 };
            sscanf(line.c_str() + 5, "%x %d %d %x %x %x %x %x",
                   &pn, &sub, &cnt, &l[0], &l[1], &l[2], &l[3], &l[4]);
            /* accept if it's our page, and we asked for it or we're not holding */
            if (pn == g_pageNum && (g_waiting || !g_hold)) {
                memcpy(g_screen, data, ROWS * COLS);
                g_subNum = sub;
                g_subCount = cnt;
                for (int i = 0; i < 5; i++) g_links[i] = l[i];
                g_waiting = false;
                UpdateTitle();
            }
            g_rx.erase(0, nl + 1 + ROWS * COLS);
        }
        else if (line == "HDR") {
            if (avail < COLS) return;
            memcpy(g_screen[0] + 8, data + 8, COLS - 8);  /* clock keeps running */
            g_rx.erase(0, nl + 1 + COLS);
        }
        else if (line.compare(0, 5, "FLASH") == 0) {
            std::string old = g_flashText;
            int pg = 0;
            char text[512] = "";
            if (sscanf(line.c_str() + 5, " %x %511[^\n]", &pg, text) == 2) {
                g_flashPage = pg;
                g_flashText = text;
            } else {
                g_flashText.clear();
            }
            if (g_flashText != old) {
                g_flashPos = 0;
                if (FlashShowing()) MessageBeep(MB_ICONEXCLAMATION);
            }
            g_rx.erase(0, nl + 1);
        }
        else if (line.compare(0, 7, "NOPAGE ") == 0) {
            int pn = 0;
            sscanf(line.c_str() + 7, "%x", &pn);
            if (pn == g_pageNum) {
                char buf[48];
                ClearBody();
                sprintf(buf, "\x01" "Page %03X is not available", pn);
                PutText(10, 0, buf);
                PutText(12, 0, "\x07" "Press F5 for the index");
                g_subCount = 0;
                g_waiting = false;
                UpdateTitle();
            }
            g_rx.erase(0, nl + 1);
        }
        else {
            g_rx.erase(0, nl + 1);                          /* unknown - skip */
        }
        Redraw();
    }
}

/* ------------------------------------------------------------------ */
/*  Rendering                                                          */
/* ------------------------------------------------------------------ */

static void FillCell(int x, int y, int w, int h, int colour)
{
    unsigned int c = g_pal[colour & 7];
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++)
        for (int xx = x; xx < x + w && xx < SCREEN_W; xx++)
            g_pix[yy][xx] = c;
}

/* Mosaic character: 2 x 3 blocks.  Bits: 0 1 / 2 3 / 4 6
 * Block heights are 6/8/6 lines (12/16/12 in double height) as on the SAA5050.
 * Separated graphics lose 2 pixels on the left and the bottom of each block. */
static void DrawMosaic(int x, int y, int h, unsigned char c, int fg, bool sep)
{
    static const unsigned char bit[3][2] = { {0x01, 0x02}, {0x04, 0x08}, {0x10, 0x40} };
    int rowH[3];
    rowH[0] = h * 3 / 10;
    rowH[1] = h * 4 / 10;
    rowH[2] = h - rowH[0] - rowH[1];
    int gx = 2, gy = h / 10;

    int yy = y;
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 2; k++) {
            if (c & bit[r][k]) {
                int xx = x + k * (CW / 2);
                if (sep) FillCell(xx + gx, yy, CW / 2 - gx, rowH[r] - gy, fg);
                else     FillCell(xx, yy, CW / 2, rowH[r], fg);
            }
        }
        yy += rowH[r];
    }
}

/* Character from the SAA5050 ROM. Double height repeats each line twice. */
static void DrawChar(int x, int y, int h, unsigned char c, int fg, bool dbl)
{
    if (c < 0x20 || c > 0x7F) return;
    const unsigned short *g = g_glyph[c - 0x20];
    unsigned int colour = g_pal[fg & 7];
    for (int line = 0; line < h; line++) {
        int yy = y + line;
        if (yy >= SCREEN_H) break;
        unsigned short bits = g[dbl ? line / 2 : line];
        if (!bits) continue;
        for (int px = 0; px < CW; px++)
            if (bits & (0x800 >> px)) g_pix[yy][x + px] = colour;
    }
}

/* Newsflash strip for the bottom row: flashing NEWSFLASH on red,
 * then the headline scrolling past in yellow on blue. */
static void OverlayFlash(unsigned char *row)
{
    static const unsigned char head[17] = {
        0x01, 0x1D, 0x07, 0x08, 'N','E','W','S','F','L','A','S','H', 0x09, 0x04, 0x1D, 0x03 };
    memcpy(row, head, sizeof(head));
    std::string loop = g_flashText + "   +++   ";
    int w = COLS - (int)sizeof(head);
    for (int i = 0; i < w; i++)
        row[sizeof(head) + i] = (unsigned char)(loop[(g_flashPos + i) % loop.size()] & 0x7F);
}

/* Fill in columns 0-7 of the header: page number being entered / shown */
static void OverlayHeader(unsigned char *row)
{
    char tmp[9];
    if (g_entryLen > 0)
        sprintf(tmp, " %c%c%c    ", g_entry[0],
                g_entryLen > 1 ? g_entry[1] : '-', '-');
    else if (g_hold)
        strcpy(tmp, " HOLD   ");
    else
        sprintf(tmp, " %03X    ", g_pageNum);
    memcpy(row, tmp, 8);
}

/*
 * Draw the whole page into the off-screen bitmap, decoding the
 * Teletext "spacing attributes" left to right along each row.
 * "Set-at" codes affect the cell they're in; "set-after" codes
 * take effect from the next cell.
 */
static void RenderScreen()
{
    memset(g_pix, 0, sizeof(g_pix));

    bool skipRow = false;
    for (int row = 0; row < ROWS; row++) {
        if (skipRow) { skipRow = false; continue; }   /* lower half of double height */

        unsigned char rb[COLS];
        memcpy(rb, g_screen[row], COLS);
        if (row == 0) OverlayHeader(rb);
        if (row == ROWS - 1 && FlashShowing()) OverlayFlash(rb);

        bool rowHasDbl = false;
        if (row > 0 && row < ROWS - 1)
            for (int c = 0; c < COLS; c++)
                if ((rb[c] & 0x7F) == 0x0D) rowHasDbl = true;

        int  fg = 7, bg = 0;
        bool gfx = false, sep = false, hold = false;
        bool flash = false, conceal = false, dbl = false;
        unsigned char held = 0x20;
        bool heldSep = false;
        int  y = row * CH;

        for (int col = 0; col < COLS; col++) {
            unsigned char c = rb[col] & 0x7F;
            int x = col * CW;

            /* ---- set-at attributes ---- */
            switch (c) {
            case 0x09: flash = false;                 break;  /* steady       */
            case 0x0C: if (dbl) held = 0x20; dbl = false; break; /* normal size */
            case 0x18: conceal = true;                break;  /* conceal      */
            case 0x19: sep = false;                   break;  /* contiguous   */
            case 0x1A: sep = true;                    break;  /* separated    */
            case 0x1C: bg = 0;                        break;  /* black bg     */
            case 0x1D: bg = fg;                       break;  /* new bg       */
            case 0x1E: hold = true;                   break;  /* hold gfx     */
            }

            /* ---- what goes in this cell ---- */
            unsigned char show = 0x20;
            bool showSep = sep, mosaic = false;
            if (c < 0x20) {
                if (hold && gfx) { show = held; showSep = heldSep; mosaic = true; }
            } else {
                show = c;
                if (gfx && (c & 0x20)) {          /* 0x40-0x5F stay as letters */
                    mosaic = true;
                    held = c;
                    heldSep = sep;
                }
            }

            FillCell(x, y, CW, rowHasDbl ? 2 * CH : CH, bg);
            bool visible = !(flash && !g_flashOn) && !(conceal && !g_reveal);
            if (visible && show != 0x20) {
                int h = dbl ? 2 * CH : CH;
                if (mosaic) DrawMosaic(x, y, h, show, fg, showSep);
                else        DrawChar(x, y, h, show, fg, dbl);
            }

            /* ---- set-after attributes ---- */
            if (c <= 0x07)                   { fg = c; gfx = false; conceal = false; held = 0x20; }
            else if (c >= 0x10 && c <= 0x17) { fg = c - 0x10; gfx = true; conceal = false; }
            else if (c == 0x08)              { flash = true; }
            else if (c == 0x0D && rowHasDbl) { dbl = true; held = 0x20; }
            else if (c == 0x1F)              { hold = false; }
        }
        if (rowHasDbl) skipRow = true;
    }
}

/* ------------------------------------------------------------------ */
/*  Navigation                                                         */
/* ------------------------------------------------------------------ */

static void StepPage(int delta)
{
    int m = (g_pageNum >> 8) & 0xF, t = (g_pageNum >> 4) & 0xF, u = g_pageNum & 0xF;
    if (t > 9) t = 9;
    if (u > 9) u = 9;
    int d = m * 100 + t * 10 + u + delta;
    if (d > 899) d = 100;
    if (d < 100) d = 899;
    g_hold = false;
    RequestPage(((d / 100) << 8) | (((d / 10) % 10) << 4) | (d % 10), 0);
}

static void StepSubpage(int delta)
{
    if (g_subCount < 2) return;
    int s = g_subNum + delta;
    if (s > g_subCount) s = 1;
    if (s < 1) s = g_subCount;
    RequestPage(g_pageNum, s);
}

static void GoLink(int i)
{
    if (i == 4 && !g_links[4]) { g_hold = false; RequestPage(0x100, 0); return; }
    if (g_links[i]) { g_hold = false; RequestPage(g_links[i], 0); }
}

static void DigitKey(char d)
{
    if (g_entryLen == 0 && (d < '1' || d > '8')) return;   /* pages 100-899 */
    g_entry[g_entryLen++] = d;
    if (g_entryLen == 3) {
        g_entry[3] = 0;
        g_entryLen = 0;
        g_hold = false;
        RequestPage((int)strtol(g_entry, NULL, 16), 0);
    }
    Redraw();
}

/* Where the page goes in the window: a 4:3 "TV picture", centred,
 * with a small black border like a real set. */
static void GetViewRect(RECT &v)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    int w = rc.right, h = rc.bottom;
    int vw = w, vh = w * 3 / 4;
    if (vh > h) { vh = h; vw = h * 4 / 3; }
    vw = vw * 92 / 100;                 /* border */
    vh = vh * 92 / 100;
    v.left = (w - vw) / 2;
    v.top = (h - vh) / 2;
    v.right = v.left + vw;
    v.bottom = v.top + vh;
}

/* Mouse click: a 3-digit page number on screen, or a Fastext colour */
static void ClickAt(int px, int py)
{
    RECT v;
    GetViewRect(v);
    if (v.right <= v.left || v.bottom <= v.top) return;
    if (px < v.left || py < v.top) return;
    int col = (px - v.left) * SCREEN_W / (v.right - v.left) / CW;
    int row = (py - v.top) * SCREEN_H / (v.bottom - v.top) / CH;
    if (row < 1 || row >= ROWS || col < 0 || col >= COLS) return;

    unsigned char *r = g_screen[row];
    if (isdigit(r[col] & 0x7F)) {
        int a = col, b = col;
        while (a > 0 && isdigit(r[a - 1] & 0x7F)) a--;
        while (b < COLS - 1 && isdigit(r[b + 1] & 0x7F)) b++;
        if (b - a == 2 && r[a] >= '1' && r[a] <= '8') {
            char num[4] = { (char)r[a], (char)r[a + 1], (char)r[a + 2], 0 };
            g_hold = false;
            RequestPage((int)strtol(num, NULL, 16), 0);
            return;
        }
    }
    if (row == ROWS - 1 && FlashShowing()) {       /* newsflash strip: go to the story */
        g_hold = false;
        RequestPage(g_flashPage, 0);
        return;
    }
    if (row == ROWS - 1) {          /* Fastext row: work out text colour at the click */
        int fg = 7;
        for (int c = 0; c <= col; c++) {
            unsigned char ch = r[c] & 0x7F;
            if (ch <= 0x07) fg = ch;
        }
        switch (fg) {
        case 1: GoLink(0); break;
        case 2: GoLink(1); break;
        case 3: GoLink(2); break;
        case 6: GoLink(3); break;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  Window procedure                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SOCKET: {
        if ((SOCKET)wp != g_sock) return 0;
        int ev = WSAGETSELECTEVENT(lp), err = WSAGETSELECTERROR(lp);
        if (ev == FD_CONNECT) {
            if (err) { NetRetryLater("Cannot connect to the server"); return 0; }
            g_connected = true;
            g_rx.clear();
            memset(g_screen, ' ', sizeof(g_screen));
            RequestPage(g_pageNum, 0);
        } else if (ev == FD_READ) {
            char buf[4096];
            int n = recv(g_sock, buf, sizeof(buf), 0);
            if (n > 0) { g_rx.append(buf, n); ProcessRx(); }
        } else if (ev == FD_CLOSE) {
            NetRetryLater("Connection to the server was lost");
        }
        return 0;
    }

    case WM_TIMER:
        if (wp == TIMER_FLASH) { g_flashOn = !g_flashOn; Redraw(); }
        else if (wp == TIMER_TICKER) {
            if (FlashShowing()) { g_flashPos++; Redraw(); }
        }
        else if (wp == TIMER_RETRY) { KillTimer(hwnd, TIMER_RETRY); NetConnect(); }
        return 0;

    case WM_CHAR:
        if (wp >= '0' && wp <= '9') { DigitKey((char)wp); return 0; }
        switch (tolower((int)wp)) {
        case 'r': g_reveal = !g_reveal; Redraw(); break;
        case 'h':
            g_hold = !g_hold;
            if (!g_hold) RequestPage(g_pageNum, 0);   /* catch up */
            UpdateTitle(); Redraw();
            break;
        case 'i': GoLink(4); break;
        case 27:                                     /* Esc */
            if (g_entryLen) g_entryLen = 0;
            else if (FlashShowing()) g_flashDismissed = g_flashText;
            Redraw();
            break;
        }
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_F1: GoLink(0); break;
        case VK_F2: GoLink(1); break;
        case VK_F3: GoLink(2); break;
        case VK_F4: GoLink(3); break;
        case VK_F5: GoLink(4); break;
        case VK_F6:
            if (!g_flashText.empty()) { g_hold = false; RequestPage(g_flashPage, 0); }
            break;
        case VK_UP:    case VK_PRIOR: StepPage(+1); break;
        case VK_DOWN:  case VK_NEXT:  StepPage(-1); break;
        case VK_RIGHT: StepSubpage(+1); break;
        case VK_LEFT:  StepSubpage(-1); break;
        }
        return 0;

    case WM_LBUTTONDOWN:
        ClickAt((short)LOWORD(lp), (short)HIWORD(lp));
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        Redraw();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RenderScreen();

        RECT rc, v;
        GetClientRect(hwnd, &rc);
        GetViewRect(v);
        /* black border around the picture */
        HBRUSH black = (HBRUSH)GetStockObject(BLACK_BRUSH);
        RECT b;
        SetRect(&b, 0, 0, rc.right, v.top);              FillRect(dc, &b, black);
        SetRect(&b, 0, v.bottom, rc.right, rc.bottom);   FillRect(dc, &b, black);
        SetRect(&b, 0, v.top, v.left, v.bottom);         FillRect(dc, &b, black);
        SetRect(&b, v.right, v.top, rc.right, v.bottom); FillRect(dc, &b, black);

        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = SCREEN_W;
        bi.bmiHeader.biHeight      = -SCREEN_H;          /* top-down */
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        if (g_smooth) {
            SetStretchBltMode(dc, HALFTONE);             /* soft, like a CRT */
            SetBrushOrgEx(dc, 0, 0, NULL);
        } else {
            SetStretchBltMode(dc, COLORONCOLOR);         /* hard pixels */
        }
        StretchDIBits(dc, v.left, v.top, v.right - v.left, v.bottom - v.top,
                      0, 0, SCREEN_W, SCREEN_H, g_pix, &bi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY:
        NetSend("QUIT\n");
        NetClose();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/*  Start-up                                                           */
/* ------------------------------------------------------------------ */

static void LoadConfig()
{
    FILE *f = fopen("ttxclient.cfg", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64], val[192];
            if (sscanf(line, " %63[^= ] = %191[^\r\n]", key, val) != 2) continue;
            if      (!_stricmp(key, "host")) { strncpy(g_host, val, sizeof(g_host) - 1); }
            else if (!_stricmp(key, "port")) { g_port = atoi(val); }
            else if (!_stricmp(key, "scaling")) { g_smooth = _stricmp(val, "sharp") != 0; }
            else if (!_stricmp(key, "page")) { g_pageNum = (int)strtol(val, NULL, 16); }
        }
        fclose(f);
    }
    if (__argc > 1) strncpy(g_host, __argv[1], sizeof(g_host) - 1);
    if (__argc > 2) g_port = atoi(__argv[2]);
    if (g_pageNum < 0x100 || g_pageNum > 0x8FF) g_pageNum = 0x100;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow)
{
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        MessageBoxA(NULL, "WSAStartup failed", "Teletext", MB_ICONERROR);
        return 1;
    }
    LoadConfig();

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "TtxClientWnd";
    RegisterClassA(&wc);

    BuildGlyphs();

    RECT r = { 0, 0, 800, 600 };                       /* 4:3 window */
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("TtxClientWnd", "Teletext", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, hInst, NULL);
    if (!g_hwnd) return 1;

    memset(g_screen, ' ', sizeof(g_screen));
    ShowOffline("Connecting...");
    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    SetTimer(g_hwnd, TIMER_FLASH, 500, NULL);
    SetTimer(g_hwnd, TIMER_TICKER, 250, NULL);
    NetConnect();

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    WSACleanup();
    return 0;
}
