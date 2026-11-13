/*
 * ttxweather.cpp  -  Weather pages for ttxserver, from Open-Meteo
 * ----------------------------------------------------------------
 * Fetches current conditions and a 5-day forecast for each place in
 * ttxweather.cfg from Open-Meteo (free for non-commercial use, no key)
 * and writes Ceefax-style .tti pages:
 *
 *     index page     every place with its temperature and weather (400)
 *     one page each  now + mosaic weather symbol, 5-day forecast   (401-409)
 *
 * ttxweather.cfg (next to the program):
 *     pagesdir=../server/pages
 *     interval=30                    minutes between updates
 *     indexpage=400
 *     timezone=Europe/London
 *     place=SOUTHAMPTON,50.909,-1.404,401      name,latitude,longitude,page
 *
 * Weather data by Open-Meteo.com (CC BY 4.0) - shown on every page.
 * Console keys: U = update now   Q = quit
 *
 * Build:
 *   Dev-C++ : Console Application, Linker: -lwininet
 *   Linux   : g++ -O2 -o ttxweather ttxweather.cpp      (needs curl)
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
#include <math.h>
#include <time.h>
#include <string>
#include <vector>
#include <map>

#define ROWS 25
#define COLS 40
#define DAYS 5

#define A_RED     "\x01"
#define A_GREEN   "\x02"
#define A_YELLOW  "\x03"
#define A_BLUE    "\x04"
#define A_MAGENTA "\x05"
#define A_CYAN    "\x06"
#define A_WHITE   "\x07"
#define DOUBLE_HEIGHT "\x0D"
#define NEW_BG    "\x1D"

struct Place { std::string name, lat, lon; int page; };
struct Page  { unsigned char cell[ROWS][COLS]; };
typedef std::map<std::string, std::string> Json;

/* what we keep for the index page */
struct Summary { bool ok; std::string temp, text; int colour; };

static std::vector<Place> g_places;
static std::string g_pagesDir  = "../server/pages";
static std::string g_timezone  = "Europe/London";
static std::string g_url =
    "https://api.open-meteo.com/v1/forecast?latitude={lat}&longitude={lon}"
    "&current=temperature_2m,relative_humidity_2m,weather_code,wind_speed_10m,wind_direction_10m"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,sunrise,sunset"
    "&timezone={tz}&wind_speed_unit=mph&forecast_days=5";
static int g_interval  = 30;
static int g_indexPage = 0x400;

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

static void ReplaceAll(std::string &s, const std::string &a, const std::string &b)
{
    size_t p = 0;
    while ((p = s.find(a, p)) != std::string::npos) { s.replace(p, a.size(), b); p += b.size(); }
}

/* ------------------------------------------------------------------ */
/*  Settings                                                           */
/* ------------------------------------------------------------------ */

static void AddPlace(const char *name, const char *lat, const char *lon, int page)
{
    Place p;
    p.name = name; p.lat = lat; p.lon = lon; p.page = page;
    g_places.push_back(p);
}

static void LoadConfig()
{
    FILE *f = fopen("ttxweather.cfg", "r");
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
            else if (!_stricmp(key, "timezone"))  g_timezone = val;
            else if (!_stricmp(key, "url"))       g_url = val;
            else if (!_stricmp(key, "interval"))  g_interval = atoi(val);
            else if (!_stricmp(key, "indexpage")) g_indexPage = (int)strtol(val, NULL, 16);
            else if (!_stricmp(key, "place")) {
                char name[64], lat[32], lon[32];
                int page = 0;
                char pg[16];
                if (sscanf(val, "%63[^,],%31[^,],%31[^,],%15s", name, lat, lon, pg) == 4) {
                    page = (int)strtol(pg, NULL, 16);
                    if (page >= 0x100 && page <= 0x8FF) AddPlace(name, lat, lon, page);
                }
            }
        }
        fclose(f);
    }
    if (g_places.empty()) {
        AddPlace("SOUTHAMPTON", "50.909", "-1.404", g_indexPage + 1);
        AddPlace("LONDON",      "51.507", "-0.128", g_indexPage + 2);
        AddPlace("BRISTOL",     "51.454", "-2.588", g_indexPage + 3);
    }
    if (g_interval < 10) g_interval = 10;           /* be polite to Open-Meteo */
}

/* ------------------------------------------------------------------ */
/*  Downloading                                                        */
/* ------------------------------------------------------------------ */

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
#ifdef _WIN32
    HINTERNET net = InternetOpenA("ttxweather/1.0", INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
    if (!net) return 0;
    HINTERNET h = InternetOpenUrlA(net, url.c_str(), "Accept: application/json\r\n", (DWORD)-1L,
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
    std::string cmd = "curl -sL --max-time 30 -A ttxweather/1.0 -w '\\n%{http_code}' '" + url + "'";
    FILE *p = popen(cmd.c_str(), "r");
    if (!p) return 0;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
    pclose(p);
    size_t nl = out.rfind('\n');
    if (nl == std::string::npos) return 0;
    int status = atoi(out.c_str() + nl + 1);
    out.erase(nl);
    return status;
#endif
}

/* ------------------------------------------------------------------ */
/*  A small JSON reader: flattens the reply into  path = value  pairs  */
/*  e.g.  daily.temperature_2m_max[2] = 11.4                           */
/* ------------------------------------------------------------------ */

static void SkipWs(const std::string &s, size_t &i)
{
    while (i < s.size() && isspace((unsigned char)s[i])) i++;
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
        if (e == 'u') i += 4;                      /* not needed for weather data */
        else if (e != 'n' && e != 'r' && e != 't') out += e;
    }
    if (i >= s.size()) return false;
    i++;
    return true;
}

static bool ParseValue(const std::string &s, size_t &i, const std::string &path, Json &out, int depth)
{
    if (depth > 30) return false;
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
        out[path] = v;
        return true;
    }
    size_t start = i;
    while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && !isspace((unsigned char)s[i])) i++;
    std::string v = s.substr(start, i - start);
    out[path] = (v == "null") ? "" : v;
    return true;
}

static std::string Get(const Json &j, const std::string &key)
{
    Json::const_iterator it = j.find(key);
    return it == j.end() ? "" : it->second;
}

static std::string GetDay(const Json &j, const char *field, int d)
{
    char k[96];
    sprintf(k, "daily.%s[%d]", field, d);
    return Get(j, k);
}

/* ------------------------------------------------------------------ */
/*  Weather words and symbols                                          */
/* ------------------------------------------------------------------ */

enum { K_SUN, K_SUNCLOUD, K_CLOUD, K_FOG, K_RAIN, K_SNOW, K_THUNDER };

/* WMO weather code -> short description, symbol kind, text colour */
static void Describe(int code, std::string &text, int &kind, int &colour)
{
    kind = K_CLOUD; colour = 7;
    switch (code) {
    case 0:  text = "Sunny";            kind = K_SUN;      colour = 3; break;
    case 1:  text = "Mainly sunny";     kind = K_SUNCLOUD; colour = 3; break;
    case 2:  text = "Partly cloudy";    kind = K_SUNCLOUD; colour = 7; break;
    case 3:  text = "Cloudy";           kind = K_CLOUD;    colour = 7; break;
    case 45: case 48: text = "Fog";     kind = K_FOG;      colour = 7; break;
    case 51: case 53: case 55: text = "Drizzle"; kind = K_RAIN; colour = 6; break;
    case 56: case 57: text = "Freezing drizzle"; kind = K_RAIN; colour = 6; break;
    case 61: text = "Light rain";       kind = K_RAIN;     colour = 6; break;
    case 63: text = "Rain";             kind = K_RAIN;     colour = 6; break;
    case 65: text = "Heavy rain";       kind = K_RAIN;     colour = 6; break;
    case 66: case 67: text = "Freezing rain"; kind = K_RAIN; colour = 6; break;
    case 71: text = "Light snow";       kind = K_SNOW;     colour = 7; break;
    case 73: text = "Snow";             kind = K_SNOW;     colour = 7; break;
    case 75: text = "Heavy snow";       kind = K_SNOW;     colour = 7; break;
    case 77: text = "Snow grains";      kind = K_SNOW;     colour = 7; break;
    case 80: text = "Light showers";    kind = K_RAIN;     colour = 6; break;
    case 81: text = "Showers";          kind = K_RAIN;     colour = 6; break;
    case 82: text = "Heavy showers";    kind = K_RAIN;     colour = 6; break;
    case 85: case 86: text = "Snow showers"; kind = K_SNOW; colour = 7; break;
    case 95: text = "Thunderstorms";    kind = K_THUNDER;  colour = 5; break;
    case 96: case 99: text = "Thunder, hail"; kind = K_THUNDER; colour = 5; break;
    default: text = "Unknown";          break;
    }
}

/* A 20 x 15 "pixel" picture, drawn as 10 x 5 mosaic cells.
 * Each of the 5 rows of cells has one colour (a teletext rule). */
struct Icon {
    bool px[15][20];
    int  rowColour[5];
};

static void Disc(Icon &ic, double cx, double cy, double r)
{
    for (int y = 0; y < 15; y++)
        for (int x = 0; x < 20; x++) {
            double dx = (x + 0.5 - cx), dy = (y + 0.5 - cy) * 1.1;
            if (dx * dx + dy * dy <= r * r) ic.px[y][x] = true;
        }
}

static void Cloud(Icon &ic, double cy)
{
    Disc(ic, 6.5, cy + 0.5, 3.6);
    Disc(ic, 11.0, cy - 1.0, 4.6);
    Disc(ic, 15.0, cy + 1.0, 3.2);
    for (int y = (int)cy + 1; y <= (int)cy + 3 && y < 15; y++)
        for (int x = 4; x <= 17; x++) if (y >= 0) ic.px[y][x] = true;
}

static void MakeIcon(Icon &ic, int kind)
{
    memset(ic.px, 0, sizeof(ic.px));
    for (int r = 0; r < 5; r++) ic.rowColour[r] = 7;
    switch (kind) {
    case K_SUN:
        Disc(ic, 10, 7.5, 6.5);
        for (int r = 0; r < 5; r++) ic.rowColour[r] = 3;
        break;
    case K_SUNCLOUD:                       /* sun peeping over a cloud */
        Disc(ic, 13, 5, 5.0);
        for (int y = 6; y < 15; y++) for (int x = 0; x < 20; x++) ic.px[y][x] = false;
        Cloud(ic, 10);
        ic.rowColour[0] = 3; ic.rowColour[1] = 3;
        break;
    case K_CLOUD:
        Cloud(ic, 8);
        break;
    case K_FOG:
        for (int y = 3; y < 15; y += 3)
            for (int x = (y % 2) * 2; x < 18 + (y % 2) * 2 && x < 20; x++) ic.px[y][x] = true;
        break;
    case K_RAIN:
    case K_SNOW:
    case K_THUNDER:
        Cloud(ic, 4);                      /* cloud in the top three rows */
        for (int y = 9; y < 15; y++) for (int x = 0; x < 20; x++) ic.px[y][x] = false;
        if (kind == K_THUNDER) {           /* lightning bolt */
            static const int bolt[6][2] = { {11,9}, {10,10}, {9,11}, {10,12}, {9,13}, {8,14} };
            for (int k = 0; k < 6; k++) {
                ic.px[bolt[k][1]][bolt[k][0]] = true;
                ic.px[bolt[k][1]][bolt[k][0] + 1] = true;
            }
            ic.rowColour[3] = 3; ic.rowColour[4] = 3;
        } else {                           /* drops or flakes */
            for (int y = 9; y < 15; y += 2)
                for (int x = 3 + (y % 4 == 1 ? 2 : 0); x < 18; x += 4) ic.px[y][x] = true;
            ic.rowColour[3] = ic.rowColour[4] = (kind == K_RAIN) ? 6 : 7;
        }
        break;
    }
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

static void PutIcon(Page &p, int row0, int col0, const Icon &ic)
{
    static const unsigned char bit[3][2] = { {0x01, 0x02}, {0x04, 0x08}, {0x10, 0x40} };
    for (int r = 0; r < 5; r++) {
        p.cell[row0 + r][col0] = (unsigned char)(0x10 + ic.rowColour[r]);   /* graphics colour */
        for (int c = 0; c < 10; c++) {
            unsigned char ch = 0x20;
            for (int sy = 0; sy < 3; sy++)
                for (int sx = 0; sx < 2; sx++)
                    if (ic.px[r * 3 + sy][c * 2 + sx]) ch |= bit[sy][sx];
            p.cell[row0 + r][col0 + 1 + c] = ch;
        }
    }
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

static std::string RightFit(const std::string &s, size_t w)
{
    std::string t = s.substr(0, w);
    while (t.size() < w) t = " " + t;
    return t;
}

static void Banner(Page &p, const std::string &title)
{
    Put(p, 1, 0, A_BLUE NEW_BG DOUBLE_HEIGHT A_YELLOW);
    Put(p, 1, 4, title.substr(0, 35));
}

static std::string Round(const std::string &v)            /* "9.6" -> "10" */
{
    if (v.empty()) return "--";
    char b[16];
    sprintf(b, "%d", (int)floor(atof(v.c_str()) + 0.5));
    return b;
}

static std::string Compass(const std::string &deg)
{
    static const char *pts[16] = { "N","NNE","NE","ENE","E","ESE","SE","SSE",
                                   "S","SSW","SW","WSW","W","WNW","NW","NNW" };
    if (deg.empty()) return "";
    int d = (int)floor(atof(deg.c_str()) / 22.5 + 0.5) % 16;
    if (d < 0) d += 16;
    return pts[d];
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

static std::string DayName(const std::string &date)        /* "2026-11-13" -> "Fri" */
{
    static const char *n[7] = { "Sun","Mon","Tue","Wed","Thu","Fri","Sat" };
    int y, m, d;
    if (sscanf(date.c_str(), "%d-%d-%d", &y, &m, &d) != 3) return "";
    long days = DaysFromCivil(y, m, d);
    return n[((days % 7) + 7 + 4) % 7];
}

static std::string TimePart(const std::string &iso)        /* "2026-11-13T07:15" -> "07:15" */
{
    size_t t = iso.find('T');
    return t == std::string::npos ? iso : iso.substr(t + 1, 5);
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
    std::string tmp = g_pagesDir + "/~W" + Hex3(number) + ".tmp";
    f = fopen(tmp.c_str(), "wb");
    if (!f) { printf("   cannot write %s\n", tmp.c_str()); return false; }
    fwrite(content.data(), 1, content.size(), f);
    fclose(f);
    if (!ReplaceWith(tmp.c_str(), path.c_str())) { remove(tmp.c_str()); return false; }
    return true;
}

/* One place's page. Returns a summary for the index. */
static Summary BuildPlace(Page &p, const Place &pl, const Json &j)
{
    Summary sum;
    sum.ok = false;
    Blank(p);
    Banner(p, "WEATHER: " + pl.name);

    std::string temp = Get(j, "current.temperature_2m");
    std::string codeS = Get(j, "current.weather_code");
    if (temp.empty() || codeS.empty()) {
        Put(p, 8, 0, A_RED "Weather is not available at the moment");
        return sum;
    }
    std::string text;
    int kind, colour;
    Describe(atoi(codeS.c_str()), text, kind, colour);

    /* now - text on the left, symbol on the right (rows 3-7) */
    Put(p, 3, 0, A_CYAN "Now (" + TimePart(Get(j, "current.time")) + ")");
    std::string col(1, (char)colour);
    Put(p, 4, 0, A_WHITE "Temperature " A_YELLOW + Round(temp) + "C");
    Put(p, 5, 0, col + text);
    std::string wind = Round(Get(j, "current.wind_speed_10m"));
    Put(p, 6, 0, A_WHITE "Wind " + Compass(Get(j, "current.wind_direction_10m")) + " " + wind + " mph");
    Put(p, 7, 0, A_WHITE "Humidity " + Round(Get(j, "current.relative_humidity_2m")) + "%");
    Icon ic;
    MakeIcon(ic, kind);
    PutIcon(p, 3, 28, ic);

    /* forecast */
    Put(p, 10, 0, A_CYAN "Day   Weather          Max  Min  Rain");
    for (int d = 0; d < DAYS; d++) {
        std::string date = GetDay(j, "time", d);
        if (date.empty()) break;
        std::string dt;
        int dk, dc;
        Describe(atoi(GetDay(j, "weather_code", d).c_str()), dt, dk, dc);
        std::string rain = GetDay(j, "precipitation_probability_max", d);
        std::string line = A_WHITE + Fit(d == 0 ? "Today" : DayName(date), 5);
        line += (char)dc;
        line += Fit(dt, 16);
        line += A_YELLOW + RightFit(Round(GetDay(j, "temperature_2m_max", d)), 3) + "C";
        line += A_CYAN + RightFit(Round(GetDay(j, "temperature_2m_min", d)), 3) + "C";
        line += A_WHITE + RightFit(rain.empty() ? "" : Round(rain) + "%", 4);
        Put(p, 12 + d, 0, line);
    }

    std::string rise = TimePart(GetDay(j, "sunrise", 0)), set = TimePart(GetDay(j, "sunset", 0));
    if (!rise.empty()) Put(p, 18, 0, A_YELLOW "Sunrise " + rise + A_MAGENTA "Sunset " + set);

    sum.ok = true;
    sum.temp = Round(temp) + "C";
    sum.text = text;
    sum.colour = colour;
    return sum;
}

static void Footer(Page &p, const std::string &updated)
{
    Put(p, 22, 0, A_GREEN "Updated " + updated);
    Put(p, 23, 0, A_WHITE "Weather data by Open-Meteo.com");
}

static void BuildIndex(Page &p, const std::vector<Summary> &sums, const std::string &updated)
{
    Blank(p);
    Banner(p, "WEATHER");
    Put(p, 3, 0, A_CYAN "Latest conditions");
    int row = 5;
    for (size_t i = 0; i < g_places.size() && row <= 20; i++, row += 2) {
        std::string line = A_WHITE + Fit(g_places[i].name, 13);
        if (sums[i].ok) {
            line += A_YELLOW + RightFit(sums[i].temp, 4);
            line += (char)sums[i].colour;
            line += Fit(sums[i].text, 15);
        } else {
            line += A_RED + Fit("no data", 20);
        }
        line += A_CYAN + Hex3(g_places[i].page);
        Put(p, row, 0, line);
    }
    Footer(p, updated);
    Put(p, 24, 0, A_RED "First place  " A_CYAN "Index");
}

/* ------------------------------------------------------------------ */
/*  Update cycle                                                       */
/* ------------------------------------------------------------------ */

static std::string UrlEncode(const std::string &s)
{
    std::string r;
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.') r += (char)c;
        else { char b[4]; sprintf(b, "%%%02X", c); r += b; }
    }
    return r;
}

static void UpdateAll()
{
    time_t now = time(NULL);
    char updated[8];
    strftime(updated, sizeof(updated), "%H:%M", localtime(&now));
    printf("\n--- Updating at %s ---\n", updated);

    std::vector<Summary> sums;
    size_t n = g_places.size();
    for (size_t i = 0; i < n; i++) {
        const Place &pl = g_places[i];
        std::string url = g_url;
        ReplaceAll(url, "{lat}", pl.lat);
        ReplaceAll(url, "{lon}", pl.lon);
        ReplaceAll(url, "{tz}", UrlEncode(g_timezone));
        ReplaceAll(url, "{name}", pl.name);

        printf("%-14s ", pl.name.c_str());
        std::string body;
        int status = Fetch(url, body);
        Json j;
        size_t k = 0;
        if (status != 200) {
            printf(status ? "HTTP %d - keeping the old page\n" : "no connection - keeping the old page\n", status);
            Summary s; s.ok = false; s.colour = 7;
            sums.push_back(s);
            continue;
        }
        if (!ParseValue(body, k, "", j, 0)) {
            printf("reply is not JSON - keeping the old page\n");
            Summary s; s.ok = false; s.colour = 7;
            sums.push_back(s);
            continue;
        }
        Page p;
        Summary s = BuildPlace(p, pl, j);
        Footer(p, updated);
        Put(p, 24, 0, A_RED "Next place  " A_GREEN "Previous  " A_YELLOW "Weather  " A_CYAN "Index");
        int links[5];
        links[0] = g_places[(i + 1) % n].page;
        links[1] = g_places[(i + n - 1) % n].page;
        links[2] = g_indexPage;
        links[3] = 0x100;
        links[4] = 0x100;
        bool changed = WriteIfChanged(pl.page, MakeTTI(p, pl.page, "Weather " + pl.name, links));
        printf("%s %s%s\n", s.temp.c_str(), s.text.c_str(), changed ? ", page updated" : "");
        sums.push_back(s);
        SleepMs(300);
    }

    Page idx;
    BuildIndex(idx, sums, updated);
    int links[5] = { g_places[0].page, 0, 0, 0x100, 0x100 };
    WriteIfChanged(g_indexPage, MakeTTI(idx, g_indexPage, "Weather index", links));
}

int main()
{
    setvbuf(stdout, NULL, _IOLBF, 1024);
    LoadConfig();
    MakeDir(g_pagesDir.c_str());

    printf("=== Teletext weather (data by Open-Meteo.com) ===\n");
    printf("Writing pages to %s (index %03X), every %d minutes\n",
           g_pagesDir.c_str(), g_indexPage, g_interval);
    for (size_t i = 0; i < g_places.size(); i++)
        printf("   %03X  %-14s %s, %s\n", g_places[i].page, g_places[i].name.c_str(),
               g_places[i].lat.c_str(), g_places[i].lon.c_str());
    printf("U = update now   Q = quit\n");

    time_t next = 0;
    for (;;) {
        bool force = false;
        int k;
        while (KeyPressed(k)) {
            if (k == 'q' || k == 'Q') return 0;
            if (k == 'u' || k == 'U') force = true;
        }
        time_t now = time(NULL);
        if (force || now >= next) {
            UpdateAll();
            printf("Next update in %d minutes. U = update now, Q = quit\n", g_interval);
            next = now + g_interval * 60;
        }
        SleepMs(500);
    }
}
