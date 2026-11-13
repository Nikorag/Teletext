/*
 * ttxflights.cpp  -  Airport arrivals and departures boards for ttxserver
 * ------------------------------------------------------------------------
 * Fetches live flight lists from a flight-data web API (MyAirports by
 * default) and writes Ceefax-style boards as .tti pages:
 *
 *     index page          list of airports              (e.g. 410)
 *     page N              arrivals for an airport       (e.g. 411)
 *     page N+1            departures                    (e.g. 412)
 *
 * Busy airports get several subpages that rotate on the client.
 * ttxserver notices the changed files and updates clients by itself.
 *
 * Free API accounts allow only so many requests a day, so ttxflights
 * counts them (in ttxflights.state) and spaces its updates to stay under
 * budget=, refreshing only between the hours you choose.
 *
 * ttxflights.cfg (next to the program):
 *     apikey=ma_live_xxxxxxxx
 *     pagesdir=../server/pages
 *     budget=95                 requests per day to stay within
 *     hours=6-23                only refresh between these hours
 *     indexpage=410
 *     airport=SOU,SOUTHAMPTON,411     code,name,arrivals page (departures = +1)
 *     airport=BRS,BRISTOL,413
 *     airport=LHR,HEATHROW,415
 *     airport=LGW,GATWICK,417
 *     url=https://myairports.online/api/airports/{code}/{type}
 *     header=Authorization: Bearer {key}     (repeat for more headers)
 *
 * Each raw reply is saved as last_<code>_<type>.json for checking.
 * Console keys: U = update now (uses requests)   Q = quit
 *
 * Build:
 *   Dev-C++ : Console Application, Linker: -lwininet
 *   Linux   : g++ -O2 -o ttxflights ttxflights.cpp      (needs curl)
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
#define ROWS_PER_SUB 17        /* flights per subpage (rows 5-21) */
#define MAX_SUBS 3

#define A_RED     "\x01"
#define A_GREEN   "\x02"
#define A_YELLOW  "\x03"
#define A_BLUE    "\x04"
#define A_CYAN    "\x06"
#define A_WHITE   "\x07"
#define DOUBLE_HEIGHT "\x0D"
#define NEW_BG    "\x1D"

struct Airport { std::string code, name; int page; };
struct Flight  {
    time_t      when;          /* scheduled time, for sorting */
    std::string time;          /* HH:MM local */
    std::string flight, place, status;
    int         colour;        /* teletext colour code for the status */
};
struct Page { unsigned char cell[ROWS][COLS]; };
typedef std::vector<std::pair<std::string, std::string> > Fields;

static std::vector<Airport>     g_airports;
static std::vector<std::string> g_headers;
static std::string g_apiKey;
static std::string g_pagesDir  = "../server/pages";
static std::string g_url       = "https://myairports.online/api/airports/{code}/{type}";
static int         g_budget    = 95;
static int         g_hourFrom  = 6, g_hourTo = 23;
static int         g_indexPage = 0x410;

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

static void ReplaceAll(std::string &s, const std::string &a, const std::string &b)
{
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
}

static void LoadConfig()
{
    FILE *f = fopen("ttxflights.cfg", "r");
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
            if (!_stricmp(key, "apikey"))         g_apiKey = val;
            else if (!_stricmp(key, "pagesdir"))  g_pagesDir = val;
            else if (!_stricmp(key, "url"))       g_url = val;
            else if (!_stricmp(key, "header"))    g_headers.push_back(val);
            else if (!_stricmp(key, "budget"))    g_budget = atoi(val);
            else if (!_stricmp(key, "indexpage")) g_indexPage = (int)strtol(val, NULL, 16);
            else if (!_stricmp(key, "hours"))     sscanf(val, "%d-%d", &g_hourFrom, &g_hourTo);
            else if (!_stricmp(key, "airport")) {
                std::string v(val);
                size_t c1 = v.find(','), c2 = (c1 == std::string::npos) ? c1 : v.find(',', c1 + 1);
                if (c2 == std::string::npos) continue;
                Airport a;
                a.code = v.substr(0, c1);
                a.name = v.substr(c1 + 1, c2 - c1 - 1);
                a.page = (int)strtol(v.substr(c2 + 1).c_str(), NULL, 16);
                if (a.page >= 0x100 && a.page < 0x8FF) g_airports.push_back(a);
            }
        }
        fclose(f);
    }
    if (g_airports.empty()) {
        const char *def[4][2] = { {"SOU","SOUTHAMPTON"}, {"BRS","BRISTOL"},
                                  {"LHR","HEATHROW"},    {"LGW","GATWICK"} };
        for (int i = 0; i < 4; i++) {
            Airport a;
            a.code = def[i][0];
            a.name = def[i][1];
            a.page = g_indexPage + 1 + i * 2;
            g_airports.push_back(a);
        }
    }
    if (g_headers.empty()) {             /* send the key both common ways */
        g_headers.push_back("Authorization: Bearer {key}");
        g_headers.push_back("X-API-Key: {key}");
    }
    if (g_budget < 8) g_budget = 8;
    if (g_hourFrom < 0 || g_hourFrom > 23) g_hourFrom = 0;
    if (g_hourTo <= g_hourFrom || g_hourTo > 24) g_hourTo = 24;
}

/* ------------------------------------------------------------------ */
/*  Daily request count, kept in a file so restarts don't reset it     */
/* ------------------------------------------------------------------ */

static std::string Today()
{
    time_t t = time(NULL);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&t));
    return buf;
}

static int RequestsToday()
{
    FILE *f = fopen("ttxflights.state", "r");
    if (!f) return 0;
    char day[16] = "";
    int n = 0;
    if (fscanf(f, "%15s %d", day, &n) != 2) n = 0;
    fclose(f);
    return Today() == day ? n : 0;
}

static void AddRequests(int k)
{
    int n = RequestsToday() + k;
    FILE *f = fopen("ttxflights.state", "w");
    if (!f) return;
    fprintf(f, "%s %d\n", Today().c_str(), n);
    fclose(f);
}

/* ------------------------------------------------------------------ */
/*  Downloading                                                        */
/* ------------------------------------------------------------------ */

/* Returns the HTTP status (200 = OK), 0 if no connection */
static int Fetch(const std::string &url, std::string &out)
{
    out.clear();
    if (url.compare(0, 7, "http://") != 0 && url.compare(0, 8, "https://") != 0) {
        FILE *f = fopen(url.c_str(), "rb");          /* local file, for testing */
        if (!f) return 0;
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        fclose(f);
        return 200;
    }

    std::vector<std::string> hdr;
    for (size_t i = 0; i < g_headers.size(); i++) {
        std::string h = g_headers[i];
        ReplaceAll(h, "{key}", g_apiKey);
        hdr.push_back(h);
    }

#ifdef _WIN32
    std::string headers = "Accept: application/json\r\n";
    for (size_t i = 0; i < hdr.size(); i++) headers += hdr[i] + "\r\n";
    HINTERNET net = InternetOpenA("ttxflights/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return 0;
    HINTERNET h = InternetOpenUrlA(net, url.c_str(), headers.c_str(), (DWORD)-1L,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE |
                                   INTERNET_FLAG_NO_UI, 0);
    if (!h) { InternetCloseHandle(net); return 0; }
    DWORD status = 0, len = sizeof(status);
    HttpQueryInfoA(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &len, NULL);
    char buf[8192];
    DWORD n = 0;
    while (InternetReadFile(h, buf, sizeof(buf), &n) && n > 0) out.append(buf, n);
    InternetCloseHandle(h);
    InternetCloseHandle(net);
    return (int)status;
#else
    if (url.find('\'') != std::string::npos) return 0;
    std::string cmd = "curl -sL --max-time 30 -H 'Accept: application/json'";
    for (size_t i = 0; i < hdr.size(); i++) {
        if (hdr[i].find('\'') != std::string::npos) continue;
        cmd += " -H '" + hdr[i] + "'";
    }
    cmd += " -w '\\n%{http_code}' '" + url + "'";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return 0;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    size_t nl = out.rfind('\n');                 /* status code is on the last line */
    if (nl == std::string::npos) return 0;
    int status = atoi(out.c_str() + nl + 1);
    out.erase(nl);
    return status;
#endif
}

/* ------------------------------------------------------------------ */
/*  A small JSON reader                                                */
/*  It turns the reply into a flat list of  path = value  pairs, e.g.  */
/*     data[3].departure.scheduledTime = 2026-11-13T14:35:00Z          */
/* ------------------------------------------------------------------ */

static void SkipWs(const std::string &s, size_t &i)
{
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
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

static bool ParseString(const std::string &s, size_t &i, std::string &out)
{
    if (s[i] != '"') return false;
    i++;
    out.clear();
    while (i < s.size() && s[i] != '"') {
        char c = s[i++];
        if (c != '\\') { out += c; continue; }
        if (i >= s.size()) return false;
        char e = s[i++];
        switch (e) {
        case 'n': out += ' '; break;
        case 't': out += ' '; break;
        case 'r': break;
        case 'b': case 'f': break;
        case 'u':
            if (i + 4 <= s.size()) {
                PutUtf8(out, strtoul(s.substr(i, 4).c_str(), NULL, 16));
                i += 4;
            }
            break;
        default: out += e;                     /* \" \\ \/ */
        }
    }
    if (i >= s.size()) return false;
    i++;                                       /* closing quote */
    return true;
}

static bool ParseValue(const std::string &s, size_t &i, const std::string &path, Fields &out, int depth)
{
    if (depth > 40) return false;
    SkipWs(s, i);
    if (i >= s.size()) return false;
    char c = s[i];
    if (c == '{') {
        i++;
        SkipWs(s, i);
        if (i < s.size() && s[i] == '}') { i++; return true; }
        for (;;) {
            SkipWs(s, i);
            std::string key;
            if (i >= s.size() || !ParseString(s, i, key)) return false;
            SkipWs(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            i++;
            if (!ParseValue(s, i, path.empty() ? key : path + "." + key, out, depth + 1)) return false;
            SkipWs(s, i);
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == '}') { i++; return true; }
            return false;
        }
    }
    if (c == '[') {
        i++;
        SkipWs(s, i);
        if (i < s.size() && s[i] == ']') { i++; return true; }
        for (int idx = 0; ; idx++) {
            char b[16];
            sprintf(b, "[%d]", idx);
            if (!ParseValue(s, i, path + b, out, depth + 1)) return false;
            SkipWs(s, i);
            if (i < s.size() && s[i] == ',') { i++; continue; }
            if (i < s.size() && s[i] == ']') { i++; return true; }
            return false;
        }
    }
    if (c == '"') {
        std::string v;
        if (!ParseString(s, i, v)) return false;
        out.push_back(std::make_pair(path, v));
        return true;
    }
    size_t start = i;                           /* number, true, false, null */
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isspace((unsigned char)s[i])) i++;
    std::string v = s.substr(start, i - start);
    if (v == "null") v = "";
    out.push_back(std::make_pair(path, v));
    return true;
}

/* Key in a standard form for matching: lower case, no _ or - */
static std::string Norm(const std::string &k)
{
    std::string r;
    for (size_t i = 0; i < k.size(); i++) {
        char c = k[i];
        if (c == '_' || c == '-') continue;
        r += (char)tolower((unsigned char)c);
    }
    return r;
}

/* Find the list of flights: the array with the most object elements.
 * Each element becomes one record of (normalised key, value). */
static void SplitRecords(const Fields &all, std::vector<Fields> &records)
{
    records.clear();
    std::map<std::string, std::map<int, int> > counts;
    for (size_t i = 0; i < all.size(); i++) {
        const std::string &p = all[i].first;
        for (size_t b = p.find('['); b != std::string::npos; b = p.find('[', b + 1)) {
            size_t e = p.find(']', b);
            if (e == std::string::npos || e + 1 >= p.size() || p[e + 1] != '.') continue;
            counts[p.substr(0, b)][atoi(p.c_str() + b + 1)]++;
        }
    }
    std::string best;
    size_t bestN = 0;
    std::map<std::string, std::map<int, int> >::iterator it;
    for (it = counts.begin(); it != counts.end(); ++it)
        if (it->second.size() > bestN ||
            (it->second.size() == bestN && it->first.size() < best.size())) {
            best = it->first;
            bestN = it->second.size();
        }
    if (!bestN) return;

    std::map<int, Fields> byIndex;
    std::string pre = best + "[";
    for (size_t i = 0; i < all.size(); i++) {
        const std::string &p = all[i].first;
        if (p.compare(0, pre.size(), pre) != 0) continue;
        size_t e = p.find("].", pre.size());
        if (e == std::string::npos) continue;
        int idx = atoi(p.c_str() + pre.size());
        byIndex[idx].push_back(std::make_pair(Norm(p.substr(e + 2)), all[i].second));
    }
    std::map<int, Fields>::iterator r;
    for (r = byIndex.begin(); r != byIndex.end(); ++r) records.push_back(r->second);
}

/* First non-empty value whose key matches a candidate exactly,
 * then any whose key ends in ".candidate" */
static std::string Pick(const Fields &rec, const char *const *cands)
{
    for (int pass = 0; pass < 2; pass++)
        for (int c = 0; cands[c]; c++) {
            std::string want = cands[c];
            for (size_t i = 0; i < rec.size(); i++) {
                const std::string &k = rec[i].first;
                bool hit = pass == 0 ? (k == want)
                                     : (k.size() > want.size() &&
                                        k.compare(k.size() - want.size(), want.size(), want) == 0 &&
                                        k[k.size() - want.size() - 1] == '.');
                if (hit && !rec[i].second.empty()) return rec[i].second;
            }
        }
    return "";
}

static bool HasKeyStarting(const Fields &rec, const char *prefix)
{
    size_t n = strlen(prefix);
    for (size_t i = 0; i < rec.size(); i++)
        if (rec[i].first.compare(0, n, prefix) == 0) return true;
    return false;
}

/* ------------------------------------------------------------------ */
/*  Times                                                              */
/* ------------------------------------------------------------------ */

static long DaysFromCivil(int y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* Understands 2026-11-13T14:35:00Z, ...+01:00, "2026-11-13 14:35",
 * "14:35", and Unix times in seconds or milliseconds. 0 = not a time. */
static time_t ParseTime(const std::string &s)
{
    if (s.empty()) return 0;
    bool digits = true;
    for (size_t i = 0; i < s.size(); i++) if (!isdigit((unsigned char)s[i])) digits = false;
    if (digits && s.size() >= 10) {
        double v = atof(s.c_str());
        if (s.size() >= 13) v /= 1000;
        return (time_t)v;
    }
    int Y, M, D, h, m, sec = 0;
    if (sscanf(s.c_str(), "%d-%d-%d%*c%d:%d:%d", &Y, &M, &D, &h, &m, &sec) >= 5) {
        /* time zone? */
        size_t tpos = s.find_first_of("Tt ", 8);
        std::string rest = tpos == std::string::npos ? "" : s.substr(tpos + 1);
        size_t z = rest.find_first_of("Zz+-");
        if (z != std::string::npos) {
            long off = 0;
            if (rest[z] == '+' || rest[z] == '-') {
                int oh = 0, om = 0;
                sscanf(rest.c_str() + z + 1, "%2d:%2d", &oh, &om);
                off = (oh * 60 + om) * 60L * (rest[z] == '-' ? -1 : 1);
            }
            return (time_t)(DaysFromCivil(Y, M, D) * 86400L + h * 3600L + m * 60L + sec - off);
        }
        struct tm t;
        memset(&t, 0, sizeof(t));
        t.tm_year = Y - 1900; t.tm_mon = M - 1; t.tm_mday = D;
        t.tm_hour = h; t.tm_min = m; t.tm_sec = sec; t.tm_isdst = -1;
        return mktime(&t);
    }
    if (sscanf(s.c_str(), "%d:%d", &h, &m) == 2 && h < 24 && m < 60) {
        time_t now = time(NULL);
        struct tm t = *localtime(&now);
        t.tm_hour = h; t.tm_min = m; t.tm_sec = 0; t.tm_isdst = -1;
        return mktime(&t);
    }
    return 0;
}

static std::string HHMM(time_t t)
{
    char b[8];
    strftime(b, sizeof(b), "%H:%M", localtime(&t));
    return b;
}

/* ------------------------------------------------------------------ */
/*  Text tidying                                                       */
/* ------------------------------------------------------------------ */

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
        else if (cp == 0x2019 || cp == 0x2018 || cp == '`') out += '\'';
        else if (cp >= 0x20 && cp < 0x7F && cp != '\\' && cp != '|' && cp != '^') out += (char)cp;
        else if (cp >= 0xC0 && cp <= 0xFF) out += latin[cp - 0xC0];
        else if (cp == '\t' || cp == 0xA0) out += ' ';
    }
    return out;
}

static std::string Fit(const std::string &s, size_t w)
{
    std::string t = s.substr(0, w);
    while (t.size() < w) t += ' ';
    return t;
}

static std::string Lower(const std::string &s)
{
    std::string t(s);
    for (size_t i = 0; i < t.size(); i++) t[i] = (char)tolower((unsigned char)t[i]);
    return t;
}

/* Common airport codes, so a board can show a name when the API gives
 * only a code */
static std::string CodeName(const std::string &c)
{
    static const char *tab[][2] = {
        {"ABZ","Aberdeen"}, {"ACE","Lanzarote"}, {"AGP","Malaga"}, {"ALC","Alicante"},
        {"AMS","Amsterdam"}, {"ARN","Stockholm"}, {"ATH","Athens"}, {"BCN","Barcelona"},
        {"BFS","Belfast Intl"}, {"BHD","Belfast City"}, {"BHX","Birmingham"}, {"BRS","Bristol"},
        {"BRU","Brussels"}, {"CDG","Paris CDG"}, {"CPH","Copenhagen"}, {"CWL","Cardiff"},
        {"DOH","Doha"}, {"DUB","Dublin"}, {"DUS","Dusseldorf"}, {"DXB","Dubai"},
        {"EDI","Edinburgh"}, {"EWR","Newark"}, {"EXT","Exeter"}, {"FAO","Faro"},
        {"FCO","Rome"}, {"FRA","Frankfurt"}, {"FUE","Fuerteventura"}, {"GCI","Guernsey"},
        {"GLA","Glasgow"}, {"GVA","Geneva"}, {"IBZ","Ibiza"}, {"INV","Inverness"},
        {"IOM","Isle of Man"}, {"IST","Istanbul"}, {"JER","Jersey"}, {"JFK","New York JFK"},
        {"LCY","London City"}, {"LGW","Gatwick"}, {"LHR","Heathrow"}, {"LIS","Lisbon"},
        {"LPA","Gran Canaria"}, {"LTN","Luton"}, {"MAD","Madrid"}, {"MAN","Manchester"},
        {"MUC","Munich"}, {"NCE","Nice"}, {"NCL","Newcastle"}, {"NQY","Newquay"},
        {"ORY","Paris Orly"}, {"OSL","Oslo"}, {"PMI","Palma"}, {"SOU","Southampton"},
        {"STN","Stansted"}, {"TFS","Tenerife S"}, {"VIE","Vienna"}, {"ZRH","Zurich"},
        {NULL, NULL}
    };
    for (int i = 0; tab[i][0]; i++) if (c == tab[i][0]) return tab[i][1];
    return c;
}

/* ------------------------------------------------------------------ */
/*  Turning API records into flights                                   */
/* ------------------------------------------------------------------ */

static const char *kFlight[] = { "flightnumber", "flightiata", "flight.iata", "flight.iatanumber",
    "flight.number", "flightno", "iatanumber", "flightcode", "flight", "number", "ident",
    "callsign", NULL };
static const char *kAirline[] = { "airline.iata", "airlineiata", "airline.code", "airlinecode",
    "carrier.iata", "carriercode", NULL };
static const char *kStatus[] = { "status", "flightstatus", "status.text", "statustext",
    "remark", "remarks", "state", NULL };

/* arrivals: where from, our arrival time */
static const char *kFrom[] = { "origincity", "origin.city", "origin.name", "originname",
    "origin.airportname", "origin.airport.name", "departure.city", "departure.airport.name",
    "departure.airportname", "departure.airport", "fromcity", "from", "origin",
    "origin.iata", "origin.code", "originiata", "origincode", "departure.iata",
    "departure.iatacode", NULL };
static const char *kArrSched[] = { "arrival.scheduledtime", "arrival.scheduled",
    "arrival.scheduled.local", "scheduledarrival", "scheduledarrivaltime", "sta",
    "scheduledtime", "scheduled", "time", "scheduledat", "datetime", NULL };
static const char *kArrEst[] = { "arrival.actualtime", "arrival.actual", "arrival.estimatedtime",
    "arrival.estimated", "actualarrival", "estimatedarrival", "ata", "eta", "actualtime",
    "actual", "estimatedtime", "estimated", "expectedtime", "expected", "revisedtime",
    "revised", NULL };

/* departures: where to, our departure time */
static const char *kTo[] = { "destinationcity", "destination.city", "destination.name",
    "destinationname", "destination.airportname", "destination.airport.name",
    "arrival.city", "arrival.airport.name", "arrival.airportname", "arrival.airport",
    "tocity", "to", "destination", "destination.iata", "destination.code",
    "destinationiata", "destinationcode", "arrival.iata", "arrival.iatacode", NULL };
static const char *kDepSched[] = { "departure.scheduledtime", "departure.scheduled",
    "departure.scheduled.local", "scheduleddeparture", "scheduleddeparturetime", "std",
    "scheduledtime", "scheduled", "time", "scheduledat", "datetime", NULL };
static const char *kDepEst[] = { "departure.actualtime", "departure.actual",
    "departure.estimatedtime", "departure.estimated", "actualdeparture", "estimateddeparture",
    "atd", "etd", "actualtime", "actual", "estimatedtime", "estimated", "expectedtime",
    "expected", "revisedtime", "revised", NULL };

static bool IsCodeshare(const Fields &rec)
{
    if (HasKeyStarting(rec, "codeshared.")) return true;     /* Aviation Edge style */
    for (size_t i = 0; i < rec.size(); i++) {
        const std::string &k = rec[i].first;
        if ((k == "iscodeshare" || k == "codeshare" || k == "iscodeshared") &&
            Lower(rec[i].second) == "true") return true;
    }
    return false;
}

static void MakeStatus(Flight &f, const std::string &raw, time_t sched, time_t est, bool arrivals)
{
    std::string s = Lower(raw);
    f.colour = 7;
    if (s.find("cancel") != std::string::npos)  { f.status = "Cancelled"; f.colour = 1; return; }
    if (s.find("divert") != std::string::npos)  { f.status = "Diverted";  f.colour = 1; return; }
    if (arrivals && (s.find("land") != std::string::npos || s.find("arrived") != std::string::npos)) {
        f.status = est ? "Lnd " + HHMM(est) : "Landed";
        f.colour = 2;
        return;
    }
    if (!arrivals && (s.find("depart") != std::string::npos || s.find("airborne") != std::string::npos ||
                      s.find("en route") != std::string::npos || s.find("active") != std::string::npos)) {
        f.status = est ? "Dep " + HHMM(est) : "Departed";
        f.colour = 2;
        return;
    }
    if (s.find("board") != std::string::npos)   { f.status = "Boarding";  f.colour = 3; return; }
    if (s.find("final") != std::string::npos)   { f.status = "Final call"; f.colour = 3; return; }
    if (s.find("gate clos") != std::string::npos || s.find("closed") != std::string::npos) {
        f.status = "Gate shut"; f.colour = 6; return;
    }
    if (est && sched && (est - sched >= 5 * 60)) {
        f.status = "Exp " + HHMM(est);
        f.colour = 3;
        return;
    }
    if (s.find("delay") != std::string::npos)   { f.status = "Delayed";   f.colour = 3; return; }
    if (arrivals && s.find("approach") != std::string::npos) { f.status = "Landing"; f.colour = 6; return; }
    if (arrivals && (s.find("active") != std::string::npos || s.find("en route") != std::string::npos ||
                     s.find("airborne") != std::string::npos || s.find("in flight") != std::string::npos)) {
        f.status = (est && est - sched >= 5 * 60) ? "Exp " + HHMM(est) : "In flight";
        f.colour = 6;
        return;
    }
    f.status = "On time";
    f.colour = 7;
}

static bool ByTime(const Flight &a, const Flight &b) { return a.when < b.when; }

static int ParseFlights(const std::string &json, bool arrivals, std::vector<Flight> &list)
{
    list.clear();
    Fields all;
    size_t i = 0;
    if (!ParseValue(json, i, "", all, 0)) return -1;
    std::vector<Fields> recs;
    SplitRecords(all, recs);

    time_t now = time(NULL);
    time_t from = now - (arrivals ? 60 : 30) * 60;    /* keep recent ones briefly */
    std::map<std::string, bool> seen;

    for (size_t r = 0; r < recs.size(); r++) {
        const Fields &rec = recs[r];
        if (IsCodeshare(rec)) continue;
        Flight f;
        time_t sched = ParseTime(Pick(rec, arrivals ? kArrSched : kDepSched));
        if (!sched) continue;
        time_t est = ParseTime(Pick(rec, arrivals ? kArrEst : kDepEst));
        if ((est ? est : sched) < from) continue;

        f.when = sched;
        f.time = HHMM(sched);
        f.flight = Pick(rec, kFlight);
        bool numOnly = !f.flight.empty();
        for (size_t k = 0; k < f.flight.size(); k++) if (!isdigit((unsigned char)f.flight[k])) numOnly = false;
        if (numOnly) f.flight = Pick(rec, kAirline) + f.flight;
        ReplaceAll(f.flight, " ", "");
        f.flight = ToTeletext(f.flight);

        std::string place = Pick(rec, arrivals ? kFrom : kTo);
        if (place.size() == 3) {
            for (size_t k = 0; k < 3; k++) place[k] = (char)toupper((unsigned char)place[k]);
            place = CodeName(place);
        }
        f.place = ToTeletext(place);

        MakeStatus(f, Pick(rec, kStatus), sched, est, arrivals);

        std::string dup = f.time + "|" + f.place;      /* drop codeshare copies */
        if (seen[dup]) continue;
        seen[dup] = true;
        list.push_back(f);
    }
    std::sort(list.begin(), list.end(), ByTime);
    return (int)recs.size();
}

/* ------------------------------------------------------------------ */
/*  Pages                                                              */
/* ------------------------------------------------------------------ */

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
    std::string tmp = g_pagesDir + "/~F" + Hex3(number) + ".tmp";
    f = fopen(tmp.c_str(), "wb");
    if (!f) { printf("   cannot write %s\n", tmp.c_str()); return false; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (!ReplaceWith(tmp.c_str(), path.c_str())) { remove(tmp.c_str()); return false; }
    return true;
}

static void Banner(Page &p, const std::string &title)
{
    Put(p, 1, 0, A_BLUE NEW_BG DOUBLE_HEIGHT A_YELLOW);
    Put(p, 1, 4, title.substr(0, 35));
}

static void BuildBoard(std::vector<Page> &subs, const Airport &a, bool arrivals,
                       const std::vector<Flight> &list, const std::string &updated)
{
    subs.clear();
    size_t per = ROWS_PER_SUB;
    size_t nsub = list.empty() ? 1 : (list.size() + per - 1) / per;
    if (nsub > MAX_SUBS) nsub = MAX_SUBS;

    for (size_t s = 0; s < nsub; s++) {
        Page p;
        Blank(p);
        Banner(p, a.name + (arrivals ? " ARRIVALS" : " DEPARTURES"));
        Put(p, 3, 0, std::string(A_CYAN) + Fit("Time", 6) + Fit("Flight", 8)
                     + Fit(arrivals ? "From" : "To", 15) + "Status");
        if (nsub > 1) {
            char b[16];
            sprintf(b, "%d/%d", (int)s + 1, (int)nsub);
            Put(p, 1, 36, b);
        }
        int row = 5;
        for (size_t i = s * per; i < list.size() && i < (s + 1) * per; i++) {
            const Flight &f = list[i];
            std::string line = A_WHITE + Fit(f.time, 5) + " " + Fit(f.flight, 7) + " " + Fit(f.place, 13);
            line += (char)f.colour;
            line += Fit(f.status, 10);
            Put(p, row++, 0, line);
        }
        if (list.empty()) Put(p, 8, 0, A_WHITE "No flights to show at the moment");
        Put(p, 23, 0, A_GREEN "Updated " + updated + "  Times are local");
        Put(p, 24, 0, std::string(A_RED) + (arrivals ? "Departures " : "Arrivals   ")
                      + A_GREEN "Next airport " A_YELLOW "Back  " A_CYAN "Index");
        subs.push_back(p);
    }
}

static void BuildIndex(Page &p)
{
    Blank(p);
    Banner(p, "UK AIRPORTS");
    Put(p, 3, 0, A_CYAN "Live arrivals and departures");
    int row = 6;
    for (size_t i = 0; i < g_airports.size() && row <= 20; i++, row += 2) {
        char line[64];
        sprintf(line, A_WHITE "%-12.12s" A_CYAN "Arrive " A_YELLOW "%03X " A_CYAN "Depart " A_YELLOW "%03X",
                g_airports[i].name.c_str(), g_airports[i].page, g_airports[i].page + 1);
        Put(p, row, 0, line);
    }
    Put(p, 22, 0, A_GREEN "Pages update through the day");
    Put(p, 24, 0, A_RED "First airport " A_CYAN "Index");
}

/* ------------------------------------------------------------------ */
/*  Update cycle                                                       */
/* ------------------------------------------------------------------ */

static void UpdateBoard(size_t ai, bool arrivals, const std::string &updated)
{
    const Airport &a = g_airports[ai];
    const char *type = arrivals ? "arrivals" : "departures";
    std::string url = g_url;
    ReplaceAll(url, "{code}", a.code);
    ReplaceAll(url, "{type}", type);
    ReplaceAll(url, "{key}", g_apiKey);

    printf("%s %-10s ", a.code.c_str(), type);
    std::string body;
    int status = Fetch(url, body);
    if (url.compare(0, 4, "http") == 0) AddRequests(1);

    std::string dump = "last_" + a.code + "_" + type + ".json";
    FILE *f = fopen(dump.c_str(), "wb");
    if (f) { fwrite(body.data(), 1, body.size(), f); fclose(f); }

    if (status == 0)   { printf("no connection - keeping the old page\n"); return; }
    if (status == 401 || status == 403) {
        printf("key refused (HTTP %d) - check apikey= in ttxflights.cfg\n", status);
        return;
    }
    if (status == 429) { printf("daily limit reached at the API (HTTP 429)\n"); return; }
    if (status != 200) { printf("HTTP %d - see %s\n", status, dump.c_str()); return; }

    std::vector<Flight> list;
    int recs = ParseFlights(body, arrivals, list);
    if (recs < 0) { printf("reply is not JSON - see %s\n", dump.c_str()); return; }
    if (recs > 0 && list.empty())
        printf("(%d records, none usable - please send %s) ", recs, dump.c_str());

    std::vector<Page> subs;
    BuildBoard(subs, a, arrivals, list, updated);
    int page = a.page + (arrivals ? 0 : 1);
    size_t n = g_airports.size();
    const Airport &next = g_airports[(ai + 1) % n];
    const Airport &prev = g_airports[(ai + n - 1) % n];
    int links[5];
    links[0] = arrivals ? a.page + 1 : a.page;
    links[1] = next.page + (arrivals ? 0 : 1);
    links[2] = prev.page + (arrivals ? 0 : 1);
    links[3] = g_indexPage;
    links[4] = g_indexPage;
    bool changed = WriteIfChanged(page, MakeTTI(subs, page, a.name + " " + type, links, 10));
    printf("%3d flights, %d subpage(s)%s\n", (int)list.size(), (int)subs.size(),
           changed ? ", page updated" : "");
}

static void UpdateAll()
{
    std::string updated = HHMM(time(NULL));
    printf("\n--- Updating at %s (requests used today: %d of %d) ---\n",
           updated.c_str(), RequestsToday(), g_budget);

    Page idx;
    BuildIndex(idx);
    std::vector<Page> one(1, idx);
    int links[5] = { g_airports[0].page, 0, 0, 0x100, 0x100 };
    WriteIfChanged(g_indexPage, MakeTTI(one, g_indexPage, "Airports index", links, 8));

    for (size_t i = 0; i < g_airports.size(); i++) {
        UpdateBoard(i, true, updated);
        UpdateBoard(i, false, updated);
    }
}

int main()
{
    setvbuf(stdout, NULL, _IOLBF, 1024);
    LoadConfig();
    MakeDir(g_pagesDir.c_str());

    int perRun = (int)g_airports.size() * 2;
    int runs = g_budget / perRun;
    if (runs < 1) runs = 1;
    int activeMin = (g_hourTo - g_hourFrom) * 60;
    int interval = activeMin / runs;
    if (interval < 15) interval = 15;

    printf("=== Teletext airport boards ===\n");
    printf("Writing pages to %s (index %03X)\n", g_pagesDir.c_str(), g_indexPage);
    for (size_t i = 0; i < g_airports.size(); i++)
        printf("   %s  %-14s arrivals %03X  departures %03X\n", g_airports[i].code.c_str(),
               g_airports[i].name.c_str(), g_airports[i].page, g_airports[i].page + 1);
    printf("%d requests per update, budget %d a day -> updating every %d minutes, %02d:00-%02d:00\n",
           perRun, g_budget, interval, g_hourFrom, g_hourTo);
    if (g_apiKey.empty() && g_url.compare(0, 4, "http") == 0)
        printf("WARNING: no apikey= in ttxflights.cfg\n");
    printf("U = update now   Q = quit\n");

    time_t next = 0;
    for (;;) {
        time_t now = time(NULL);
        struct tm *tm = localtime(&now);
        bool inHours = tm->tm_hour >= g_hourFrom && tm->tm_hour < g_hourTo;
        bool force = false;

        int k;
        while (KeyPressed(k)) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'u' || k == 'U') force = true;
        }

        if (force || (inHours && now >= next)) {
            bool local = g_url.compare(0, 4, "http") != 0;
            if (!local && RequestsToday() + perRun > g_budget) {
                if (force) printf("Not enough requests left today (%d used of %d)\n",
                                  RequestsToday(), g_budget);
            } else {
                UpdateAll();
                printf("Next update in %d minutes. U = update now, Q = quit\n", interval);
            }
            next = now + interval * 60;
        }
        SleepMs(500);
    }
}
