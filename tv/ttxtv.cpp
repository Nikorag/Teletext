/*
 * ttxtv.cpp  -  TV listings pages for ttxserver
 * ----------------------------------------------------------------
 * Downloads UK Freeview listings (an XMLTV file from the open-source
 * Freeview-EPG project, for personal use) and writes Ceefax-style pages:
 *
 *     index page     Now and Next on every channel        (600)
 *     one page each  the channel's listings from now on   (601-609)
 *
 * The listings file is downloaded every few hours; the pages are rebuilt
 * every few minutes from the saved copy so "now" keeps moving.
 *
 * ttxtv.cfg (next to the program):
 *     pagesdir=../server/pages
 *     url=https://raw.githubusercontent.com/dp247/Freeview-EPG/master/epg.xml
 *     download=240                 minutes between downloads
 *     refresh=5                    minutes between page rebuilds
 *     indexpage=600
 *     channel=BBCOneSouth.uk,BBC ONE,601      listings id,name,page
 *
 * Console keys: U = download and update now   Q = quit
 *
 * Build:
 *   Dev-C++ : Console Application, Linker: -lwininet
 *   Linux   : g++ -O2 -o ttxtv ttxtv.cpp      (needs curl)
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
#include <ctype.h>
#include <time.h>
#include <string>
#include <vector>
#include <map>
#include <algorithm>

#define ROWS 25
#define COLS 40
#define LINES_PER_SUB 17       /* listing lines per subpage (rows 4-20) */
#define MAX_SUBS 3
#define PER_INDEX_SUB 8        /* channels per Now and Next subpage */

#define A_RED     "\x01"
#define A_GREEN   "\x02"
#define A_YELLOW  "\x03"
#define A_BLUE    "\x04"
#define A_MAGENTA "\x05"
#define A_CYAN    "\x06"
#define A_WHITE   "\x07"
#define DOUBLE_HEIGHT "\x0D"
#define NEW_BG    "\x1D"

struct Channel { std::string id, name; int page; };
struct Prog    { time_t start, stop; std::string title; };
struct Page    { unsigned char cell[ROWS][COLS]; };

static std::vector<Channel> g_channels;
static std::map<std::string, std::vector<Prog> > g_sched;
static std::string g_pagesDir  = "../server/pages";
static std::string g_url       = "https://raw.githubusercontent.com/dp247/Freeview-EPG/master/epg.xml";
static std::string g_saved     = "epg.xml";
static int g_download  = 240;
static int g_refresh   = 5;
static int g_indexPage = 0x600;
static time_t g_loadedAt = 0;      /* when the saved listings were downloaded */

/* ------------------------------------------------------------------ */
/*  Windows / Linux differences, page helpers                          */
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

static void Blank(Page &p) { memset(p.cell, ' ', sizeof(p.cell)); }

static void Put(Page &p, int row, int col, const std::string &s)
{
    for (size_t i = 0; i < s.size() && col < COLS; i++)
        p.cell[row][col++] = (unsigned char)(s[i] & 0x7F);
}

static std::string Hex3(int n)
{
    char b[8];
    sprintf(b, "%03X", n);
    return b;
}

static std::string Fit(const std::string &s, size_t w)
{
    std::string t = s.substr(0, w);
    while (t.size() < w) t += ' ';
    return t;
}

static long DaysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static std::string MakeTTI(const std::vector<Page> &subs, int number, const std::string &desc,
                           const int links[5], int cycle)
{
    std::string s;
    char buf[128];
    s += "DE," + desc + "\r\n";
    sprintf(buf, "CT,%d,T\r\n", cycle);
    s += buf;
    bool multi = subs.size() > 1;
    for (size_t sp = 0; sp < subs.size(); sp++) {
        sprintf(buf, "PN,%03X%02d\r\nSC,%04d\r\nPS,8000\r\n",
                number, multi ? (int)sp + 1 : 0, multi ? (int)sp + 1 : 0);
        s += buf;
        for (int r = 1; r < ROWS; r++) {
            int len = COLS;
            while (len > 0 && subs[sp].cell[r][len - 1] == ' ') len--;
            if (!len) continue;
            sprintf(buf, "OL,%d,", r);
            s += buf;
            for (int c = 0; c < len; c++) {
                unsigned char ch = subs[sp].cell[r][c] & 0x7F;
                if (ch < 0x20) { s += (char)0x1B; s += (char)(ch + 0x40); }
                else s += (char)ch;
            }
            s += "\r\n";
        }
    }
    int l[5];
    for (int i = 0; i < 5; i++) l[i] = links[i] ? links[i] : 0x8FF;
    sprintf(buf, "FL,%03X,%03X,%03X,%03X,8FF,%03X\r\n", l[0], l[1], l[2], l[3], l[4]);
    s += buf;
    return s;
}

static bool WriteIfChanged(int number, const std::string &content)
{
    std::string path = g_pagesDir + "/P" + Hex3(number) + ".tti";
    FILE *f = fopen(path.c_str(), "rb");
    if (f) {
        std::string old;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) old.append(buf, n);
        fclose(f);
        if (old == content) return false;
    }
    std::string tmp = g_pagesDir + "/~T" + Hex3(number) + ".tmp";
    f = fopen(tmp.c_str(), "wb");
    if (!f) { printf("   cannot write %s\n", tmp.c_str()); return false; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (!ReplaceWith(tmp.c_str(), path.c_str())) { remove(tmp.c_str()); return false; }
    return true;
}

/* ------------------------------------------------------------------ */
/*  Settings                                                           */
/* ------------------------------------------------------------------ */

static void AddChannel(const char *id, const char *name, int page)
{
    Channel c;
    c.id = id; c.name = name; c.page = page;
    g_channels.push_back(c);
}

static void LoadConfig()
{
    FILE *f = fopen("ttxtv.cfg", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            size_t n = strlen(line);
            while (n && (line[n - 1] == '\n' || line[n - 1] == '\r' || line[n - 1] == ' ')) line[--n] = 0;
            if (line[0] == '#' || line[0] == ';') continue;
            char *eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            const char *key = line, *val = eq + 1;
            if (!_stricmp(key, "pagesdir"))       g_pagesDir = val;
            else if (!_stricmp(key, "url"))       g_url = val;
            else if (!_stricmp(key, "download"))  g_download = atoi(val);
            else if (!_stricmp(key, "refresh"))   g_refresh = atoi(val);
            else if (!_stricmp(key, "indexpage")) g_indexPage = (int)strtol(val, NULL, 16);
            else if (!_stricmp(key, "channel")) {
                char id[96], name[64], pg[16];
                if (sscanf(val, "%95[^,],%63[^,],%15s", id, name, pg) == 3) {
                    int page = (int)strtol(pg, NULL, 16);
                    if (page >= 0x100 && page <= 0x8FF) AddChannel(id, name, page);
                }
            }
        }
        fclose(f);
    }
    if (g_channels.empty()) {
        AddChannel("BBCOneSouth.uk",   "BBC ONE",   0x601);
        AddChannel("BBCTwoHD.uk",      "BBC TWO",   0x602);
        AddChannel("ITV1MeridianS.uk", "ITV1",      0x603);
        AddChannel("Channel4South.uk", "CHANNEL 4", 0x604);
        AddChannel("5.uk",             "CHANNEL 5", 0x605);
        AddChannel("BBCFourHD.uk",     "BBC FOUR",  0x606);
        AddChannel("BBCNews.uk",       "BBC NEWS",  0x607);
        AddChannel("ITV2.uk",          "ITV2",      0x608);
        AddChannel("E4.uk",            "E4",        0x609);
    }
    if (g_download < 60) g_download = 60;      /* the file only changes every 8 hours */
    if (g_refresh < 1) g_refresh = 1;
}

/* ------------------------------------------------------------------ */
/*  Downloading - straight to the file epg.xml                         */
/* ------------------------------------------------------------------ */

static bool Download()
{
    std::string tmp = g_saved + ".part";
#ifdef _WIN32
    HINTERNET net = InternetOpenA("ttxtv/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return false;
    HINTERNET h = InternetOpenUrlA(net, g_url.c_str(), NULL, 0,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                   INTERNET_FLAG_NO_UI, 0);
    if (!h) { InternetCloseHandle(net); return false; }
    DWORD status = 0, len = sizeof(status);
    HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL);
    FILE *f = (status == 200) ? fopen(tmp.c_str(), "wb") : NULL;
    bool ok = f != NULL;
    char buf[16384];
    DWORD n = 0;
    while (ok && InternetReadFile(h, buf, sizeof(buf), &n) && n > 0)
        ok = fwrite(buf, 1, n, f) == n;
    if (f) fclose(f);
    InternetCloseHandle(h);
    InternetCloseHandle(net);
#else
    if (g_url.find('\'') != std::string::npos) return false;
    std::string cmd = "curl -sSfL --max-time 300 -A ttxtv/1.0 -o '" + tmp + "' '" + g_url + "'";
    bool ok = system(cmd.c_str()) == 0;
#endif
    if (!ok) { remove(tmp.c_str()); return false; }
    return ReplaceWith(tmp.c_str(), g_saved.c_str());
}

/* ------------------------------------------------------------------ */
/*  Reading the XMLTV file                                             */
/* ------------------------------------------------------------------ */

static std::string Attr(const std::string &tag, const char *name)
{
    std::string key = std::string(" ") + name + "=\"";
    size_t p = tag.find(key);
    if (p == std::string::npos) return "";
    p += key.size();
    size_t e = tag.find('"', p);
    return e == std::string::npos ? "" : tag.substr(p, e - p);
}

/* "20261113220000 +0000" -> time_t */
static time_t XmlTime(const std::string &s)
{
    int Y, M, D, h, m, sec;
    if (sscanf(s.c_str(), "%4d%2d%2d%2d%2d%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
    long off = 0;
    size_t z = s.find_first_of("+-", 14);
    if (z != std::string::npos) {
        int v = atoi(s.c_str() + z + 1);
        off = ((v / 100) * 60 + v % 100) * 60L * (s[z] == '-' ? -1 : 1);
    }
    return (time_t)(DaysFromCivil(Y, M, D) * 86400L + h * 3600L + m * 60L + sec - off);
}

static void PutUtf8(std::string &out, unsigned long cp)
{
    if (cp < 0x80) out += (char)cp;
    else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
    else {
        out += (char)(0xE0 | (cp >> 12));
        out += (char)(0x80 | ((cp >> 6) & 0x3F));
        out += (char)(0x80 | (cp & 0x3F));
    }
}

static std::string DecodeEntities(const std::string &t)
{
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
        else if (!e.empty() && e[0] == '#')
            PutUtf8(out, (e.size() > 1 && (e[1] == 'x' || e[1] == 'X'))
                         ? strtoul(e.c_str() + 2, NULL, 16) : strtoul(e.c_str() + 1, NULL, 10));
        else { out += '&'; continue; }
        i = semi;
    }
    return out;
}

/* UTF-8 text -> printable Teletext (UK character set) */
static std::string ToTeletext(const std::string &s)
{
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
        if (cp == '#') out += (char)0x5F;
        else if (cp == 0xA3) out += (char)0x23;
        else if (cp == '[' || cp == '{') out += '(';
        else if (cp == ']' || cp == '}') out += ')';
        else if (cp == '_' || cp == '~' || cp == 0x2013 || cp == 0x2014) out += '-';
        else if (cp == 0x2018 || cp == 0x2019 || cp == '`') out += '\'';
        else if (cp == 0x201C || cp == 0x201D) out += '"';
        else if (cp == 0x2026) out += "...";
        else if (cp >= 0x20 && cp < 0x7F && cp != '\\' && cp != '|' && cp != '^') out += (char)cp;
        else if (cp >= 0xC0 && cp <= 0xFF) out += latin[cp - 0xC0];
        else if (cp == '\t' || cp == '\n' || cp == 0xA0) out += ' ';
    }
    return out;
}

static bool ByStart(const Prog &a, const Prog &b) { return a.start < b.start; }

static bool LoadListings()
{
    FILE *f = fopen(g_saved.c_str(), "rb");
    if (!f) return false;
    std::string x;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) x.append(buf, n);
    fclose(f);

    std::map<std::string, bool> wanted;
    for (size_t i = 0; i < g_channels.size(); i++) wanted[g_channels[i].id] = true;

    g_sched.clear();
    size_t p = 0;
    while ((p = x.find("<programme ", p)) != std::string::npos) {
        size_t gt = x.find('>', p);
        if (gt == std::string::npos) break;
        size_t end = x.find("</programme>", gt);
        if (end == std::string::npos) break;
        std::string tag = x.substr(p, gt - p);
        std::string ch = Attr(tag, "channel");
        if (wanted.count(ch)) {
            Prog pr;
            pr.start = XmlTime(Attr(tag, "start"));
            pr.stop = XmlTime(Attr(tag, "stop"));
            size_t t1 = x.find("<title", gt);
            if (t1 != std::string::npos && t1 < end) {
                size_t t2 = x.find('>', t1), t3 = x.find("</title>", t1);
                if (t2 != std::string::npos && t3 != std::string::npos && t3 < end)
                    pr.title = ToTeletext(DecodeEntities(x.substr(t2 + 1, t3 - t2 - 1)));
            }
            if (pr.start && !pr.title.empty()) g_sched[ch].push_back(pr);
        }
        p = end + 12;
    }
    std::map<std::string, std::vector<Prog> >::iterator it;
    for (it = g_sched.begin(); it != g_sched.end(); ++it)
        std::sort(it->second.begin(), it->second.end(), ByStart);
    return true;
}

/* ------------------------------------------------------------------ */
/*  Pages                                                              */
/* ------------------------------------------------------------------ */

static std::string HHMM(time_t t)
{
    char b[8];
    strftime(b, sizeof(b), "%H:%M", localtime(&t));
    return b;
}

static std::string DayKey(time_t t)
{
    char b[16];
    strftime(b, sizeof(b), "%Y%m%d", localtime(&t));
    return b;
}

static std::string DayTitle(time_t t)
{
    char b[32];
    strftime(b, sizeof(b), "%A %d %B", localtime(&t));
    return b;
}

static void Banner(Page &p, const std::string &title)
{
    Put(p, 1, 0, A_BLUE NEW_BG DOUBLE_HEIGHT A_YELLOW);
    Put(p, 1, 4, title.substr(0, 35));
}

static void Footer(Page &p)
{
    std::string when = g_loadedAt ? HHMM(g_loadedAt) : "--:--";
    Put(p, 22, 0, A_GREEN "Listings downloaded " + when);
    Put(p, 23, 0, A_WHITE "Listings: Freeview-EPG (personal use)");
}

/* index of the programme on now (or the next one), -1 if none */
static int NowIndex(const std::vector<Prog> &v, time_t now)
{
    for (size_t i = 0; i < v.size(); i++)
        if (v[i].stop > now || (!v[i].stop && v[i].start >= now)) return (int)i;
    return -1;
}

static void BuildChannel(std::vector<Page> &subs, size_t ci, time_t now)
{
    const Channel &c = g_channels[ci];
    const std::vector<Prog> &v = g_sched[c.id];
    subs.clear();

    /* the lines: programmes from now on, with a day heading when the date changes */
    std::vector<std::string> lines;
    int first = NowIndex(v, now);
    std::string day = DayKey(now);
    for (int i = first; i >= 0 && i < (int)v.size(); i++) {
        if ((int)lines.size() >= LINES_PER_SUB * MAX_SUBS) break;
        std::string d = DayKey(v[i].start);
        if (d != day && v[i].start > now) {
            day = d;
            lines.push_back(std::string(A_GREEN) + DayTitle(v[i].start));
        }
        bool onNow = v[i].start <= now;
        std::string line = std::string(onNow ? A_YELLOW : A_CYAN) + HHMM(v[i].start);
        line += onNow ? A_YELLOW : A_WHITE;
        line += v[i].title.substr(0, 33);
        lines.push_back(line);
    }

    size_t nsub = lines.empty() ? 1 : (lines.size() + LINES_PER_SUB - 1) / LINES_PER_SUB;
    for (size_t s = 0; s < nsub; s++) {
        Page p;
        Blank(p);
        Banner(p, c.name);
        Put(p, 3, 0, A_CYAN + DayTitle(now));
        if (nsub > 1) {
            char b[16];
            sprintf(b, "%d/%d", (int)s + 1, (int)nsub);
            Put(p, 1, 36, b);
        }
        for (size_t k = 0; k < LINES_PER_SUB && s * LINES_PER_SUB + k < lines.size(); k++)
            Put(p, 4 + (int)k, 0, lines[s * LINES_PER_SUB + k]);
        if (lines.empty()) Put(p, 8, 0, A_WHITE "No listings for this channel yet");
        Footer(p);
        Put(p, 24, 0, A_RED "Next channel " A_GREEN "Previous " A_YELLOW "Now/Next " A_CYAN "Index");
        subs.push_back(p);
    }
}

static void BuildIndex(std::vector<Page> &subs, time_t now)
{
    subs.clear();
    size_t n = g_channels.size();
    size_t nsub = (n + PER_INDEX_SUB - 1) / PER_INDEX_SUB;
    if (!nsub) nsub = 1;
    for (size_t s = 0; s < nsub; s++) {
        Page p;
        Blank(p);
        Banner(p, "TV NOW AND NEXT");
        Put(p, 3, 0, A_CYAN + DayTitle(now) + "  " + HHMM(now));
        if (nsub > 1) {
            char b[16];
            sprintf(b, "%d/%d", (int)s + 1, (int)nsub);
            Put(p, 1, 36, b);
        }
        int row = 5;
        for (size_t i = s * PER_INDEX_SUB; i < n && i < (s + 1) * PER_INDEX_SUB; i++) {
            const Channel &c = g_channels[i];
            const std::vector<Prog> &v = g_sched[c.id];
            int k = NowIndex(v, now);
            std::string nowT = k >= 0 ? v[k].title : "";
            Put(p, row, 0, A_YELLOW + Fit(c.name, 10) + A_WHITE + Fit(nowT, 24) + A_CYAN + Hex3(c.page));
            if (k >= 0 && k + 1 < (int)v.size())
                Put(p, row + 1, 0, A_CYAN + Fit(" " + HHMM(v[k + 1].start), 10) + A_WHITE +
                                   Fit(v[k + 1].title, 28));
            row += 2;
        }
        Footer(p);
        Put(p, 24, 0, A_RED "First channel " A_CYAN "Index");
        subs.push_back(p);
    }
}

static void BuildAll()
{
    time_t now = time(NULL);
    size_t n = g_channels.size();
    int changed = 0;
    for (size_t i = 0; i < n; i++) {
        std::vector<Page> subs;
        BuildChannel(subs, i, now);
        int links[5];
        links[0] = g_channels[(i + 1) % n].page;
        links[1] = g_channels[(i + n - 1) % n].page;
        links[2] = g_indexPage;
        links[3] = 0x100;
        links[4] = 0x100;
        if (WriteIfChanged(g_channels[i].page,
                           MakeTTI(subs, g_channels[i].page, "TV " + g_channels[i].name, links, 12)))
            changed++;
    }
    std::vector<Page> idx;
    BuildIndex(idx, now);
    int links[5] = { g_channels.empty() ? 0 : g_channels[0].page, 0, 0, 0x100, 0x100 };
    if (WriteIfChanged(g_indexPage, MakeTTI(idx, g_indexPage, "TV now and next", links, 15))) changed++;
    printf("%s  pages rebuilt (%d changed)\n", HHMM(now).c_str(), changed);
}

static time_t FileTime(const std::string &path)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA a;
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) return 0;
    unsigned long long t = ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) |
                           a.ftLastWriteTime.dwLowDateTime;
    return (time_t)(t / 10000000ULL - 11644473600ULL);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? st.st_mtime : 0;
#endif
}

static void DownloadAndLoad()
{
    printf("\n--- Downloading listings ---\n");
    if (Download()) printf("Downloaded OK\n");
    else            printf("Download failed - using the saved listings, if any\n");
    g_loadedAt = FileTime(g_saved);
    if (LoadListings()) {
        size_t total = 0;
        for (size_t i = 0; i < g_channels.size(); i++) {
            size_t k = g_sched[g_channels[i].id].size();
            total += k;
            if (!k) printf("   no programmes found for %s (%s) - check the id\n",
                           g_channels[i].id.c_str(), g_channels[i].name.c_str());
        }
        printf("Read %d programmes for %d channels\n", (int)total, (int)g_channels.size());
    }
}

int main()
{
    setvbuf(stdout, NULL, _IOLBF, 1024);
    LoadConfig();
    MakeDir(g_pagesDir.c_str());

    printf("=== Teletext TV listings (Freeview-EPG) ===\n");
    printf("Writing pages to %s (index %03X)\n", g_pagesDir.c_str(), g_indexPage);
    for (size_t i = 0; i < g_channels.size(); i++)
        printf("   %03X  %-12s %s\n", g_channels[i].page, g_channels[i].name.c_str(), g_channels[i].id.c_str());
    printf("Download every %d minutes, rebuild pages every %d minutes\n", g_download, g_refresh);
    printf("U = download now   Q = quit\n");

    /* use a recent saved copy straight away rather than downloading again */
    time_t nextDownload = 0, nextBuild = 0;
    time_t saved = FileTime(g_saved);
    if (saved && time(NULL) - saved < g_download * 60) {
        g_loadedAt = saved;
        LoadListings();
        nextDownload = saved + g_download * 60;
        printf("Using the saved listings from %s\n", HHMM(saved).c_str());
    }

    for (;;) {
        bool force = false;
        int k;
        while (KeyPressed(k)) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'u' || k == 'U') force = true;
        }
        time_t now = time(NULL);
        if (force || now >= nextDownload) {
            DownloadAndLoad();
            nextDownload = now + g_download * 60;
            nextBuild = 0;
        }
        if (now >= nextBuild) {
            BuildAll();
            nextBuild = now + g_refresh * 60;
        }
        SleepMs(500);
    }
}
