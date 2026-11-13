/*
 * ttxfeed.cpp  -  Turns RSS news feeds (e.g. BBC News) into Teletext pages
 * ------------------------------------------------------------------------
 * Every few minutes it downloads each feed listed in ttxfeed.cfg and writes
 * .tti pages into the server's pages folder:
 *
 *     page N        headlines for that section (up to 9 stories)
 *     pages N+1..9  one page per story: headline, summary, time
 *
 * ttxserver notices the changed files and pushes them to clients by itself.
 * A page file is only rewritten when the news has actually changed.
 *
 * ttxfeed.cfg  (next to the .exe; built-in defaults are used if missing)
 *     pagesdir=../server/pages        (just "pages" if run in the server's folder)
 *     interval=10                     minutes between updates
 *     source=BBC News                 shown at the foot of each page
 *     indexpage=100                   where the cyan Fastext key goes
 *     feed=110,TOP STORIES,https://feeds.bbci.co.uk/news/rss.xml
 *     feed=120,UK NEWS,https://feeds.bbci.co.uk/news/uk/rss.xml
 *     ...      (page,title,url - the page should end in 0, e.g. 110 120 300)
 *
 * Console keys:  U = update now   Q = quit
 *
 * Build:
 *   Dev-C++ : Console Application, Linker: -lwininet
 *   Linux   : g++ -O2 -o ttxfeed ttxfeed.cpp      (downloads with curl)
 */

#ifdef _WIN32
  #include <windows.h>
  #include <wininet.h>
  #include <conio.h>
#else
  #include <sys/types.h>
  #include <sys/stat.h>
  #include <sys/select.h>
  #include <unistd.h>
  #include <strings.h>
  #define _stricmp strcasecmp
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <vector>

#define ROWS 25
#define COLS 40

/* Teletext control codes as string pieces (keep them as separate literals) */
#define A_RED     "\x01"
#define A_GREEN   "\x02"
#define A_YELLOW  "\x03"
#define A_BLUE    "\x04"
#define A_CYAN    "\x06"
#define A_WHITE   "\x07"
#define DOUBLE_HEIGHT "\x0D"
#define NEW_BG    "\x1D"

struct Feed { int page; std::string title, url; };
struct Item { std::string title, desc, date; };
struct Page { unsigned char cell[ROWS][COLS]; };

static std::vector<Feed> g_feeds;
static std::string g_pagesDir  = "../server/pages";
static std::string g_source    = "BBC News";
static int         g_interval  = 10;
static int         g_indexPage = 0x100;

/* ------------------------------------------------------------------ */
/*  Windows / Linux differences                                        */
/* ------------------------------------------------------------------ */

static void SleepMs(int ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000);
#endif
}

static void MakeDir(const char *path)
{
#ifdef _WIN32
    CreateDirectoryA(path, NULL);
#else
    mkdir(path, 0755);
#endif
}

static bool ReplaceWith(const char *tmp, const char *dst)
{
#ifdef _WIN32
    return MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp, dst) == 0;
#endif
}

static bool KeyPressed(int &key)
{
#ifdef _WIN32
    if (!_kbhit()) return false;
    key = _getch();
    return true;
#else
    if (!isatty(0)) return false;
    fd_set rd;
    FD_ZERO(&rd);
    FD_SET(0, &rd);
    timeval tv = { 0, 0 };
    if (select(1, &rd, NULL, NULL, &tv) <= 0) return false;
    char c;
    if (read(0, &c, 1) != 1 || c == '\n' || c == '\r') return false;
    key = c;
    return true;
#endif
}

/* ------------------------------------------------------------------ */
/*  Settings                                                           */
/* ------------------------------------------------------------------ */

static void AddFeed(int page, const char *title, const char *url)
{
    Feed f;
    f.page = page;
    f.title = title;
    f.url = url;
    g_feeds.push_back(f);
}

static void LoadConfig()
{
    FILE *f = fopen("ttxfeed.cfg", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
            if (line[0] == '#' || line[0] == ';') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *key = line, *val = eq + 1;
            if (!_stricmp(key, "pagesdir"))       g_pagesDir = val;
            else if (!_stricmp(key, "source"))    g_source = val;
            else if (!_stricmp(key, "interval"))  g_interval = atoi(val);
            else if (!_stricmp(key, "indexpage")) g_indexPage = (int)strtol(val, NULL, 16);
            else if (!_stricmp(key, "feed")) {
                /* page,title,url */
                std::string v(val);
                size_t c1 = v.find(','), c2 = (c1 == std::string::npos) ? c1 : v.find(',', c1 + 1);
                if (c2 == std::string::npos) continue;
                int page = (int)strtol(v.substr(0, c1).c_str(), NULL, 16);
                if (page < 0x100 || page > 0x8F0) continue;
                AddFeed(page, v.substr(c1 + 1, c2 - c1 - 1).c_str(), v.substr(c2 + 1).c_str());
            }
        }
        fclose(f);
    }
    if (g_interval < 2) g_interval = 2;          /* be polite to the feed server */
    if (g_feeds.empty()) {
        AddFeed(0x110, "TOP STORIES", "https://feeds.bbci.co.uk/news/rss.xml");
        AddFeed(0x120, "UK NEWS",     "https://feeds.bbci.co.uk/news/uk/rss.xml");
        AddFeed(0x130, "WORLD NEWS",  "https://feeds.bbci.co.uk/news/world/rss.xml");
        AddFeed(0x140, "BUSINESS",    "https://feeds.bbci.co.uk/news/business/rss.xml");
        AddFeed(0x150, "TECHNOLOGY",  "https://feeds.bbci.co.uk/news/technology/rss.xml");
        AddFeed(0x300, "SPORT",       "https://feeds.bbci.co.uk/sport/rss.xml");
        AddFeed(0x360, "FORMULA 1",   "https://feeds.bbci.co.uk/sport/formula1/rss.xml");
    }
}

/* ------------------------------------------------------------------ */
/*  Downloading                                                        */
/* ------------------------------------------------------------------ */

static bool Fetch(const std::string &url, std::string &out)
{
    out.clear();
    /* anything that isn't http(s) is treated as a local file - handy for testing */
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        FILE *f = fopen(url.c_str(), "rb");
        if (!f) return false;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return !out.empty();
    }

#ifndef _WIN32
    /* Linux: let curl do the work */
    if (url.find('\'') != std::string::npos) return false;
    std::string cmd = "curl -sL --max-time 30 -A ttxfeed/1.0 '" + url + "'";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return false;
    char cbuf[8192];
    size_t cn;
    while ((cn = fread(cbuf, 1, sizeof(cbuf), p)) > 0) out.append(cbuf, cn);
    if (pclose(p) != 0) { printf("   download failed (is curl installed?)\n"); return false; }
    return !out.empty();
#else
    HINTERNET net = InternetOpenA("ttxfeed/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return false;
    HINTERNET h = InternetOpenUrlA(net, url.c_str(), NULL, 0,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                   INTERNET_FLAG_NO_UI, 0);
    if (!h) {
        printf("   download failed (error %lu)\n", GetLastError());
        InternetCloseHandle(net);
        return false;
    }
    char buf[8192];
    DWORD n = 0;
    while (InternetReadFile(h, buf, sizeof(buf), &n) && n > 0) out.append(buf, n);
    InternetCloseHandle(h);
    InternetCloseHandle(net);
    return !out.empty();
#endif
}

/* ------------------------------------------------------------------ */
/*  RSS parsing                                                        */
/* ------------------------------------------------------------------ */

/* Text between <tag ...> and </tag>, searching s[from..to) */
static std::string TagText(const std::string &s, size_t from, size_t to, const char *tag)
{
    std::string open = std::string("<") + tag;
    size_t a = from;
    for (;;) {
        a = s.find(open, a);
        if (a == std::string::npos || a >= to) return "";
        char next = s[a + open.size()];
        if (next == '>' || next == ' ') break;          /* not <titleXYZ */
        a++;
    }
    size_t gt = s.find('>', a);
    if (gt == std::string::npos || gt >= to || s[gt - 1] == '/') return "";
    std::string close = std::string("</") + tag + ">";
    size_t b = s.find(close, gt);
    if (b == std::string::npos || b > to) return "";
    return s.substr(gt + 1, b - gt - 1);
}

static void ReplaceAll(std::string &s, const std::string &a, const std::string &b)
{
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
}

/* Append a Unicode code point as UTF-8 */
static void PutUtf8(std::string &out, unsigned long cp)
{
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

/* Remove CDATA wrappers and HTML tags, decode &entities; */
static std::string CleanXml(std::string s)
{
    ReplaceAll(s, "<![CDATA[", "");
    ReplaceAll(s, "]]>", "");

    std::string t;
    bool inTag = false;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '<') inTag = true;
        else if (s[i] == '>' && inTag) { inTag = false; t += ' '; }
        else if (!inTag) t += s[i];
    }

    std::string out;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] != '&') { out += t[i]; continue; }
        size_t semi = t.find(';', i);
        if (semi == std::string::npos || semi - i > 10) { out += '&'; continue; }
        std::string e = t.substr(i + 1, semi - i - 1);
        if (e == "amp") out += '&';
        else if (e == "lt") out += '<';
        else if (e == "gt") out += '>';
        else if (e == "quot") out += '"';
        else if (e == "apos") out += '\'';
        else if (e == "nbsp") out += ' ';
        else if (e == "pound") PutUtf8(out, 0xA3);
        else if (!e.empty() && e[0] == '#') {
            unsigned long cp = (e.size() > 1 && (e[1] == 'x' || e[1] == 'X'))
                               ? strtoul(e.c_str() + 2, NULL, 16)
                               : strtoul(e.c_str() + 1, NULL, 10);
            PutUtf8(out, cp);
        }
        else { out += '&'; continue; }               /* unknown: leave as is */
        i = semi;
    }
    return out;
}

/* UTF-8 text -> printable Teletext (UK character set) */
static std::string ToTeletext(const std::string &s)
{
    /* Latin-1 letters U+00C0..U+00FF without their accents */
    static const char *latin = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        unsigned long cp;
        int len = 1;
        if (c < 0x80)                { cp = c; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else                         { cp = '?'; }
        for (int k = 1; k < len && i + k < s.size(); k++)
            cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += len;

        switch (cp) {
        case '\n': case '\r': case '\t': case 0xA0: out += ' '; continue;
        case '#':  out += (char)0x5F; continue;        /* # is at 0x5F in the UK set */
        case 0xA3: out += (char)0x23; continue;        /* pound sign */
        case '[': case '{': out += '('; continue;
        case ']': case '}': out += ')'; continue;
        case '\\': case '|': out += '/'; continue;
        case '_': case '~': out += '-'; continue;
        case '^': out += ' '; continue;
        case '`': case 0x2018: case 0x2019: case 0x201A: case 0x2032: out += '\''; continue;
        case 0x201C: case 0x201D: case 0x201E: out += '"'; continue;
        case 0x2013: case 0x2014: case 0x2212: out += '-'; continue;
        case 0x2026: out += "..."; continue;
        case 0x20AC: out += "EUR"; continue;
        case 0xBD: out += (char)0x5C; continue;        /* 1/2 */
        case 0xBC: out += (char)0x7B; continue;        /* 1/4 */
        case 0xBE: out += (char)0x7D; continue;        /* 3/4 */
        }
        if (cp >= 0x20 && cp < 0x7F) out += (char)cp;
        else if (cp >= 0xC0 && cp <= 0xFF) out += latin[cp - 0xC0];
        /* anything else is dropped */
    }

    /* tidy spaces */
    std::string t;
    for (size_t k = 0; k < out.size(); k++) {
        if (out[k] == ' ' && (t.empty() || t[t.size() - 1] == ' ')) continue;
        t += out[k];
    }
    while (!t.empty() && t[t.size() - 1] == ' ') t.erase(t.size() - 1);
    return t;
}

static void ParseRss(const std::string &xml, std::vector<Item> &items, size_t maxItems)
{
    items.clear();
    size_t p = 0;
    while (items.size() < maxItems) {
        size_t a = xml.find("<item", p);
        if (a == std::string::npos) break;
        size_t b = xml.find("</item>", a);
        if (b == std::string::npos) break;
        Item it;
        it.title = ToTeletext(CleanXml(TagText(xml, a, b, "title")));
        it.desc  = ToTeletext(CleanXml(TagText(xml, a, b, "description")));
        it.date  = CleanXml(TagText(xml, a, b, "pubDate"));
        if (!it.title.empty()) items.push_back(it);
        p = b + 7;
    }
}

/* "Fri, 13 Nov 2026 12:05:23 GMT" -> "12:05 13 Nov" */
static std::string ShortDate(const std::string &d)
{
    int day = 0, year = 0, hh = 0, mm = 0;
    char mon[8] = "";
    if (sscanf(d.c_str(), "%*3s, %d %3s %d %d:%d", &day, mon, &year, &hh, &mm) == 5) {
        char buf[32];
        sprintf(buf, "%02d:%02d %d %s", hh, mm, day, mon);
        return buf;
    }
    return "";
}

/* ------------------------------------------------------------------ */
/*  Building pages                                                     */
/* ------------------------------------------------------------------ */

static void Blank(Page &p) { memset(p.cell, ' ', sizeof(p.cell)); }

static void Put(Page &p, int row, int col, const std::string &s)
{
    for (size_t i = 0; i < s.size() && col < COLS; i++)
        p.cell[row][col++] = (unsigned char)(s[i] & 0x7F);
}

static std::vector<std::string> Wrap(const std::string &text, int width)
{
    std::vector<std::string> lines;
    std::string line;
    size_t i = 0;
    while (i < text.size()) {
        size_t sp = text.find(' ', i);
        if (sp == std::string::npos) sp = text.size();
        std::string word = text.substr(i, sp - i);
        i = sp + 1;
        while ((int)word.size() > width) {                 /* very long word */
            if (!line.empty()) { lines.push_back(line); line.clear(); }
            lines.push_back(word.substr(0, width));
            word = word.substr(width);
        }
        if (line.empty()) line = word;
        else if ((int)(line.size() + 1 + word.size()) <= width) line += " " + word;
        else { lines.push_back(line); line = word; }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

static std::string Upper(const std::string &s)
{
    std::string t(s);
    for (size_t i = 0; i < t.size(); i++)
        if (t[i] >= 'a' && t[i] <= 'z') t[i] = (char)(t[i] - 32);
    return t;
}

static void Banner(Page &p, const std::string &title)
{
    Put(p, 1, 0, A_BLUE NEW_BG DOUBLE_HEIGHT A_YELLOW);
    Put(p, 1, 5, Upper(title).substr(0, 34));
}

static std::string Hex3(int n)
{
    char b[8];
    sprintf(b, "%03X", n);
    return b;
}

/* Headlines page for one feed */
static void BuildIndex(Page &p, const Feed &f, const std::vector<Item> &items, int nextFeed, int prevFeed)
{
    Blank(p);
    Banner(p, f.title);
    if (!items.empty()) {
        std::string when = ShortDate(items[0].date);
        if (!when.empty()) Put(p, 3, 0, A_CYAN "Latest: " + when);
    }
    int row = 5;
    for (size_t i = 0; i < items.size() && row <= 21; i++) {
        std::vector<std::string> l = Wrap(items[i].title, 34);
        if (l.size() > 2) {                                /* keep to 2 lines */
            l.resize(2);
            if (l[1].size() > 31) l[1] = l[1].substr(0, 31);
            l[1] += "...";
        }
        if (row + (int)l.size() - 1 > 21) break;
        for (size_t k = 0; k < l.size(); k++) {
            Put(p, row, 0, (i == 0 ? A_YELLOW : A_WHITE) + l[k]);
            if (k == 0) Put(p, row, 36, A_CYAN + Hex3(f.page + 1 + (int)i));
            row++;
        }
    }
    if (items.empty()) Put(p, 8, 0, A_RED "News is not available at the moment");
    Put(p, 23, 0, A_GREEN "Source: " + g_source);
    Put(p, 24, 0, A_RED "Top story  " A_GREEN "Next topic  " A_YELLOW "Back  " A_CYAN "Index");
    (void)nextFeed; (void)prevFeed;
}

/* One story page */
static void BuildStory(Page &p, const Feed &f, const std::vector<Item> &items, size_t i)
{
    Blank(p);
    Banner(p, f.title);
    char buf[48];
    sprintf(buf, A_CYAN "Story %d of %d", (int)i + 1, (int)items.size());
    Put(p, 3, 0, buf);

    int row = 5;
    std::vector<std::string> h = Wrap(items[i].title, 39);
    for (size_t k = 0; k < h.size() && row <= 8; k++) Put(p, row++, 0, A_YELLOW + h[k]);
    row++;
    std::vector<std::string> d = Wrap(items[i].desc, 39);
    for (size_t k = 0; k < d.size() && row <= 20; k++) Put(p, row++, 0, A_WHITE + d[k]);

    std::string when = ShortDate(items[i].date);
    if (!when.empty()) Put(p, 22, 0, A_CYAN "Published " + when);
    Put(p, 23, 0, A_GREEN "Source: " + g_source);
    Put(p, 24, 0, A_RED "Next story " A_GREEN "Headlines " A_YELLOW "Topic " A_CYAN "Index");
}

/* ------------------------------------------------------------------ */
/*  Writing .tti files                                                 */
/* ------------------------------------------------------------------ */

static std::string PagePath(int page)
{
    return g_pagesDir + "/P" + Hex3(page) + ".tti";
}

static std::string MakeTTI(const Page &p, int number, const std::string &desc, const int links[5])
{
    std::string s;
    char buf[128];
    s += "DE," + desc + "\r\n";
    sprintf(buf, "PN,%03X00\r\nSC,0000\r\nPS,8000\r\n", number);
    s += buf;
    for (int r = 1; r < ROWS; r++) {
        int len = COLS;
        while (len > 0 && p.cell[r][len - 1] == ' ') len--;
        if (!len) continue;
        sprintf(buf, "OL,%d,", r);
        s += buf;
        for (int c = 0; c < len; c++) {
            unsigned char ch = p.cell[r][c] & 0x7F;
            if (ch < 0x20) { s += (char)0x1B; s += (char)(ch + 0x40); }
            else s += (char)ch;
        }
        s += "\r\n";
    }
    int l[5];
    for (int i = 0; i < 5; i++) l[i] = links[i] ? links[i] : 0x8FF;
    sprintf(buf, "FL,%03X,%03X,%03X,%03X,8FF,%03X\r\n", l[0], l[1], l[2], l[3], l[4]);
    s += buf;
    return s;
}

/* Write only if different, via a temporary file so the server never
 * reads a half-written page. Returns true if the file changed. */
static bool WriteIfChanged(int number, const std::string &content)
{
    std::string path = PagePath(number);
    FILE *f = fopen(path.c_str(), "rb");
    if (f) {
        std::string old;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) old.append(buf, n);
        fclose(f);
        if (old == content) return false;
    }
    std::string tmp = g_pagesDir + "/~P" + Hex3(number) + ".tmp";
    f = fopen(tmp.c_str(), "wb");
    if (!f) { printf("   cannot write %s\n", tmp.c_str()); return false; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (!ReplaceWith(tmp.c_str(), path.c_str())) {
        printf("   cannot replace %s\n", path.c_str());
        remove(tmp.c_str());
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Update cycle                                                       */
/* ------------------------------------------------------------------ */

static void UpdateFeed(size_t fi)
{
    const Feed &f = g_feeds[fi];
    int nextFeed = g_feeds[(fi + 1) % g_feeds.size()].page;
    int prevFeed = g_feeds[(fi + g_feeds.size() - 1) % g_feeds.size()].page;

    printf("%03X %-14s ", f.page, f.title.c_str());
    std::string xml;
    if (!Fetch(f.url, xml)) {
        printf("   no data - keeping the old pages\n");
        return;
    }
    std::vector<Item> items;
    ParseRss(xml, items, 9);
    if (items.empty()) {
        printf("   no stories found - keeping the old pages\n");
        return;
    }

    int changed = 0;
    Page p;
    int links[5];

    BuildIndex(p, f, items, nextFeed, prevFeed);
    links[0] = f.page + 1; links[1] = nextFeed; links[2] = prevFeed;
    links[3] = g_indexPage; links[4] = g_indexPage;
    if (WriteIfChanged(f.page, MakeTTI(p, f.page, f.title + " headlines", links))) changed++;

    for (size_t i = 0; i < items.size(); i++) {
        int num = f.page + 1 + (int)i;
        BuildStory(p, f, items, i);
        links[0] = (i + 1 < items.size()) ? num + 1 : f.page;
        links[1] = f.page; links[2] = nextFeed;
        links[3] = g_indexPage; links[4] = g_indexPage;
        if (WriteIfChanged(num, MakeTTI(p, num, f.title + " story", links))) changed++;
    }
    /* remove story pages left over from a longer feed */
    for (size_t i = items.size(); i < 9; i++)
        remove(PagePath(f.page + 1 + (int)i).c_str());

    printf("%2d stories, %d page(s) updated\n", (int)items.size(), changed);
}

static void UpdateAll()
{
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    printf("\n--- Updating at %02d:%02d ---\n", tm->tm_hour, tm->tm_min);
    for (size_t i = 0; i < g_feeds.size(); i++) UpdateFeed(i);
    printf("Next update in %d minutes.  U = update now, Q = quit\n", g_interval);
}

int main()
{
    LoadConfig();
    setvbuf(stdout, NULL, _IOLBF, 1024);
    MakeDir(g_pagesDir.c_str());

    printf("=== Teletext news feed ===\n");
    printf("Writing pages to %s\n", g_pagesDir.c_str());
    for (size_t i = 0; i < g_feeds.size(); i++)
        printf("   %03X  %-14s %s\n", g_feeds[i].page, g_feeds[i].title.c_str(),
               g_feeds[i].url.c_str());

    for (;;) {
        UpdateAll();
        time_t until = time(NULL) + (time_t)g_interval * 60;
        bool now = false;
        while (time(NULL) < until && !now) {
            SleepMs(200);
            int k;
            while (KeyPressed(k)) {
                if (k == 'q' || k == 'Q') return 0;
                if (k == 'u' || k == 'U') now = true;
            }
        }
    }
}
