/*
 * d2port-importar: copia al paquete del port los archivos del juego que
 * necesita, sacándolos de tu propia instalación de Diablo II: Resurrected
 * (el almacenamiento CASC, con CascLib). El port no trae ningún archivo de
 * Blizzard: cada uno los pone desde su copia del juego.
 *
 *   d2port-importar [--idioma esMX|enUS|deDE|...] [--juego <carpeta del D2R>] [--destino <carpeta>] [--sin-pausa]
 *                   [--sin-cinematicas] [--steam [--usuario <cuenta>] [--descarga <carpeta>] [--conservar-descarga]]
 *
 * Sin argumentos pregunta lo que falta. El destino es la carpeta `ports` del
 * paquete (o su `.data/d2port`): ahí está `archivos.txt`, la lista de lo que
 * usa el port. También puede ser la carpeta del código del port (la del
 * repositorio, con CMakeLists.txt): la lista es tools/archivos-del-juego.txt
 * y los archivos van a su `data/`, que git no sube, para compilarlo y
 * jugarlo desde el código. Solo se copian los de la lista, no el juego
 * entero. Las voces salen del idioma elegido (uno de los trece del
 * Resurrected: las del español latino, de locales/audio/esmx; las del
 * inglés, de la ruta de siempre) y el idioma queda anotado en `idioma.txt`
 * ("esMX", "deDE"...), que el juego lee al arrancar. Las cinemáticas se convierten aparte, con ffmpeg
 * (import_movies), salvo con --sin-cinematicas.
 *
 * Compila para Windows (mingw), Linux y la Pi (aarch64); ver
 * scripts/package-release.sh.
 */
#include <CascLib.h>

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <glob.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

static int s_en; /* los mensajes en inglés (en todos los idiomas que no son español) */

/* Los idiomas del Resurrected: el código (el de los .json y el de idioma.txt) y su nombre */
static const struct {
    const char *code, *name;
} k_langs[] = {
    { "esMX", "Español (Latinoamérica)" }, { "enUS", "English" },  { "esES", "Español (España)" },
    { "deDE", "Deutsch" },                 { "frFR", "Français" }, { "itIT", "Italiano" },
    { "ptBR", "Português (Brasil)" },      { "plPL", "Polski" },   { "ruRU", "Русский" },
    { "jaJP", "日本語" },                  { "koKR", "한국어" },   { "zhCN", "简体中文" },
    { "zhTW", "繁體中文" },
};
#define NUM_LANGS ((int)(sizeof k_langs / sizeof k_langs[0]))

/* "deDE", "dede" (o "es" y "en", los de antes) -> su número; -1 = no está */
static int lang_index(const char *s)
{
    if (!strcmp(s, "es"))
        return 0;
    if (!strcmp(s, "en"))
        return 1;
    for (int i = 0; i < NUM_LANGS; i++) {
        int same = strlen(s) == 4;
        for (int k = 0; same && k < 4; k++)
            same = tolower((unsigned char)s[k]) == tolower((unsigned char)k_langs[i].code[k]);
        if (same)
            return i;
    }
    return -1;
}

#define T(es, en) (s_en ? (en) : (es))

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void chomp(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' || s[n - 1] == '"'))
        s[--n] = '\0';
    if (s[0] == '"') /* (arrastrar una carpeta a la consola la pone entre comillas) */
        memmove(s, s + 1, strlen(s));
}

/* Una carpeta arrastrada a la consola: entre comillas simples en Linux (Konsole, la de GNOME), como
   file:///... con %20 en los espacios en otras, o escrita como en la consola (Diablo\ II\ Resurrected) */
static void unquote_path(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == '\'' || s[n - 1] == ' '))
        s[--n] = '\0';
    int quoted = s[0] == '\'';
    if (quoted)
        memmove(s, s + 1, n--);
    if (strncmp(s, "file://", 7) == 0) {
        char *w = s;
        for (const char *r = s + 7; *r; r++) {
            if (r[0] == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
                char hex[3] = { r[1], r[2], '\0' };
                *w++ = (char)strtol(hex, NULL, 16);
                r += 2;
            } else
                *w++ = *r;
        }
        *w = '\0';
        quoted = 1;
    }
#ifndef _WIN32
    if (!quoted) {
        char *w = s;
        for (const char *r = s; *r; r++) {
            if (r[0] == '\\' && r[1])
                r++;
            *w++ = *r;
        }
        *w = '\0';
    }
#endif
}

static void ask(const char *prompt, char *out, size_t cap)
{
    printf("%s", prompt);
    fflush(stdout);
    if (!fgets(out, (int)cap, stdin))
        out[0] = '\0';
    chomp(out);
    unquote_path(out);
}

static void join(char *out, size_t cap, const char *a, const char *b)
{
    size_t n = strlen(a);
    if (snprintf(out, cap, "%s%s%s", a, n && (a[n - 1] == '/' || a[n - 1] == '\\') ? "" : "/", b) >= (int)cap)
        out[0] = '\0'; /* (una ruta de más de 1 KB: mejor que no exista a que sea otra) */
}

/* "data:data/global/excel/armor.txt" -> "data/global/excel/armor.txt", en minúsculas */
static void normalize(const char *in, char *out, size_t cap)
{
    const char *colon = strchr(in, ':');
    if (colon)
        in = colon + 1;
    size_t i = 0;
    for (; *in && i + 1 < cap; in++, i++)
        out[i] = *in == '\\' ? '/' : (char)tolower((unsigned char)*in);
    out[i] = '\0';
}

static int mkdirs_for(const char *path)
{
    char tmp[2048];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\')
            continue;
        char c = *p;
        *p = '\0';
        /* (el error de cada uno no importa: "C:" o el servidor de una ruta de
           red como \\BATOCERA\share no se crean; si falta algo, falla al
           abrir el archivo) */
        if (!(p > tmp && p[-1] == ':'))
            MKDIR(tmp);
        *p = c;
    }
    return 0;
}

/* ---- La lista de archivos (archivos.txt) en una tabla hash ---- */

typedef struct {
    char *path;      /* "data/..." como lo busca el port */
    char *casc_name; /* de dónde sale en el CASC */
    int   prio;      /* 1 = la ruta de siempre, 2 = la del idioma */
} Entry;

static Entry   *s_entries;
static int      s_num;
static int     *s_table;
static unsigned s_mask;

static unsigned hash_str(const char *s)
{
    unsigned h = 2166136261u;
    for (; *s; s++)
        h = (h ^ (unsigned char)*s) * 16777619u;
    return h;
}

static Entry *lookup(const char *path)
{
    for (unsigned i = hash_str(path) & s_mask; s_table[i] >= 0; i = (i + 1) & s_mask)
        if (strcmp(s_entries[s_table[i]].path, path) == 0)
            return &s_entries[s_table[i]];
    return NULL;
}

static int load_list(const char *file)
{
    FILE *f = fopen(file, "r");
    if (!f)
        return -1;
    int cap = 1024;
    s_entries = malloc((size_t)cap * sizeof *s_entries);
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        chomp(line);
        if (!line[0])
            continue;
        if (s_num == cap) {
            cap *= 2;
            s_entries = realloc(s_entries, (size_t)cap * sizeof *s_entries);
        }
        char norm[1024];
        normalize(line, norm, sizeof norm);
        s_entries[s_num].path = strdup(norm);
        s_entries[s_num].casc_name = NULL;
        s_entries[s_num].prio = 0;
        s_num++;
    }
    fclose(f);
    unsigned size = 1;
    while (size < (unsigned)s_num * 2u)
        size *= 2;
    s_mask = size - 1;
    s_table = malloc(size * sizeof *s_table);
    memset(s_table, 0xff, size * sizeof *s_table);
    for (int k = 0; k < s_num; k++) {
        unsigned i = hash_str(s_entries[k].path) & s_mask;
        while (s_table[i] >= 0)
            i = (i + 1) & s_mask;
        s_table[i] = k;
    }
    return 0;
}

/* ---- Dónde está cada cosa ---- */

static int is_game_dir(const char *dir)
{
    char p[1024];
    join(p, sizeof p, dir, ".build.info");
    return exists(p);
}

#include "importar-steam.inc" /* --steam: bajarlo de Steam */

/* Instalado con Steam: steamapps/common/Diablo II Resurrected en alguna de sus bibliotecas (libraryfolders.vdf
   las lista todas, también las de la tarjeta SD de la Steam Deck o de otro disco) */
static int find_in_steam(const char *steam, char *out, size_t cap)
{
    char dir[1024], line[1024], lib[1024];
    join(dir, sizeof dir, steam, "steamapps/common/Diablo II Resurrected");
    if (is_game_dir(dir)) {
        snprintf(out, cap, "%s", dir);
        return 0;
    }
    join(line, sizeof line, steam, "steamapps/libraryfolders.vdf");
    FILE *f = fopen(line, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof line, f)) {
        char *k = strstr(line, "\"path\"");
        char *a = k ? strchr(k + 6, '"') : NULL;
        char *b = a ? strrchr(a + 1, '"') : NULL;
        if (!b || b == a)
            continue;
        *b = '\0';
        char *w = lib; /* (en Windows con las barras dobladas: "D:\\SteamLibrary") */
        for (const char *r = a + 1; *r && w < lib + sizeof lib - 1; r++) {
            if (r[0] == '\\' && r[1] == '\\')
                r++;
            *w++ = *r;
        }
        *w = '\0';
        join(dir, sizeof dir, lib, "steamapps/common/Diablo II Resurrected");
        if (is_game_dir(dir)) {
            snprintf(out, cap, "%s", dir);
            fclose(f);
            return 0;
        }
    }
    fclose(f);
    return -1;
}

static int find_game(char *out, size_t cap)
{
    static const char *const k_dirs[] = {
#ifdef _WIN32
        "C:\\Program Files (x86)\\Diablo II Resurrected",
        "C:\\Program Files\\Diablo II Resurrected",
        "C:\\Battle.net\\Diablo II Resurrected",
        "D:\\Battle.net\\Diablo II Resurrected",
        "D:\\Diablo II Resurrected",
        "E:\\Battle.net\\Diablo II Resurrected",
        "E:\\Diablo II Resurrected",
        "D:\\Games\\Diablo II Resurrected",
        "C:\\Games\\Diablo II Resurrected",
#else
        "/mnt/c/Program Files (x86)/Diablo II Resurrected",
        "/mnt/d/Battle.net/Diablo II Resurrected",
        "/mnt/d/Diablo II Resurrected",
#endif
    };
#ifdef _WIN32
    for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0]; i++)
        if (is_game_dir(k_dirs[i])) {
            snprintf(out, cap, "%s", k_dirs[i]);
            return 0;
        }
    /* Donde lo instaló el Battle.net */
    char loc[1024];
    DWORD size = sizeof loc;
    if (RegGetValueA(HKEY_LOCAL_MACHINE,
                     "SOFTWARE\\WOW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Diablo II Resurrected",
                     "InstallLocation", RRF_RT_REG_SZ, NULL, loc, &size) == ERROR_SUCCESS &&
        is_game_dir(loc)) {
        snprintf(out, cap, "%s", loc);
        return 0;
    }
    /* Con Steam */
    size = sizeof loc;
    if (RegGetValueA(HKEY_CURRENT_USER, "Software\\Valve\\Steam", "SteamPath", RRF_RT_REG_SZ, NULL, loc, &size) ==
            ERROR_SUCCESS &&
        find_in_steam(loc, out, cap) == 0)
        return 0;
    if (find_in_steam("C:\\Program Files (x86)\\Steam", out, cap) == 0)
        return 0;
#else
    /* En Linux, adentro del prefijo de Wine donde se instaló el Battle.net
       (Wine, Lutris, Bottles o Steam con Proton) */
    const char *home = getenv("HOME");
    static const char *const k_prefixes[] = {
        "%s/.wine",
        "%s/Games/*",
        "%s/.local/share/bottles/bottles/*",
        "%s/.var/app/com.usebottles.bottles/data/bottles/bottles/*",
        "%s/.steam/steam/steamapps/compatdata/*/pfx",
        "%s/.local/share/Steam/steamapps/compatdata/*/pfx",
        "%s/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/compatdata/*/pfx",
    };
    for (size_t i = 0; home && i < sizeof k_prefixes / sizeof k_prefixes[0]; i++) {
        static const char *const k_pf[] = { "Program Files (x86)", "Program Files" };
        for (int k = 0; k < 2; k++) {
            char prefix[512], pattern[1024];
            snprintf(prefix, sizeof prefix, k_prefixes[i], home);
            snprintf(pattern, sizeof pattern, "%s/drive_c/%s/Diablo II Resurrected", prefix, k_pf[k]);
            glob_t g;
            if (glob(pattern, 0, NULL, &g) == 0) {
                for (size_t j = 0; j < g.gl_pathc; j++)
                    if (is_game_dir(g.gl_pathv[j])) {
                        snprintf(out, cap, "%s", g.gl_pathv[j]);
                        globfree(&g);
                        return 0;
                    }
                globfree(&g);
            }
        }
    }
    /* Con Steam (la Steam Deck) */
    static const char *const k_steams[] = {
        "%s/.local/share/Steam",
        "%s/.steam/steam",
        "%s/.var/app/com.valvesoftware.Steam/.local/share/Steam",
    };
    for (size_t i = 0; home && i < sizeof k_steams / sizeof k_steams[0]; i++) {
        char steam[512];
        snprintf(steam, sizeof steam, k_steams[i], home);
        if (find_in_steam(steam, out, cap) == 0)
            return 0;
    }
    /* Más adentro de una tarjeta SD o un disco: /run/media/deck/MicroSD/Games/Diablo II Resurrected */
    static const char *const k_deep[] = { "/run/media/*/*/*/*[Dd][Ii][Aa][Bb][Ll][Oo]*",
                                          "/media/*/*/*/*[Dd][Ii][Aa][Bb][Ll][Oo]*" };
    for (size_t i = 0; i < sizeof k_deep / sizeof k_deep[0]; i++) {
        glob_t g;
        if (glob(k_deep[i], 0, NULL, &g) != 0)
            continue;
        for (size_t j = 0; j < g.gl_pathc; j++)
            if (is_game_dir(g.gl_pathv[j])) {
                snprintf(out, cap, "%s", g.gl_pathv[j]);
                globfree(&g);
                return 0;
            }
        globfree(&g);
    }
    /* En Batocera (o en Linux): una carpeta del juego copiada a un disco USB,
       al share o a la carpeta de ports, con cualquier nombre que diga Diablo */
    static const char *const k_parents[] = { "/media", "/userdata", "/userdata/roms/ports", "/userdata/roms",
                                             "/run/media", "/mnt" };
    for (size_t i = 0; i < sizeof k_parents / sizeof k_parents[0]; i++) {
        DIR *d = opendir(k_parents[i]);
        if (!d)
            continue;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            char sub[1024];
            join(sub, sizeof sub, k_parents[i], e->d_name);
            char low[256];
            normalize(e->d_name, low, sizeof low);
            if (strstr(low, "diablo") && is_game_dir(sub)) {
                snprintf(out, cap, "%s", sub);
                closedir(d);
                return 0;
            }
            /* un nivel más: /media/USB/Diablo II Resurrected */
            DIR *d2 = opendir(sub);
            if (!d2)
                continue;
            struct dirent *e2;
            while ((e2 = readdir(d2)) != NULL) {
                normalize(e2->d_name, low, sizeof low);
                char sub2[1024];
                join(sub2, sizeof sub2, sub, e2->d_name);
                if (e2->d_name[0] != '.' && strstr(low, "diablo") && is_game_dir(sub2)) {
                    snprintf(out, cap, "%s", sub2);
                    closedir(d2);
                    closedir(d);
                    return 0;
                }
            }
            closedir(d2);
        }
        closedir(d);
    }
    /* En WSL: el de Windows */
    for (size_t i = 0; i < sizeof k_dirs / sizeof k_dirs[0]; i++)
        if (is_game_dir(k_dirs[i])) {
            snprintf(out, cap, "%s", k_dirs[i]);
            return 0;
        }
#endif
    return -1;
}

/* La lista de archivos del destino: archivos.txt en el paquete,
   tools/archivos-del-juego.txt en el código */
static char s_list_name[64] = "archivos.txt";
static int  s_repo; /* el destino es el código del port */

/* La carpeta del paquete donde están d2port y archivos.txt: la que se dio
   (la de `ports` o su `.data/d2port`), o la del código del port */
static int pkg_dir(const char *dir, char *out, size_t cap)
{
    char p[1024], cm[1024];
    join(cm, sizeof cm, dir, "CMakeLists.txt");
    join(p, sizeof p, dir, "tools/archivos-del-juego.txt");
    if (exists(cm) && exists(p)) {
        snprintf(out, cap, "%s", dir);
        snprintf(s_list_name, sizeof s_list_name, "tools/archivos-del-juego.txt");
        s_repo = 1;
        return 0;
    }
    join(p, sizeof p, dir, "archivos.txt");
    if (exists(p)) {
        snprintf(out, cap, "%s", dir);
        return 0;
    }
    char sub[1024];
    join(sub, sizeof sub, dir, ".data/d2port");
    join(p, sizeof p, sub, "archivos.txt");
    if (exists(p)) {
        snprintf(out, cap, "%s", sub);
        return 0;
    }
    return -1;
}

/* Al lado del importador: <publicar>/importador/<sistema>/ -> <publicar>/batocera/ports,
   o el código si es el que se compiló en build/ */
static int find_pkg(char *out, size_t cap)
{
    char exe[1024] = "";
#ifdef _WIN32
    GetModuleFileNameA(NULL, exe, sizeof exe);
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = '\0';
#endif
    char *slash = strrchr(exe, '/');
    char *bslash = strrchr(exe, '\\');
    if (bslash > slash)
        slash = bslash;
    if (!slash)
        return -1;
    *slash = '\0';
    static const char *const k_rel[] = { "../../batocera/ports", "../batocera/ports", "batocera/ports", ".", ".." };
    for (size_t i = 0; i < sizeof k_rel / sizeof k_rel[0]; i++) {
        char p[1024];
        join(p, sizeof p, exe, k_rel[i]);
        if (pkg_dir(p, out, cap) == 0)
            return 0;
    }
    return pkg_dir(".", out, cap);
}

static int extract_one(HANDLE storage, const char *casc_name, const char *out_path)
{
    HANDLE file;
    if (!CascOpenFile(storage, casc_name, 0, CASC_OPEN_BY_NAME, &file))
        return -1;
    seg_note(file);
    ULONGLONG size = 0;
    if (!CascGetFileSize64(file, &size) || size > 0x7FFFFFFF) {
        CascCloseFile(file);
        return -1;
    }
    unsigned char *buf = malloc(size ? (size_t)size : 1);
    DWORD got = 0;
    int ok = buf && CascReadFile(file, buf, (DWORD)size, &got) && got == size;
    CascCloseFile(file);
    if (ok && mkdirs_for(out_path) == 0) {
        FILE *f = fopen(out_path, "wb");
        ok = f && fwrite(buf, 1, (size_t)size, f) == size;
        if (f && fclose(f) != 0)
            ok = 0;
    } else {
        ok = 0;
    }
    free(buf);
    return ok ? 0 : -1;
}

/* ---- Las cinemáticas ----
   No se copian tal cual: el Resurrected las trae en 1080p (VP8, cientos de
   MB cada una) con el sonido aparte por idioma. Se sacan de a una y ffmpeg
   las pasa a lo que lee el port (src/cinema.c): MPEG-1 de 640x288 (sin las
   franjas negras) con el sonido MP2 del idioma elegido. Las de la expansión:
   d2x_intro (la presentación de la expansión) y d2x_out (el final, después
   de Baal). Los logos del arranque: los dos clásicos vienen mudos, así que
   va también el video de logos del Resurrected (los mismos dos, con su
   sonido, que no depende del idioma), recortado a 4:3 y en 640x480. */
static const struct {
    const char *name, *video, *audio;
    int logo; /* 4:3 a 640x480 (los logos) */
} k_movies[] = {
    { "new_bliz", "data/global/video/new_bliz.webm", NULL, 1 },
    { "bliznorth", "data/global/video/bliznorth.webm", NULL, 1 },
    { "blizzardlogos", "data/hd/global/video/blizzardlogos.webm", "data/hd/local/video/blizzardlogos.flac", 1 },
    { "d2intro", "data/global/video/d2intro.webm", "data/local/video/d2intro.flac", 0 },
    { "act02start", "data/global/video/act2/act02start.webm", "data/local/video/act2/act02start.flac", 0 },
    { "act03start", "data/global/video/act3/act03start.webm", "data/local/video/act3/act03start.flac", 0 },
    { "act04start", "data/global/video/act4/act04start.webm", "data/local/video/act4/act04start.flac", 0 },
    { "act04end", "data/global/video/act4/act04end.webm", "data/local/video/act4/act04end.flac", 0 },
    { "d2x_intro", "data/global/video/d2x_intro.webm", "data/local/video/d2x_intro.flac", 0 },
    { "d2x_out", "data/global/video/act5/d2x_out.webm", "data/local/video/act5/d2x_out.flac", 0 },
};
#define NUM_MOVIES ((int)(sizeof k_movies / sizeof k_movies[0]))
static char *s_movie_video[NUM_MOVIES], *s_movie_audio[NUM_MOVIES]; /* sus nombres en el CASC */
static int   s_movie_audio_prio[NUM_MOVIES];

#ifdef _WIN32
#define NULL_OUT " >NUL 2>&1"
#else
#define NULL_OUT " >/dev/null 2>&1"
#endif

/* Uno grande, de a 1 MB (los videos no entran enteros en la memoria de la Pi) */
static int extract_stream(HANDLE storage, const char *casc_name, const char *out_path)
{
    static unsigned char buf[1 << 20];
    HANDLE file;
    if (!CascOpenFile(storage, casc_name, 0, CASC_OPEN_BY_NAME, &file))
        return -1;
    seg_note(file);
    FILE *f = mkdirs_for(out_path) == 0 ? fopen(out_path, "wb") : NULL;
    int ok = f != NULL;
    while (ok) {
        DWORD got = 0;
        if (!CascReadFile(file, buf, (DWORD)sizeof buf, &got))
            ok = 0;
        else if (got == 0)
            break;
        else if (fwrite(buf, 1, got, f) != got)
            ok = 0;
    }
    CascCloseFile(file);
    if (f && fclose(f) != 0)
        ok = 0;
    if (!ok)
        remove(out_path);
    return ok ? 0 : -1;
}

static void import_movies(HANDLE storage, const char *pkg, const char *lang)
{
    char dir[1024], mark[1024], old[8] = "";
    join(dir, sizeof dir, pkg, "data/global/video");
    join(mark, sizeof mark, dir, "idioma.txt");
    FILE *mf = fopen(mark, "r");
    if (mf) {
        if (!fgets(old, sizeof old, mf))
            old[0] = '\0';
        chomp(old);
        fclose(mf);
    }
    printf("%s", T("\nCinemáticas: ", "\nCinematics: "));
    if (system("ffmpeg -version" NULL_OUT) != 0) {
        printf("%s", T("no está ffmpeg, así que no se convierten (el juego anda igual, sin ellas).\n"
                       "  Para tenerlas: instalá ffmpeg (en Batocera ya viene) y volvé a correr el importador.\n",
                       "ffmpeg is not installed, so they are not converted (the game works without them).\n"
                       "  To get them: install ffmpeg (Batocera already has it) and run the importer again.\n"));
        return;
    }
    printf("%s", T("se convierten con ffmpeg a calidad baja (unos 190 MB; en la Pi tarda más de una hora).\n",
                   "converting them with ffmpeg to low quality (about 190 MB; on the Pi it takes over an hour).\n"));
    int ok_all = 1;
    for (int i = 0; i < NUM_MOVIES; i++) {
        char out[1024], tmp_out[1024], tmp_video[1024], tmp_audio[1024], file[64], cmd[4096];
        snprintf(file, sizeof file, "%s.mpg", k_movies[i].name);
        join(out, sizeof out, dir, file);
        /* ya convertida (con el sonido de este idioma): no hace falta otra vez */
        if (exists(out) && (!k_movies[i].audio || k_movies[i].logo || strcmp(old, lang) == 0)) {
            printf(T("  %s: ya estaba\n", "  %s: already there\n"), k_movies[i].name);
            continue;
        }
        if (!s_movie_video[i] || (k_movies[i].audio && !s_movie_audio[i])) {
            printf(T("  %s: no está en el juego\n", "  %s: not in the game\n"), k_movies[i].name);
            ok_all = 0;
            continue;
        }
        printf(T("  %s (%d de %d)...\n", "  %s (%d of %d)...\n"), k_movies[i].name, i + 1, NUM_MOVIES);
        fflush(stdout);
        snprintf(file, sizeof file, "%s.tmp.webm", k_movies[i].name);
        join(tmp_video, sizeof tmp_video, dir, file);
        snprintf(file, sizeof file, "%s.tmp.flac", k_movies[i].name);
        join(tmp_audio, sizeof tmp_audio, dir, file);
        snprintf(file, sizeof file, "%s.tmp.mpg", k_movies[i].name);
        join(tmp_out, sizeof tmp_out, dir, file);
        int ok = extract_stream(storage, s_movie_video[i], tmp_video) == 0 &&
                 (!k_movies[i].audio || extract_stream(storage, s_movie_audio[i], tmp_audio) == 0);
        if (ok && k_movies[i].audio && k_movies[i].logo) /* el de logos del Resurrected: 1080p, al medio el 4:3 */
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -v error -y -i \"%s\" -i \"%s\" -map 0:v -map 1:a"
                     " -vf crop=1440:1080:240:0,scale=640:480:flags=bicubic -r 24"
                     " -c:v mpeg1video -q:v 4 -g 24 -bf 2 -c:a mp2 -b:a 128k -ac 2 -ar 44100 -f mpeg \"%s\"",
                     tmp_video, tmp_audio, tmp_out);
        else if (ok && k_movies[i].audio)
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -v error -y -i \"%s\" -i \"%s\" -map 0:v -map 1:a"
                     " -vf crop=1920:864:0:108,scale=640:288:flags=bicubic -r 24"
                     " -c:v mpeg1video -q:v 5 -g 24 -bf 2 -c:a mp2 -b:a 128k -ac 2 -ar 44100 -f mpeg \"%s\"",
                     tmp_video, tmp_audio, tmp_out);
        else if (ok) /* los logos no tienen sonido */
            snprintf(cmd, sizeof cmd,
                     "ffmpeg -v error -y -i \"%s\" -an -vf scale=640:480:flags=bicubic -r 24"
                     " -c:v mpeg1video -q:v 4 -g 24 -bf 2 -f mpeg \"%s\"",
                     tmp_video, tmp_out);
        ok = ok && system(cmd) == 0;
        remove(tmp_video);
        remove(tmp_audio);
        if (ok) {
            remove(out);
            ok = rename(tmp_out, out) == 0;
        }
        if (!ok) {
            remove(tmp_out);
            printf(T("  %s: no se pudo convertir\n", "  %s: could not convert it\n"), k_movies[i].name);
            ok_all = 0;
        }
    }
    if (ok_all && (mf = fopen(mark, "w")) != NULL) {
        fprintf(mf, "%s\n", lang);
        fclose(mf);
    }
}

static void pause_exit(int pause)
{
    if (!pause)
        return;
    char tmp[8];
    printf("%s", T("\nPresioná Enter para salir...", "\nPress Enter to exit..."));
    fflush(stdout);
    if (!fgets(tmp, sizeof tmp, stdin))
        return;
}

int main(int argc, char **argv)
{
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
#endif
    char lang[8] = "", game[1024] = "", dest[1024] = "", pkg[1024], user[128] = "", dl[1024] = "";
    int pause = 1, movies = 1, steam = 0, keep_dl = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--idioma") == 0 && i + 1 < argc)
            snprintf(lang, sizeof lang, "%s", argv[++i]);
        else if (strcmp(argv[i], "--juego") == 0 && i + 1 < argc)
            snprintf(game, sizeof game, "%s", argv[++i]);
        else if (strcmp(argv[i], "--destino") == 0 && i + 1 < argc)
            snprintf(dest, sizeof dest, "%s", argv[++i]);
        else if (strcmp(argv[i], "--sin-pausa") == 0)
            pause = 0;
        else if (strcmp(argv[i], "--sin-cinematicas") == 0)
            movies = 0;
        else if (strcmp(argv[i], "--steam") == 0)
            steam = 1;
        else if (strcmp(argv[i], "--usuario") == 0 && i + 1 < argc)
            snprintf(user, sizeof user, "%s", argv[++i]);
        else if (strcmp(argv[i], "--descarga") == 0 && i + 1 < argc)
            snprintf(dl, sizeof dl, "%s", argv[++i]);
        else if (strcmp(argv[i], "--conservar-descarga") == 0)
            keep_dl = 1;
        else if (strcmp(argv[i], "--ui") == 0)
            s_ui = 1;
        else if (strcmp(argv[i], "--prueba-steam") == 0)
            return steam_self_test(argc - i - 1, argv + i + 1);
        else {
            printf("uso / usage: %s [--idioma esMX|enUS|esES|deDE|frFR|itIT|ptBR|plPL|ruRU|jaJP|koKR|zhCN|zhTW]"
                   " [--juego <Diablo II Resurrected>] [--destino <ports | código / source>]"
                   " [--sin-pausa] [--sin-cinematicas]\n"
                   "     [--steam [--usuario <cuenta / account>] [--descarga <carpeta / folder>] [--conservar-descarga]]\n",
                   argv[0]);
            return 2;
        }
    }
    if (s_ui) { /* (la pantalla de la Pi: nadie contesta preguntas, y lee cada renglón en cuanto sale) */
        pause = 0;
        setvbuf(stdout, NULL, _IOLBF, 0);
    }

    printf("Diablo II port - importador / importer\n\n");
    int li = lang_index(lang);
    while (li < 0) {
        char ans[32];
        printf("Idioma del juego / Game language:\n");
        for (int i = 0; i < NUM_LANGS; i++)
            printf("  %2d) %s\n", i + 1, k_langs[i].name);
        ask("> ", ans, sizeof ans);
        if (!ans[0] && feof(stdin))
            return 2;
        int num = atoi(ans);
        li = num >= 1 && num <= NUM_LANGS ? num - 1 : lang_index(ans);
    }
    snprintf(lang, sizeof lang, "%s", k_langs[li].code);
    s_en = strncmp(lang, "es", 2) != 0;

    /* El juego original (con --steam, después: se baja a la carpeta del paquete) */
    if (!steam && !game[0] && find_game(game, sizeof game) == 0)
        printf(T("Diablo II: Resurrected encontrado en: %s\n", "Found Diablo II: Resurrected in: %s\n"), game);
    while (!steam && !is_game_dir(game)) {
        if (game[0])
            printf(T("No está el juego en \"%s\" (falta .build.info).\n", "The game is not in \"%s\" (no .build.info).\n"), game);
        ask(T("Carpeta de Diablo II: Resurrected (podés arrastrarla acá): ",
              "Diablo II: Resurrected folder (you can drag it here): "),
            game, sizeof game);
        if (!game[0] && feof(stdin))
            return 2;
    }

    /* El paquete */
    if (dest[0] ? pkg_dir(dest, pkg, sizeof pkg) != 0 : find_pkg(pkg, sizeof pkg) != 0) {
        for (;;) {
            if (dest[0])
                printf(T("En \"%s\" no está el paquete ni el código del port (falta archivos.txt).\n",
                         "\"%s\" is neither the port package nor its source (no archivos.txt).\n"),
                       dest);
            ask(T("Carpeta `ports` del paquete (la de Batocera o la que copiaste), o la del código del port: ",
                  "The package's `ports` folder (on Batocera or where you copied it), or the port's source folder: "),
                dest, sizeof dest);
            if (!dest[0] && feof(stdin))
                return 2;
            if (pkg_dir(dest, pkg, sizeof pkg) == 0)
                break;
        }
    }
    if (s_repo)
        printf(T("Código del port: %s (los archivos van a su data/)\n", "Port source: %s (the files go to its data/)\n"), pkg);
    else
        printf(T("Paquete: %s\n", "Package: %s\n"), pkg);

    char list[1024], win_exe[1024];
    join(list, sizeof list, pkg, s_list_name);
    join(win_exe, sizeof win_exe, pkg, "d2port.exe");
    int win_pkg = exists(win_exe); /* el paquete de Windows: d2port.exe al lado de archivos.txt */
    char web_wasm[1024];
    join(web_wasm, sizeof web_wasm, pkg, "d2port.wasm");
    int web_pkg = exists(web_wasm); /* la versión para el navegador: d2port.wasm al lado de archivos.txt */
    if (load_list(list) != 0 || s_num == 0) {
        printf(T("No pude leer %s\n", "Could not read %s\n"), list);
        pause_exit(pause);
        return 1;
    }

    /* Desde Steam: bajarlo a <paquete>/steam/descarga (o --descarga) */
    if (steam) {
        int dl_fixed = dl[0] != 0;
        if (!dl_fixed) {
            char sd[1024];
            join(sd, sizeof sd, pkg, "steam");
            join(dl, sizeof dl, sd, "descarga");
        }
        if (steam_download(pkg, lang, user[0] ? user : NULL, dl, sizeof dl, dl_fixed, game, sizeof game) != 0) {
            pause_exit(pause);
            return 1;
        }
        s_seg_on = 1;
        ui_phase(T("Copiando los archivos del juego", "Copying the game files"));
    }

    HANDLE storage = NULL;
    if (!CascOpenStorage(game, CASC_LOCALE_ALL, &storage)) {
        if (steam)
            printf(T("No pude abrir los archivos bajados de Steam en %s (error %d). Quedan ahí; mandá esta salida a "
                     "quien te pasó el port.\n",
                     "Could not open the files downloaded from Steam in %s (error %d). They stay there; send this output "
                     "to whoever gave you the port.\n"),
                   game, (int)GetCascError());
        else
            printf(T("No pude abrir los archivos del juego en %s (error %d). ¿Está cerrado el juego y el Battle.net "
                     "terminó de instalarlo?\n",
                     "Could not open the game files in %s (error %d). Is the game closed and fully installed?\n"),
                   game, (int)GetCascError());
        pause_exit(pause);
        return 1;
    }

    /* Dónde está cada uno: la ruta de siempre (las voces en inglés) o la del
       idioma elegido (locales/audio/esmx, locales/audio/dede...) */
    printf("%s", T("Buscando los archivos...\n", "Looking for the files...\n"));
    CASC_FIND_DATA fd;
    HANDLE find = CascFindFirstFile(storage, "*", &fd, NULL);
    if (find == INVALID_HANDLE_VALUE || find == NULL) {
        printf(T("No pude recorrer los archivos del juego (error %d).\n", "Could not list the game files (error %d).\n"),
               (int)GetCascError());
        pause_exit(pause);
        return 1;
    }
    char loc[32] = ""; /* (en inglés, ninguna: las de siempre) */
    if (strcmp(lang, "enUS") != 0) {
        snprintf(loc, sizeof loc, "locales/audio/%s/", lang);
        for (char *c = loc; *c; c++)
            *c = (char)tolower((unsigned char)*c);
    }
    do {
        char name[1024];
        normalize(fd.szFileName, name, sizeof name);
        if (!fd.bFileAvailable)
            continue;
        /* la del idioma elegido gana; la otra queda si es la única */
        int local = loc[0] && strncmp(name, loc, strlen(loc)) == 0;
        int prio = local ? 2 : 1;
        Entry *e = lookup(local ? name + strlen(loc) : name);
        if (e && prio > e->prio) {
            free(e->casc_name);
            e->casc_name = strdup(fd.szFileName);
            e->prio = prio;
        }
        /* las cinemáticas: el video, y el sonido con la misma regla del idioma */
        const char *bare = local ? name + strlen(loc) : name;
        for (int i = 0; i < NUM_MOVIES; i++) {
            if (!local && !s_movie_video[i] && strcmp(bare, k_movies[i].video) == 0)
                s_movie_video[i] = strdup(fd.szFileName);
            if (k_movies[i].audio && strcmp(bare, k_movies[i].audio) == 0 && prio > s_movie_audio_prio[i]) {
                free(s_movie_audio[i]);
                s_movie_audio[i] = strdup(fd.szFileName);
                s_movie_audio_prio[i] = prio;
            }
        }
    } while (CascFindNextFile(find, &fd));
    CascFindClose(find);

    /* Copiarlos */
    long done = 0, missing = 0, failed = 0;
    FILE *log = NULL;
    char log_path[1024];
    join(log_path, sizeof log_path, pkg, "importar.log");
    for (int k = 0; k < s_num; k++) {
        Entry *e = &s_entries[k];
        char out_path[2048];
        join(out_path, sizeof out_path, pkg, e->path);
        if (!e->casc_name) {
            missing++;
            if (!log)
                log = fopen(log_path, "w");
            if (log)
                fprintf(log, "falta / missing: %s\n", e->path);
            continue;
        }
        if (extract_one(storage, e->casc_name, out_path) != 0) {
            failed++;
            if (!log)
                log = fopen(log_path, "w");
            if (log)
                fprintf(log, "error: %s\n", e->path);
            continue;
        }
        if (++done % 1000 == 0)
            printf(T("  %ld de %d...\n", "  %ld of %d...\n"), done, s_num);
        if (done % 250 == 0)
            ui_progress(100.0 * done / s_num);
    }
    if (movies) {
        if (s_ui)
            ui_phase(T("Convirtiendo las cinemáticas (en la Pi, más de una hora)", "Converting the cinematics (over an hour on the Pi)"));
        import_movies(storage, pkg, lang);
    }
    CascCloseStorage(storage);
    if (log)
        fclose(log);

    /* Desde Steam: cuánto de lo bajado se usó (importar-steam.log) y borrar la descarga */
    if (steam) {
        char slog_path[1024];
        join(slog_path, sizeof slog_path, pkg, "importar-steam.log");
        FILE *slog = fopen(slog_path, "w");
        seg_report(game, slog);
        if (slog)
            fclose(slog);
        if (!missing && !failed && !keep_dl) {
            long long size = 0;
            walk(dl, add_size, &size, 0);
            ui_phase(T("Borrando la descarga", "Deleting the download"));
            remove_download(dl);
            printf(T("Borrada la descarga de Steam (%.1f GB).\n", "Deleted the Steam download (%.1f GB).\n"), size / 1073741824.0);
        } else if (missing || failed) {
            printf(T("La descarga queda en %s (para volver a importar sin bajarla de nuevo).\n",
                     "The download stays in %s (to import again without downloading it again).\n"),
                   dl);
        }
    }

    char lang_path[1024];
    join(lang_path, sizeof lang_path, pkg, "idioma.txt");
    FILE *lf = fopen(lang_path, "w");
    if (lf) {
        fprintf(lf, "%s\n", lang);
        fclose(lf);
    }

    printf(T("\nCopiados: %ld de %d", "\nCopied: %ld of %d"), done, s_num);
    if (missing || failed)
        printf(T(" (faltan %ld, con error %ld: ver %s)", " (%ld missing, %ld failed: see %s)"), missing, failed, log_path);
    printf("\n");
    if (!missing && !failed && s_repo)
        printf("%s", T("Listo. Ya se puede compilar y jugar desde el código:\n"
                       "  cmake -S . -B build && cmake --build build -j && ./build/d2port\n",
                       "Done. Now you can build and play from the source:\n"
                       "  cmake -S . -B build && cmake --build build -j && ./build/d2port\n"));
    else if (!missing && !failed && web_pkg)
        printf("%s", T("Listo. Para jugar en el navegador, abrí d2port-servidor de esta carpeta (en Windows,\n"
                       "d2port-servidor.exe).\n",
                       "Done. To play in the browser, open d2port-servidor in this folder (on Windows,\n"
                       "d2port-servidor.exe).\n"));
    else if (!missing && !failed && win_pkg)
        printf("%s", T("Listo. Para jugar, abrí d2port.exe (en esta carpeta).\n",
                       "Done. To play, open d2port.exe (in this folder).\n"));
    else if (!missing && !failed)
        printf("%s", T("Listo. Si no importaste directo a la Pi, copiá el contenido de esta carpeta `ports` a la\n"
                       "de Batocera: /userdata/roms/ports (por la red, smb://batocera/share/roms/ports).\n",
                       "Done. If you did not import straight to the Pi, copy the contents of this `ports` folder\n"
                       "to Batocera's: /userdata/roms/ports (over the network, smb://batocera/share/roms/ports).\n"));
    pause_exit(pause);
    return missing || failed ? 1 : 0;
}
