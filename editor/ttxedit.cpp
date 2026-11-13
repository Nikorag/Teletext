/*
 * ttxedit.cpp  -  Teletext page editor for Windows (Win32 GUI)
 * ----------------------------------------------------------------
 * Creates and edits .tti pages for ttxserver, showing them exactly as
 * the client will (same SAA5050 character ROM and attribute decoding).
 *
 *  - Type text straight onto the page; insert control codes with the
 *    function keys, the Insert menu, or Esc followed by the code letter
 *  - Draw mosaic graphics with the mouse in Draw mode
 *  - Several subpages per page (for carousels), Fastext links, undo/redo
 *
 *  - Open, save and delete pages directly on a (headless) ttxserver
 *
 * Usage:  ttxedit [file.tti]
 *   ttxedit.cfg (optional, next to the .exe):
 *        pagesdir=..\server\pages      folder offered by Open / Save As
 *        server=192.168.1.20:8023      ttxserver for File > Open from server
 *        password=secret               the server's edit password
 *
 * Dev-C++: Windows Application.
 *   Project > Project Options > Parameters > Linker:
 *        -lws2_32 -lgdi32 -lcomdlg32
 *   saa5050.h must be in the same folder as this file.
 */

#include <winsock2.h>
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <string>
#include <vector>

#define ROWS        25
#define COLS        40
#define CW          12
#define CH          20
#define SCREEN_W    (COLS * CW)      /* 480 */
#define SCREEN_H    (ROWS * CH)      /* 500 */
#define STATUS_H    88
#define TIMER_BLINK 1
#define MAX_UNDO    200

#include "saa5050.h"

/* menu command ids */
enum {
    ID_NEW = 100, ID_OPEN, ID_SAVE, ID_SAVEAS, ID_EXIT,
    ID_NETOPEN, ID_NETSAVE, ID_NETDELETE,
    ID_UNDO, ID_REDO, ID_INSROW, ID_DELROW, ID_CLRROW, ID_CLRSUB,
    ID_SUBADD, ID_SUBDUP, ID_SUBDEL, ID_SUBNEXT, ID_SUBPREV,
    ID_SETTINGS, ID_SHOWCODES, ID_REVEAL, ID_DRAWMODE, ID_HELPKEYS,
    ID_CODE_BASE = 1000              /* + control code 0x00-0x1F */
};

/* ------------------------------------------------------------------ */
/*  Document                                                           */
/* ------------------------------------------------------------------ */

struct SubPage { unsigned char cell[ROWS][COLS]; };
struct UndoItem { int sub; SubPage page; };

static std::vector<SubPage>  g_subs;
static int          g_cur      = 0;              /* current subpage index   */
static int          g_pageNum  = 0x100;
static int          g_cycle    = 8;
static int          g_links[5] = { 0, 0, 0, 0, 0x100 };   /* R G Y C index */
static std::string  g_desc;
static char         g_path[MAX_PATH] = "";
static bool         g_dirty    = false;
static char         g_pagesDir[MAX_PATH] = "";
static bool         g_remote   = false;          /* page came from the server */
static char         g_server[128]  = "";         /* host:port                 */
static char         g_password[64] = "";

static std::vector<UndoItem> g_undo, g_redo;

/* ------------------------------------------------------------------ */
/*  Editor state                                                       */
/* ------------------------------------------------------------------ */

static int   g_row = 1, g_col = 0;
static bool  g_showCodes = true, g_reveal = false, g_drawMode = false;
static bool  g_phase = true;          /* blink phase for cursor and flash */
static bool  g_escPending = false;
static int   g_mouseMode = 0;         /* 0 none, 1 setting sixels, 2 clearing */
static std::string g_message;         /* one-off message in the status bar */

static int         g_promptStep = 0;  /* page settings prompts, 0 = none */
static std::string g_promptText, g_promptBuf;

static HWND   g_hwnd;
static HMENU  g_menu;
static HFONT  g_uiFont;

static unsigned int   g_pix[SCREEN_H][SCREEN_W];
static unsigned short g_glyph[96][20];
static bool           g_rowDbl[ROWS];

static const unsigned int g_pal[8] = {
    0x000000, 0xFF0000, 0x00FF00, 0xFFFF00,
    0x0000FF, 0xFF00FF, 0x00FFFF, 0xFFFFFF
};

static const char *g_codeName[32] = {
    "Alpha black", "Alpha red", "Alpha green", "Alpha yellow",
    "Alpha blue", "Alpha magenta", "Alpha cyan", "Alpha white",
    "Flash", "Steady", "End box", "Start box",
    "Normal height", "Double height", "Double width", "Double size",
    "Graphics black", "Graphics red", "Graphics green", "Graphics yellow",
    "Graphics blue", "Graphics magenta", "Graphics cyan", "Graphics white",
    "Conceal", "Contiguous graphics", "Separated graphics", "(Escape)",
    "Black background", "New background", "Hold graphics", "Release graphics"
};

static SubPage &Cur() { return g_subs[g_cur]; }

/* defined further down */
static bool NetOpenPage(int page);
static bool NetDeletePage(int page);
static void Redraw() { InvalidateRect(g_hwnd, NULL, FALSE); }

static void BlankSub(SubPage &s)
{
    memset(s.cell, ' ', sizeof(s.cell));
}

static void UpdateTitle()
{
    char t[MAX_PATH + 64];
    const char *name = g_path[0] ? strrchr(g_path, '\\') : NULL;
    name = name ? name + 1 : (g_path[0] ? g_path : "untitled");
    if (g_remote) name = "on the server";
    sprintf(t, "Teletext Editor - P%03X - %s%s", g_pageNum, name, g_dirty ? " *" : "");
    SetWindowTextA(g_hwnd, t);
}

static void Changed()
{
    g_dirty = true;
    UpdateTitle();
    Redraw();
}

static void PushUndo()
{
    UndoItem u;
    u.sub = g_cur;
    u.page = Cur();
    g_undo.push_back(u);
    if (g_undo.size() > MAX_UNDO) g_undo.erase(g_undo.begin());
    g_redo.clear();
}

static void UndoRedo(std::vector<UndoItem> &from, std::vector<UndoItem> &to)
{
    if (from.empty()) { g_message = "Nothing to undo/redo"; Redraw(); return; }
    UndoItem u = from.back();
    from.pop_back();
    if (u.sub >= (int)g_subs.size()) return;
    UndoItem now;
    now.sub = u.sub;
    now.page = g_subs[u.sub];
    to.push_back(now);
    g_cur = u.sub;
    g_subs[u.sub] = u.page;
    Changed();
}

/* ------------------------------------------------------------------ */
/*  Rendering (same as the client, plus control-code display/cursor)   */
/* ------------------------------------------------------------------ */

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
        for (int r = 0; r < 9; r++)
            for (int x = 0; x < 5; x++) {
                bool a0 = (rom[r]     & (0x20 >> x))       != 0;
                bool a1 = (rom[r]     & (0x20 >> (x + 1))) != 0;
                bool b0 = (rom[r + 1] & (0x20 >> x))       != 0;
                bool b1 = (rom[r + 1] & (0x20 >> (x + 1))) != 0;
                unsigned short pL = (unsigned short)(0x800 >> (2 * x + 1));
                unsigned short pR = (unsigned short)(0x800 >> (2 * x + 2));
                if (a0 && b1 && !a1 && !b0) { g[2 * r + 1] |= pR; g[2 * r + 2] |= pL; }
                if (a1 && b0 && !a0 && !b1) { g[2 * r + 1] |= pL; g[2 * r + 2] |= pR; }
            }
    }
}

static void FillPix(int x, int y, int w, int h, unsigned int colour)
{
    for (int yy = y; yy < y + h && yy < SCREEN_H; yy++)
        for (int xx = x; xx < x + w && xx < SCREEN_W; xx++)
            g_pix[yy][xx] = colour;
}

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
        for (int k = 0; k < 2; k++)
            if (c & bit[r][k]) {
                int xx = x + k * (CW / 2);
                if (sep) FillPix(xx + gx, yy, CW / 2 - gx, rowH[r] - gy, g_pal[fg & 7]);
                else     FillPix(xx, yy, CW / 2, rowH[r], g_pal[fg & 7]);
            }
        yy += rowH[r];
    }
}

static void DrawGlyph(int x, int y, int h, unsigned char c, unsigned int colour, bool dbl)
{
    if (c < 0x20 || c > 0x7F) return;
    const unsigned short *g = g_glyph[c - 0x20];
    for (int line = 0; line < h; line++) {
        int yy = y + line;
        if (yy >= SCREEN_H) break;
        unsigned short bits = g[dbl ? line / 2 : line];
        for (int px = 0; px < CW; px++)
            if (bits & (0x800 >> px)) g_pix[yy][x + px] = colour;
    }
}

/* A control code shown as its Esc letter: colour codes in their colour */
static void DrawCodeCell(int x, int y, unsigned char code)
{
    unsigned int bg = 0x404040, fg = 0xFFFFFF;
    if (code <= 0x07)                       fg = code ? g_pal[code] : 0x808080;
    else if (code >= 0x10 && code <= 0x17) { bg = (g_pal[code - 0x10] >> 1) & 0x7F7F7F; fg = 0xFFFFFF; }
    FillPix(x, y, CW, CH, bg);
    DrawGlyph(x, y, CH, (unsigned char)(code + 0x40), fg, false);
}

static void RenderPage()
{
    memset(g_pix, 0, sizeof(g_pix));
    const SubPage &sp = Cur();
    bool skipRow = false;

    for (int row = 0; row < ROWS; row++) {
        g_rowDbl[row] = false;
        if (skipRow) { skipRow = false; continue; }

        unsigned char rb[COLS];
        if (row == 0) {
            char hdr[48];
            sprintf(hdr, " %03X    " "\x06" "(header row: made by the server)", g_pageNum);
            memset(rb, ' ', COLS);
            memcpy(rb, hdr, strlen(hdr) > COLS ? COLS : strlen(hdr));
        } else {
            memcpy(rb, sp.cell[row], COLS);
        }

        bool rowHasDbl = false;
        if (row > 0 && row < ROWS - 1)
            for (int c = 0; c < COLS; c++)
                if ((rb[c] & 0x7F) == 0x0D) rowHasDbl = true;
        g_rowDbl[row] = rowHasDbl;

        int  fg = 7, bg = 0;
        bool gfx = false, sep = false, hold = false, flash = false, conceal = false, dbl = false;
        unsigned char held = 0x20;
        bool heldSep = false;
        int  y = row * CH;

        for (int col = 0; col < COLS; col++) {
            unsigned char c = rb[col] & 0x7F;
            int x = col * CW;

            switch (c) {
            case 0x09: flash = false; break;
            case 0x0C: if (dbl) held = 0x20; dbl = false; break;
            case 0x18: conceal = true; break;
            case 0x19: sep = false; break;
            case 0x1A: sep = true; break;
            case 0x1C: bg = 0; break;
            case 0x1D: bg = fg; break;
            case 0x1E: hold = true; break;
            }

            unsigned char show = 0x20;
            bool showSep = sep, mosaic = false;
            if (c < 0x20) {
                if (hold && gfx) { show = held; showSep = heldSep; mosaic = true; }
            } else {
                show = c;
                if (gfx && (c & 0x20)) { mosaic = true; held = c; heldSep = sep; }
            }

            FillPix(x, y, CW, rowHasDbl ? 2 * CH : CH, g_pal[bg]);
            if (c < 0x20 && g_showCodes && row > 0) {
                DrawCodeCell(x, y, c);
            } else {
                bool visible = !(flash && !g_phase) && !(conceal && !g_reveal);
                if (visible && show != 0x20) {
                    int h = dbl ? 2 * CH : CH;
                    if (mosaic) DrawMosaic(x, y, h, show, fg, showSep);
                    else        DrawGlyph(x, y, h, show, g_pal[fg & 7], dbl);
                }
            }

            if (c <= 0x07)                   { fg = c; gfx = false; conceal = false; held = 0x20; }
            else if (c >= 0x10 && c <= 0x17) { fg = c - 0x10; gfx = true; conceal = false; }
            else if (c == 0x08)              { flash = true; }
            else if (c == 0x0D && rowHasDbl) { dbl = true; held = 0x20; }
            else if (c == 0x1F)              { hold = false; }
        }
        if (rowHasDbl) skipRow = true;
    }

    /* cursor: an inverted box, blinking in text mode */
    if (g_drawMode || g_phase) {
        int x0 = g_col * CW, y0 = g_row * CH;
        for (int yy = y0; yy < y0 + CH; yy++)
            for (int xx = x0; xx < x0 + CW; xx++) {
                bool edge = yy < y0 + 2 || yy >= y0 + CH - 2 || xx < x0 + 1 || xx >= x0 + CW - 1;
                if (edge) g_pix[yy][xx] ^= 0xFFFFFF;
            }
    }
}

/* ------------------------------------------------------------------ */
/*  Layout and mouse mapping                                           */
/* ------------------------------------------------------------------ */

static void GetLayout(RECT &view, RECT &status)
{
    RECT rc;
    GetClientRect(g_hwnd, &rc);
    SetRect(&status, 0, rc.bottom - STATUS_H, rc.right, rc.bottom);
    int w = rc.right - 16, h = rc.bottom - STATUS_H - 16;
    if (w < 40) w = 40;
    if (h < 30) h = 30;
    int vw = w, vh = w * 3 / 4;
    if (vh > h) { vh = h; vw = h * 4 / 3; }
    view.left = (rc.right - vw) / 2;
    view.top = 8 + (h - vh) / 2;
    view.right = view.left + vw;
    view.bottom = view.top + vh;
}

/* Attribute state at a cell, worked out from the codes to its left */
static bool CellIsGraphics(int row, int col)
{
    bool gfx = false;
    const unsigned char *r = Cur().cell[row];
    for (int c = 0; c < col; c++) {
        unsigned char x = r[c] & 0x7F;
        if (x <= 0x07) gfx = false;
        else if (x >= 0x10 && x <= 0x17) gfx = true;
    }
    return gfx;
}

static bool CellIsDouble(int row, int col)
{
    if (!g_rowDbl[row]) return false;
    bool dbl = false;
    const unsigned char *r = Cur().cell[row];
    for (int c = 0; c <= col; c++) {
        unsigned char x = r[c] & 0x7F;
        if (x == 0x0C) dbl = false;
        if (c < col && x == 0x0D) dbl = true;
    }
    return dbl;
}

/* Which cell, and which of its six sixels (sx 0-1, sy 0-2), is under the mouse */
static bool MouseToCell(int px, int py, int &row, int &col, int &sx, int &sy)
{
    RECT v, s;
    GetLayout(v, s);
    if (px < v.left || px >= v.right || py < v.top || py >= v.bottom) return false;
    int bx = (px - v.left) * SCREEN_W / (v.right - v.left);
    int by = (py - v.top) * SCREEN_H / (v.bottom - v.top);
    row = by / CH;
    col = bx / CW;
    if (row >= ROWS || col >= COLS) return false;
    int line = by % CH;
    if (row > 0 && g_rowDbl[row - 1]) { row--; line += CH; }   /* lower half */
    sx = (bx % CW) / (CW / 2);
    if (CellIsDouble(row, col)) line /= 2;
    else if (line >= CH) { sy = -1; return true; }
    sy = line < 6 ? 0 : (line < 14 ? 1 : 2);
    return true;
}

static void SetSixel(int row, int col, int sx, int sy, bool on)
{
    static const unsigned char bit[3][2] = { {0x01, 0x02}, {0x04, 0x08}, {0x10, 0x40} };
    if (row < 1 || sy < 0) return;
    unsigned char &ch = Cur().cell[row][col];
    unsigned char c = ch & 0x7F;
    if (c < 0x20) return;                          /* a control code cell */
    if (!CellIsGraphics(row, col)) {
        g_message = "Not a graphics cell: put a graphics colour code (Shift+F1-F7) to its left";
        return;
    }
    if (c >= 0x40 && c < 0x60) c = 0x20;           /* letter -> blank mosaic */
    unsigned char nc = on ? (unsigned char)(c | bit[sy][sx])
                          : (unsigned char)(c & ~bit[sy][sx]);
    nc |= 0x20;
    if (nc != ch) { ch = nc; g_dirty = true; }
}

/* ------------------------------------------------------------------ */
/*  Editing                                                            */
/* ------------------------------------------------------------------ */

static void MoveRight()
{
    if (g_col < COLS - 1) g_col++;
    else if (g_row < ROWS - 1) { g_row++; g_col = 0; }
}

static void PutCell(unsigned char c)
{
    PushUndo();
    Cur().cell[g_row][g_col] = c;
    MoveRight();
    Changed();
}

static void TypeChar(unsigned int ch)
{
    if (ch == 0xA3)      ch = 0x23;               /* pound sign */
    else if (ch == '#')  ch = 0x5F;               /* # lives at 0x5F in the UK set */
    if (ch < 0x20 || ch > 0x7E) return;
    PutCell((unsigned char)ch);
}

static void InsertRow()
{
    PushUndo();
    for (int r = ROWS - 1; r > g_row; r--) memcpy(Cur().cell[r], Cur().cell[r - 1], COLS);
    memset(Cur().cell[g_row], ' ', COLS);
    Changed();
}

static void DeleteRow()
{
    PushUndo();
    for (int r = g_row; r < ROWS - 1; r++) memcpy(Cur().cell[r], Cur().cell[r + 1], COLS);
    memset(Cur().cell[ROWS - 1], ' ', COLS);
    Changed();
}

static void ClearRow()
{
    PushUndo();
    memset(Cur().cell[g_row], ' ', COLS);
    Changed();
}

static void ClearSub()
{
    PushUndo();
    BlankSub(Cur());
    Changed();
}

static void SubpageOp(int id)
{
    if (id == ID_SUBNEXT || id == ID_SUBPREV) {
        int n = (int)g_subs.size();
        g_cur = (g_cur + (id == ID_SUBNEXT ? 1 : n - 1)) % n;
        Redraw();
        return;
    }
    if (id == ID_SUBADD || id == ID_SUBDUP) {
        SubPage s;
        if (id == ID_SUBADD) BlankSub(s); else s = Cur();
        g_subs.insert(g_subs.begin() + g_cur + 1, s);
        g_cur++;
    } else if (id == ID_SUBDEL) {
        if (g_subs.size() < 2) { g_message = "A page needs at least one subpage"; Redraw(); return; }
        if (MessageBoxA(g_hwnd, "Delete this subpage?", "Teletext Editor",
                        MB_YESNO | MB_ICONQUESTION) != IDYES) return;
        g_subs.erase(g_subs.begin() + g_cur);
        if (g_cur >= (int)g_subs.size()) g_cur = (int)g_subs.size() - 1;
    }
    g_undo.clear();                       /* undo history is per subpage layout */
    g_redo.clear();
    Changed();
}

/* ------------------------------------------------------------------ */
/*  Page settings prompts (shown in the status bar)                    */
/* ------------------------------------------------------------------ */

static void SetPrompt(int step)
{
    char buf[128];
    g_promptStep = step;
    switch (step) {
    case 1:
        g_promptText = "Page number (100-8FF):";
        sprintf(buf, "%03X", g_pageNum);
        g_promptBuf = buf;
        break;
    case 2:
        g_promptText = "Seconds per subpage (carousel):";
        sprintf(buf, "%d", g_cycle);
        g_promptBuf = buf;
        break;
    case 3:
        g_promptText = "Fastext red green yellow cyan index (000 = none):";
        sprintf(buf, "%03X %03X %03X %03X %03X",
                g_links[0], g_links[1], g_links[2], g_links[3], g_links[4]);
        g_promptBuf = buf;
        break;
    case 4:
        g_promptText = "Description (for your reference):";
        g_promptBuf = g_desc;
        break;
    case 10:
        g_promptText = "Open which page from the server?";
        g_promptBuf = "";
        break;
    case 11:
        g_promptText = "Delete which page from the server?";
        sprintf(buf, "%03X", g_pageNum);
        g_promptBuf = buf;
        break;
    default:
        g_promptStep = 0;
    }
    Redraw();
}

static void PromptEnter()
{
    const char *s = g_promptBuf.c_str();
    if (g_promptStep == 10 || g_promptStep == 11) {
        int n = (int)strtol(s, NULL, 16);
        int step = g_promptStep;
        g_promptStep = 0;
        g_message.clear();
        if (n < 0x100 || n > 0x8FF) { g_message = "Page must be 100-8FF"; Redraw(); return; }
        if (step == 10) {
            NetOpenPage(n);
        } else {
            char q[64];
            sprintf(q, "Delete page %03X from the server?", n);
            if (MessageBoxA(g_hwnd, q, "Teletext Editor", MB_YESNO | MB_ICONQUESTION) == IDYES)
                NetDeletePage(n);
        }
        Redraw();
        return;
    }
    switch (g_promptStep) {
    case 1: {
        int n = (int)strtol(s, NULL, 16);
        if (n < 0x100 || n > 0x8FF) { g_message = "Page must be 100-8FF"; Redraw(); return; }
        g_pageNum = n;
        break;
    }
    case 2: {
        int n = atoi(s);
        if (n < 1 || n > 600) { g_message = "Use 1-600 seconds"; Redraw(); return; }
        g_cycle = n;
        break;
    }
    case 3: {
        int l[5] = { 0, 0, 0, 0, 0 };
        sscanf(s, "%x %x %x %x %x", &l[0], &l[1], &l[2], &l[3], &l[4]);
        for (int i = 0; i < 5; i++)
            g_links[i] = (l[i] >= 0x100 && l[i] <= 0x8FF && (l[i] & 0xFF) != 0xFF) ? l[i] : 0;
        break;
    }
    case 4:
        g_desc = g_promptBuf;
        break;
    }
    g_dirty = true;
    UpdateTitle();
    SetPrompt(g_promptStep + 1);
}

/* ------------------------------------------------------------------ */
/*  TTI files                                                          */
/* ------------------------------------------------------------------ */

static void DecodeOL(const char *text, unsigned char *row)
{
    const unsigned char *p = (const unsigned char *)text;
    int col = 0;
    while (*p && col < COLS) {
        unsigned char c = *p++;
        if (c == 0x1B) {
            if (!*p) break;
            c = (unsigned char)((*p++ - 0x40) & 0x7F);
        } else {
            c &= 0x7F;
        }
        row[col++] = c;
    }
}

static int ParseLink(const char *s)
{
    int v = (int)strtol(s, NULL, 16);
    if ((v & 0xFF) == 0xFF) return 0;
    if ((v >> 8) == 0) v |= 0x800;
    if (v < 0x100 || v > 0x8FF) return 0;
    return v;
}

/* Read a page from .tti text (from a file or from the server).
 * 'where' is the file path, or "" for a page from the server. */
static bool ParseTTI(const std::string &text, const char *where)
{
    std::vector<SubPage> subs;
    int page = -1, cycle = 8, links[5] = { 0, 0, 0, 0, 0x100 };
    bool skipping = false, others = false;
    std::string desc;

    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string ln = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!ln.empty() && (ln[ln.size() - 1] == '\r' || ln[ln.size() - 1] == '\n'))
            ln.erase(ln.size() - 1);
        const char *line = ln.c_str();

        if (strncmp(line, "DE,", 3) == 0) {
            desc = line + 3;
        } else if (strncmp(line, "CT,", 3) == 0) {
            cycle = atoi(line + 3);
            if (cycle < 1) cycle = 8;
        } else if (strncmp(line, "PN,", 3) == 0) {
            char d[4] = { 0, 0, 0, 0 };
            memcpy(d, line + 3, 3);
            int num = (int)strtol(d, NULL, 16);
            if ((num >> 8) == 0) num |= 0x800;
            if (page < 0) page = num;
            if (num != page) { skipping = true; others = true; continue; }
            skipping = false;
            SubPage s;
            BlankSub(s);
            subs.push_back(s);
        } else if (strncmp(line, "OL,", 3) == 0 && !skipping && !subs.empty()) {
            int row = atoi(line + 3);
            const char *t = strchr(line + 3, ',');
            if (t && row >= 1 && row < ROWS) DecodeOL(t + 1, subs.back().cell[row]);
        } else if (strncmp(line, "FL,", 3) == 0 && !skipping) {
            const char *q = line + 3;
            int v[6] = { 0, 0, 0, 0, 0, 0 };
            for (int i = 0; i < 6 && q; i++) {
                v[i] = ParseLink(q);
                q = strchr(q, ',');
                if (q) q++;
            }
            links[0] = v[0]; links[1] = v[1]; links[2] = v[2]; links[3] = v[3];
            links[4] = v[5] ? v[5] : 0x100;
        }
    }

    if (subs.empty()) {
        MessageBoxA(g_hwnd, "No page (PN line) found.", "Teletext Editor", MB_ICONERROR);
        return false;
    }
    g_subs = subs;
    g_cur = 0;
    g_pageNum = page;
    g_cycle = cycle;
    for (int i = 0; i < 5; i++) g_links[i] = links[i];
    g_desc = desc;
    strncpy(g_path, where, MAX_PATH - 1);
    g_path[MAX_PATH - 1] = 0;
    g_remote = (where[0] == 0);
    g_dirty = false;
    g_undo.clear();
    g_redo.clear();
    g_row = 1; g_col = 0;
    g_message = others ? "File holds several pages: only the first was loaded"
                       : (g_remote ? "Opened from the server - Ctrl+S saves it back" : "Loaded");
    UpdateTitle();
    Redraw();
    return true;
}

static bool LoadTTI(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        MessageBoxA(g_hwnd, "Cannot open that file.", "Teletext Editor", MB_ICONERROR);
        return false;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) text.append(buf, n);
    fclose(f);
    return ParseTTI(text, path);
}

static void EncodeRow(const unsigned char *row, std::string &out)
{
    int len = COLS;
    while (len > 0 && row[len - 1] == ' ') len--;
    for (int i = 0; i < len; i++) {
        unsigned char c = row[i] & 0x7F;
        if (c < 0x20) { out += (char)0x1B; out += (char)(c + 0x40); }
        else          out += (char)c;
    }
}

/* The current page as .tti text */
static std::string MakeTTI()
{
    std::string s;
    char buf[96];
    s += "DE," + g_desc + "\r\n";
    sprintf(buf, "CT,%d,T\r\n", g_cycle);
    s += buf;
    bool multi = g_subs.size() > 1;
    for (size_t sp = 0; sp < g_subs.size(); sp++) {
        sprintf(buf, "PN,%03X%02d\r\nSC,%04d\r\nPS,8000\r\n",
                g_pageNum, multi ? (int)sp + 1 : 0, multi ? (int)sp + 1 : 0);
        s += buf;
        for (int r = 1; r < ROWS; r++) {
            std::string t;
            EncodeRow(g_subs[sp].cell[r], t);
            if (t.empty()) continue;
            sprintf(buf, "OL,%d,", r);
            s += buf + t + "\r\n";
        }
    }
    int l[5];
    for (int i = 0; i < 5; i++) l[i] = g_links[i] ? g_links[i] : 0x8FF;
    sprintf(buf, "FL,%03X,%03X,%03X,%03X,8FF,%03X\r\n", l[0], l[1], l[2], l[3], l[4]);
    s += buf;
    return s;
}

static bool SaveTTI(const char *path)
{
    std::string t = MakeTTI();
    FILE *f = fopen(path, "wb");
    if (!f) {
        MessageBoxA(g_hwnd, "Cannot write that file.", "Teletext Editor", MB_ICONERROR);
        return false;
    }
    fwrite(t.data(), 1, t.size(), f);
    fclose(f);

    strncpy(g_path, path, MAX_PATH - 1);
    g_remote = false;
    g_dirty = false;
    g_message = "Saved";
    UpdateTitle();
    Redraw();
    return true;
}

/* ------------------------------------------------------------------ */
/*  Talking to the server                                              */
/*  Each job opens a connection, gives the password, sends one        */
/*  command, reads the reply and closes.                              */
/* ------------------------------------------------------------------ */

static void NetFail(const std::string &why)
{
    std::string m = "Server " + std::string(g_server) + ":\n\n" + why;
    MessageBoxA(g_hwnd, m.c_str(), "Teletext Editor", MB_ICONERROR);
}

static bool NetRecvLine(SOCKET s, std::string &buf, std::string &line)
{
    for (;;) {
        size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            return true;
        }
        char tmp[4096];
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
    }
}

static bool NetRecvBytes(SOCKET s, std::string &buf, int len, std::string &out)
{
    while ((int)buf.size() < len) {
        char tmp[4096];
        int n = recv(s, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, n);
    }
    out = buf.substr(0, len);
    buf.erase(0, len);
    return true;
}

static bool NetSendAll(SOCKET s, const std::string &data)
{
    size_t done = 0;
    while (done < data.size()) {
        int n = send(s, data.data() + done, (int)(data.size() - done), 0);
        if (n <= 0) return false;
        done += n;
    }
    return true;
}

/* Connect (5 second limit) and give the password */
static SOCKET NetOpen(std::string &buf)
{
    if (!g_server[0]) {
        NetFail("No server set. Add  server=address:port  to ttxedit.cfg");
        return INVALID_SOCKET;
    }
    char host[128];
    int port = 8023;
    strncpy(host, g_server, sizeof(host) - 1);
    host[sizeof(host) - 1] = 0;
    char *colon = strchr(host, ':');
    if (colon) { *colon = 0; port = atoi(colon + 1); }

    unsigned long ip = inet_addr(host);
    if (ip == INADDR_NONE) {
        hostent *he = gethostbyname(host);
        if (!he) { NetFail("Cannot find that server name"); return INVALID_SOCKET; }
        memcpy(&ip, he->h_addr_list[0], 4);
    }
    sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons((u_short)port);
    a.sin_addr.s_addr = ip;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { NetFail("Cannot create a socket"); return s; }

    /* non-blocking connect so we can give up after 5 seconds */
    u_long nb = 1;
    ioctlsocket(s, FIONBIO, &nb);
    connect(s, (sockaddr *)&a, sizeof(a));
    fd_set wr, ex;
    FD_ZERO(&wr); FD_SET(s, &wr);
    FD_ZERO(&ex); FD_SET(s, &ex);
    timeval tv = { 5, 0 };
    if (select(0, NULL, &wr, &ex, &tv) <= 0 || FD_ISSET(s, &ex)) {
        closesocket(s);
        NetFail("Cannot connect. Is ttxserver running, and is the address right?");
        return INVALID_SOCKET;
    }
    nb = 0;
    ioctlsocket(s, FIONBIO, &nb);
    DWORD ms = 8000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&ms, sizeof(ms));

    if (g_password[0]) {
        std::string line;
        if (!NetSendAll(s, std::string("AUTH ") + g_password + "\n") ||
            !NetRecvLine(s, buf, line)) {
            closesocket(s);
            NetFail("No answer from the server");
            return INVALID_SOCKET;
        }
        if (line.compare(0, 2, "OK") != 0) {
            closesocket(s);
            NetFail(line);
            return INVALID_SOCKET;
        }
    }
    return s;
}

/* Ask the server which pages it has: "100 101 ..." */
static bool NetList(std::string &list)
{
    std::string buf, line;
    SOCKET s = NetOpen(buf);
    if (s == INVALID_SOCKET) return false;
    bool ok = NetSendAll(s, "LIST\n") && NetRecvLine(s, buf, line);
    NetSendAll(s, "QUIT\n");
    closesocket(s);
    if (!ok || line.compare(0, 5, "PAGES") != 0) { NetFail("Unexpected reply: " + line); return false; }
    list = line.size() > 6 ? line.substr(6) : "";
    return true;
}

static bool NetOpenPage(int page)
{
    std::string buf, line, data;
    SOCKET s = NetOpen(buf);
    if (s == INVALID_SOCKET) return false;
    char cmd[32];
    sprintf(cmd, "GETTTI %03X\n", page);
    bool ok = NetSendAll(s, cmd) && NetRecvLine(s, buf, line);
    int pn = 0, len = 0;
    if (ok && sscanf(line.c_str(), "TTI %x %d", &pn, &len) == 2)
        ok = NetRecvBytes(s, buf, len, data);
    else
        ok = false;
    NetSendAll(s, "QUIT\n");
    closesocket(s);
    if (!ok) {
        NetFail(line.compare(0, 3, "ERR") == 0 ? line.substr(4) : "Could not read the page");
        return false;
    }
    return ParseTTI(data, "");
}

static bool NetSavePage()
{
    std::string buf, line, t = MakeTTI();
    SOCKET s = NetOpen(buf);
    if (s == INVALID_SOCKET) return false;
    char cmd[48];
    sprintf(cmd, "PUTTTI %03X %d\n", g_pageNum, (int)t.size());
    bool ok = NetSendAll(s, cmd + t) && NetRecvLine(s, buf, line);
    NetSendAll(s, "QUIT\n");
    closesocket(s);
    if (!ok || line.compare(0, 2, "OK") != 0) {
        NetFail(line.compare(0, 3, "ERR") == 0 ? line.substr(4) : "No answer from the server");
        return false;
    }
    g_remote = true;
    g_path[0] = 0;
    g_dirty = false;
    char m[64];
    sprintf(m, "Saved to the server as page %03X", g_pageNum);
    g_message = m;
    UpdateTitle();
    Redraw();
    return true;
}

static bool NetDeletePage(int page)
{
    std::string buf, line;
    SOCKET s = NetOpen(buf);
    if (s == INVALID_SOCKET) return false;
    char cmd[32];
    sprintf(cmd, "DELETE %03X\n", page);
    bool ok = NetSendAll(s, cmd) && NetRecvLine(s, buf, line);
    NetSendAll(s, "QUIT\n");
    closesocket(s);
    if (!ok || line.compare(0, 2, "OK") != 0) {
        NetFail(line.compare(0, 3, "ERR") == 0 ? line.substr(4) : "No answer from the server");
        return false;
    }
    char m[64];
    sprintf(m, "Page %03X deleted from the server", page);
    g_message = m;
    Redraw();
    return true;
}

static bool FileDialog(bool save, char *out)
{
    OPENFILENAMEA ofn;
    memset(&ofn, 0, sizeof(ofn));
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = g_hwnd;
    ofn.lpstrFilter = "Teletext pages (*.tti)\0*.tti\0All files (*.*)\0*.*\0";
    ofn.lpstrFile   = out;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrDefExt = "tti";
    if (g_pagesDir[0]) ofn.lpstrInitialDir = g_pagesDir;
    if (save) {
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
        return GetSaveFileNameA(&ofn) != 0;
    }
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    return GetOpenFileNameA(&ofn) != 0;
}

static bool DoSaveAs()
{
    char name[MAX_PATH];
    sprintf(name, "P%03X.tti", g_pageNum);
    if (!FileDialog(true, name)) return false;
    return SaveTTI(name);
}

/* Ctrl+S: back to wherever the page came from */
static bool DoSave()
{
    if (g_remote) return NetSavePage();
    if (!g_path[0]) return DoSaveAs();
    return SaveTTI(g_path);
}

/* true if it's OK to throw away the current page */
static bool ConfirmDiscard()
{
    if (!g_dirty) return true;
    int r = MessageBoxA(g_hwnd, "Save changes to this page?", "Teletext Editor",
                        MB_YESNOCANCEL | MB_ICONQUESTION);
    if (r == IDCANCEL) return false;
    if (r == IDYES) return DoSave();
    return true;
}

static void NewPage()
{
    g_subs.clear();
    SubPage s;
    BlankSub(s);
    g_subs.push_back(s);
    g_cur = 0;
    g_pageNum = 0x100;
    g_cycle = 8;
    g_links[0] = g_links[1] = g_links[2] = g_links[3] = 0;
    g_links[4] = 0x100;
    g_desc = "";
    g_path[0] = 0;
    g_remote = false;
    g_dirty = false;
    g_undo.clear();
    g_redo.clear();
    g_row = 1; g_col = 0;
    UpdateTitle();
    Redraw();
}

/* ------------------------------------------------------------------ */
/*  Menu and status bar                                                */
/* ------------------------------------------------------------------ */

static void BuildMenu()
{
    g_menu = CreateMenu();

    HMENU m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_NEW,    "&New\tCtrl+N");
    AppendMenuA(m, MF_STRING, ID_OPEN,   "&Open...\tCtrl+O");
    AppendMenuA(m, MF_STRING, ID_SAVE,   "&Save\tCtrl+S");
    AppendMenuA(m, MF_STRING, ID_SAVEAS, "Save &As...");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_NETOPEN,   "Open from se&rver...\tCtrl+Shift+O");
    AppendMenuA(m, MF_STRING, ID_NETSAVE,   "Save to ser&ver\tCtrl+Shift+S");
    AppendMenuA(m, MF_STRING, ID_NETDELETE, "&Delete page from server...");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_EXIT,   "E&xit");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)m, "&File");

    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_UNDO,   "&Undo\tCtrl+Z");
    AppendMenuA(m, MF_STRING, ID_REDO,   "&Redo\tCtrl+Y");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_INSROW, "&Insert row\tCtrl+Ins");
    AppendMenuA(m, MF_STRING, ID_DELROW, "&Delete row\tCtrl+Del");
    AppendMenuA(m, MF_STRING, ID_CLRROW, "&Clear row");
    AppendMenuA(m, MF_STRING, ID_CLRSUB, "Clear &whole subpage");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)m, "&Edit");

    /* every control code, with its key where there is one */
    static const char *keyFor[32] = {
        "Esc @", "F1", "F2", "F3", "F4", "F5", "F6", "F7",
        "F8", "Shift+F8", "Esc J", "Esc K", "Shift+F9", "F9", "Esc N", "Esc O",
        "Esc P", "Shift+F1", "Shift+F2", "Shift+F3", "Shift+F4", "Shift+F5", "Shift+F6", "Shift+F7",
        "Esc X", "Shift+F12", "F12", "", "Shift+F11", "F11", "Esc ^", "Esc _"
    };
    m = CreatePopupMenu();
    for (int c = 0; c < 32; c++) {
        if (c == 0x1B) continue;
        char item[64];
        sprintf(item, "%02X  %s\t%s", c, g_codeName[c], keyFor[c]);
        UINT flags = MF_STRING;
        if (c == 0x08 || c == 0x10 || c == 0x18) flags |= MF_MENUBARBREAK;
        AppendMenuA(m, flags, ID_CODE_BASE + c, item);
    }
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)m, "&Insert");

    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_SUBADD,  "&Add blank subpage");
    AppendMenuA(m, MF_STRING, ID_SUBDUP,  "&Duplicate this subpage");
    AppendMenuA(m, MF_STRING, ID_SUBDEL,  "De&lete this subpage");
    AppendMenuA(m, MF_SEPARATOR, 0, NULL);
    AppendMenuA(m, MF_STRING, ID_SUBNEXT, "&Next subpage\tPgDn");
    AppendMenuA(m, MF_STRING, ID_SUBPREV, "&Previous subpage\tPgUp");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)m, "&Subpage");

    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_SETTINGS, "Page &settings...\tCtrl+P");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)m, "&Page");

    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_SHOWCODES, "Show &control codes\tTab");
    AppendMenuA(m, MF_STRING, ID_REVEAL,    "&Reveal concealed text\tCtrl+R");
    AppendMenuA(m, MF_STRING, ID_DRAWMODE,  "&Draw mode (mouse draws graphics)\tCtrl+G");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)m, "&View");

    m = CreatePopupMenu();
    AppendMenuA(m, MF_STRING, ID_HELPKEYS, "&Keys...\tCtrl+H");
    AppendMenuA(g_menu, MF_POPUP, (UINT_PTR)m, "&Help");
}

static void UpdateMenuChecks()
{
    CheckMenuItem(g_menu, ID_SHOWCODES, g_showCodes ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(g_menu, ID_REVEAL,    g_reveal    ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(g_menu, ID_DRAWMODE,  g_drawMode  ? MF_CHECKED : MF_UNCHECKED);
}

static void ShowKeys()
{
    MessageBoxA(g_hwnd,
        "Typing\n"
        "  Letters, digits, punctuation    write at the cursor\n"
        "  Arrows, Home, End, Enter        move\n"
        "  Backspace / Del / Ins           erase, delete, insert a space\n"
        "  Ctrl+Ins / Ctrl+Del             insert / delete a row\n\n"
        "Control codes (each takes one cell)\n"
        "  F1-F7          text colour: red green yellow blue magenta cyan white\n"
        "  Shift+F1-F7    graphics colour (same order)\n"
        "  F8 / Shift+F8  flash / steady\n"
        "  F9 / Shift+F9  double height / normal height\n"
        "  F11 / Shift+F11  new background / black background\n"
        "  F12 / Shift+F12  separated / contiguous graphics\n"
        "  Esc then a letter   any code, as in .tti files (Esc A = red ...)\n\n"
        "Graphics\n"
        "  Ctrl+G   Draw mode: left button draws, right button erases\n"
        "           (cells must follow a graphics colour code)\n\n"
        "Other\n"
        "  Tab      show/hide control codes      Ctrl+R  reveal\n"
        "  PgUp/PgDn  previous/next subpage     Ctrl+P  page number, carousel, Fastext\n"
        "  Ctrl+Z / Ctrl+Y  undo / redo        Ctrl+S  save\n\n"
        "Server (set server= and password= in ttxedit.cfg)\n"
        "  Ctrl+Shift+O  open a page from the server\n"
        "  Ctrl+Shift+S  save this page to the server (Ctrl+S does too\n"
        "                once a page has come from the server)\n\n"
        "Typing # gives #, and the pound key gives the pound sign.",
        "Teletext Editor - Keys", MB_OK);
}

static void CellInfo(char *out)
{
    unsigned char c = Cur().cell[g_row][g_col] & 0x7F;
    if (c < 0x20)
        sprintf(out, "Code %02X %s (Esc %c)", c, g_codeName[c], c + 0x40);
    else if (CellIsGraphics(g_row, g_col) && (c & 0x20))
        sprintf(out, "Mosaic %02X", c);
    else
        sprintf(out, "Char %02X '%c'", c, c == 0x23 ? '#' : c);
}

static void PaintStatus(HDC dc, const RECT &s)
{
    HBRUSH b = CreateSolidBrush(RGB(30, 30, 46));
    FillRect(dc, &s, b);
    DeleteObject(b);
    HGDIOBJ oldFont = SelectObject(dc, g_uiFont);
    SetBkMode(dc, TRANSPARENT);

    char l1[256], l2[256], l3[300], info[64];
    CellInfo(info);
    sprintf(l1, "Page %03X   Subpage %d/%d   Carousel %ds   Row %d Col %d   %s   [%s]%s",
            g_pageNum, g_cur + 1, (int)g_subs.size(), g_cycle, g_row, g_col, info,
            g_drawMode ? "DRAW" : "TEXT", g_escPending ? "  Esc+?" : "");
    char lk[5][8];
    for (int i = 0; i < 5; i++) {
        if (g_links[i]) sprintf(lk[i], "%03X", g_links[i]);
        else strcpy(lk[i], "---");
    }
    sprintf(l2, "Fastext  red %s  green %s  yellow %s  cyan %s  index %s     %s",
            lk[0], lk[1], lk[2], lk[3], lk[4],
            g_remote ? (std::string("on server ") + g_server).c_str()
                     : (g_path[0] ? g_path : "(not saved yet)"));

    int y = s.top + 6;
    SetTextColor(dc, RGB(230, 230, 230));
    TextOutA(dc, 10, y, l1, (int)strlen(l1));
    SetTextColor(dc, RGB(160, 200, 255));
    TextOutA(dc, 10, y + 20, l2, (int)strlen(l2));

    if (g_promptStep) {
        sprintf(l3, "%s %s_     %s", g_promptText.c_str(), g_promptBuf.c_str(),
                g_promptStep >= 10 ? "(Enter = OK, Esc = cancel)" : "(Enter = next, Esc = finish)");
        SetTextColor(dc, RGB(255, 255, 0));
        TextOutA(dc, 10, y + 44, l3, (int)strlen(l3));
        if (!g_message.empty()) {
            SetTextColor(dc, RGB(255, 200, 120));
            TextOutA(dc, 10, y + 62, g_message.c_str(), (int)g_message.size());
        }
    } else if (!g_message.empty()) {
        SetTextColor(dc, RGB(255, 200, 120));
        TextOutA(dc, 10, y + 44, g_message.c_str(), (int)g_message.size());
    } else {
        const char *h1 = "F1-F7 text colour  Shift+F1-F7 graphics  F8 flash  F9 double height  "
                         "F11 new bg  F12 separated";
        const char *h2 = "Esc+letter any code   Ctrl+G draw   Tab codes   PgUp/PgDn subpage   "
                         "Ctrl+P settings   Ctrl+H keys";
        SetTextColor(dc, RGB(140, 140, 160));
        TextOutA(dc, 10, y + 44, h1, (int)strlen(h1));
        TextOutA(dc, 10, y + 62, h2, (int)strlen(h2));
    }
    SelectObject(dc, oldFont);
}

/* ------------------------------------------------------------------ */
/*  Commands                                                           */
/* ------------------------------------------------------------------ */

static void Command(int id)
{
    g_message.clear();
    if (id >= ID_CODE_BASE && id < ID_CODE_BASE + 32) {
        PutCell((unsigned char)(id - ID_CODE_BASE));
        return;
    }
    switch (id) {
    case ID_NEW:
        if (ConfirmDiscard()) { NewPage(); SetPrompt(1); }
        break;
    case ID_OPEN: {
        if (!ConfirmDiscard()) break;
        char name[MAX_PATH] = "";
        if (FileDialog(false, name)) LoadTTI(name);
        break;
    }
    case ID_SAVE:    DoSave(); break;
    case ID_NETOPEN: {
        if (!ConfirmDiscard()) break;
        std::string list;
        if (NetList(list)) {
            g_message = "Server pages: " + (list.empty() ? std::string("(none)") : list);
            SetPrompt(10);
        }
        break;
    }
    case ID_NETSAVE:   NetSavePage(); break;
    case ID_NETDELETE: SetPrompt(11); break;
    case ID_SAVEAS:  DoSaveAs(); break;
    case ID_EXIT:    PostMessage(g_hwnd, WM_CLOSE, 0, 0); break;
    case ID_UNDO:    UndoRedo(g_undo, g_redo); break;
    case ID_REDO:    UndoRedo(g_redo, g_undo); break;
    case ID_INSROW:  InsertRow(); break;
    case ID_DELROW:  DeleteRow(); break;
    case ID_CLRROW:  ClearRow(); break;
    case ID_CLRSUB:  ClearSub(); break;
    case ID_SUBADD: case ID_SUBDUP: case ID_SUBDEL:
    case ID_SUBNEXT: case ID_SUBPREV:
        SubpageOp(id);
        break;
    case ID_SETTINGS:  g_message.clear(); SetPrompt(1); break;
    case ID_SHOWCODES: g_showCodes = !g_showCodes; break;
    case ID_REVEAL:    g_reveal = !g_reveal; break;
    case ID_DRAWMODE:
        g_drawMode = !g_drawMode;
        g_message = g_drawMode ? "Draw mode: left button draws, right button erases"
                               : "Text mode";
        break;
    case ID_HELPKEYS:  ShowKeys(); break;
    }
    UpdateMenuChecks();
    Redraw();
}

/* ------------------------------------------------------------------ */
/*  Window procedure                                                   */
/* ------------------------------------------------------------------ */

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_COMMAND:
        Command(LOWORD(wp));
        return 0;

    case WM_TIMER:
        g_phase = !g_phase;
        Redraw();
        return 0;

    case WM_KEYDOWN: {
        if (g_promptStep) return 0;                 /* prompt uses WM_CHAR */
        bool ctrl = GetKeyState(VK_CONTROL) < 0;
        bool shift = GetKeyState(VK_SHIFT) < 0;
        if (ctrl) {
            switch (wp) {
            case 'N': Command(ID_NEW); break;
            case 'O': Command(shift ? ID_NETOPEN : ID_OPEN); break;
            case 'S': Command(shift ? ID_NETSAVE : ID_SAVE); break;
            case 'Z': Command(ID_UNDO); break;
            case 'Y': Command(ID_REDO); break;
            case 'P': Command(ID_SETTINGS); break;
            case 'G': Command(ID_DRAWMODE); break;
            case 'R': Command(ID_REVEAL); break;
            case 'H': Command(ID_HELPKEYS); break;
            case VK_INSERT: Command(ID_INSROW); break;
            case VK_DELETE: Command(ID_DELROW); break;
            }
            return 0;
        }
        if (wp >= VK_F1 && wp <= VK_F7) {
            int n = (int)(wp - VK_F1);
            g_message.clear();
            PutCell((unsigned char)(shift ? 0x11 + n : 0x01 + n));
            return 0;
        }
        switch (wp) {
        case VK_F8:  PutCell(shift ? 0x09 : 0x08); break;
        case VK_F9:  PutCell(shift ? 0x0C : 0x0D); break;
        case VK_F11: PutCell(shift ? 0x1C : 0x1D); break;
        case VK_F12: PutCell(shift ? 0x19 : 0x1A); break;
        case VK_LEFT:  if (g_col > 0) g_col--; break;
        case VK_RIGHT: if (g_col < COLS - 1) g_col++; break;
        case VK_UP:    if (g_row > 1) g_row--; break;
        case VK_DOWN:  if (g_row < ROWS - 1) g_row++; break;
        case VK_HOME:  g_col = 0; break;
        case VK_END: {
            int c = COLS - 1;
            while (c > 0 && Cur().cell[g_row][c] == ' ') c--;
            g_col = (c < COLS - 1 && Cur().cell[g_row][c] != ' ') ? c + 1 : c;
            break;
        }
        case VK_PRIOR: Command(ID_SUBPREV); break;
        case VK_NEXT:  Command(ID_SUBNEXT); break;
        case VK_DELETE:
            PushUndo();
            memmove(&Cur().cell[g_row][g_col], &Cur().cell[g_row][g_col + 1], COLS - 1 - g_col);
            Cur().cell[g_row][COLS - 1] = ' ';
            Changed();
            break;
        case VK_INSERT:
            PushUndo();
            memmove(&Cur().cell[g_row][g_col + 1], &Cur().cell[g_row][g_col], COLS - 1 - g_col);
            Cur().cell[g_row][g_col] = ' ';
            Changed();
            break;
        }
        g_phase = true;
        Redraw();
        return 0;
    }

    case WM_CHAR: {
        unsigned int ch = (unsigned int)wp;
        if (g_promptStep) {
            if (ch == 8) { if (!g_promptBuf.empty()) g_promptBuf.erase(g_promptBuf.size() - 1); }
            else if (ch == 13) PromptEnter();
            else if (ch == 27) {
                g_message = g_promptStep >= 10 ? "Cancelled" : "Page settings updated";
                g_promptStep = 0;
            }
            else if (ch >= 0x20 && ch < 0x7F && g_promptBuf.size() < 60) g_promptBuf += (char)ch;
            Redraw();
            return 0;
        }
        if (g_escPending) {
            g_escPending = false;
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            if (ch >= 0x40 && ch <= 0x5F && ch != 0x5B) PutCell((unsigned char)(ch - 0x40));
            else Redraw();
            return 0;
        }
        if (GetKeyState(VK_CONTROL) < 0 && ch < 0x20) return 0;   /* Ctrl+letter */
        g_message.clear();
        switch (ch) {
        case 27:
            g_escPending = true;
            g_message = "Esc: now type the code letter  (A-G text colour, Q-W graphics, "
                        "M double height, ] new background, H flash ...)";
            break;
        case 8:
            if (g_col > 0) {
                g_col--;
                PushUndo();
                Cur().cell[g_row][g_col] = ' ';
                Changed();
            }
            break;
        case 13:
            g_col = 0;
            if (g_row < ROWS - 1) g_row++;
            break;
        case 9:
            Command(ID_SHOWCODES);
            break;
        default:
            TypeChar(ch);
        }
        Redraw();
        return 0;
    }

    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN: {
        int row, col, sx, sy;
        if (!MouseToCell((short)LOWORD(lp), (short)HIWORD(lp), row, col, sx, sy)) return 0;
        if (row < 1) return 0;
        g_message.clear();
        if (g_drawMode) {
            PushUndo();
            g_mouseMode = (msg == WM_LBUTTONDOWN) ? 1 : 2;
            SetCapture(hwnd);
            SetSixel(row, col, sx, sy, g_mouseMode == 1);
            UpdateTitle();
        }
        g_row = row;
        g_col = col;
        Redraw();
        return 0;
    }

    case WM_MOUSEMOVE:
        if (g_mouseMode) {
            int row, col, sx, sy;
            if (MouseToCell((short)LOWORD(lp), (short)HIWORD(lp), row, col, sx, sy) && row >= 1) {
                SetSixel(row, col, sx, sy, g_mouseMode == 1);
                g_row = row;
                g_col = col;
                UpdateTitle();
                Redraw();
            }
        }
        return 0;

    case WM_LBUTTONUP:
    case WM_RBUTTONUP:
        if (g_mouseMode) { g_mouseMode = 0; ReleaseCapture(); }
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        Redraw();
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        RECT rc, v, s;
        GetClientRect(hwnd, &rc);
        GetLayout(v, s);

        /* draw everything off-screen first, then copy: no flicker */
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(mem, bmp);

        HBRUSH back = CreateSolidBrush(RGB(64, 64, 72));
        FillRect(mem, &rc, back);
        DeleteObject(back);

        RenderPage();
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth       = SCREEN_W;
        bi.bmiHeader.biHeight      = -SCREEN_H;
        bi.bmiHeader.biPlanes      = 1;
        bi.bmiHeader.biBitCount    = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(mem, COLORONCOLOR);     /* crisp pixels for editing */
        StretchDIBits(mem, v.left, v.top, v.right - v.left, v.bottom - v.top,
                      0, 0, SCREEN_W, SCREEN_H, g_pix, &bi, DIB_RGB_COLORS, SRCCOPY);
        PaintStatus(mem, s);

        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, oldBmp);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_CLOSE:
        if (ConfirmDiscard()) DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
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
    FILE *f = fopen("ttxedit.cfg", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char key[64], val[MAX_PATH];
        if (sscanf(line, " %63[^= ] = %259[^\r\n]", key, val) != 2) continue;
        if (!_stricmp(key, "server"))   { strncpy(g_server, val, sizeof(g_server) - 1); continue; }
        if (!_stricmp(key, "password")) { strncpy(g_password, val, sizeof(g_password) - 1); continue; }
        if (!_stricmp(key, "pagesdir")) {
            /* make it a full path so the file dialogs accept it */
            GetFullPathNameA(val, MAX_PATH, g_pagesDir, NULL);
        }
    }
    fclose(f);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow)
{
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    LoadConfig();
    BuildGlyphs();

    WNDCLASSA wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_CROSS);
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    wc.lpszClassName = "TtxEditWnd";
    RegisterClassA(&wc);

    BuildMenu();
    RECT r = { 0, 0, 900, 675 + STATUS_H + 16 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    g_hwnd = CreateWindowA("TtxEditWnd", "Teletext Editor", WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, g_menu, hInst, NULL);
    if (!g_hwnd) return 1;

    g_uiFont = CreateFontA(16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                           FIXED_PITCH | FF_MODERN, "Consolas");

    NewPage();
    if (__argc > 1) LoadTTI(__argv[1]);
    UpdateMenuChecks();

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);
    SetTimer(g_hwnd, TIMER_BLINK, 500, NULL);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    DeleteObject(g_uiFont);
    return 0;
}
