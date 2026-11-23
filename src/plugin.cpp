// mtn.mediaview - picture and video viewer for MTN2 on libmpv.
//
// F3 on a media file opens a viewer tab whose area is a native window of the
// plugin (surface_open_ex + 256); libmpv renders into it (--wid). Every tab is
// a Player of its own with its own mpv instance, which lives as long as the
// tab; paging through the folder reuses it.
//
// Threading: the host calls us on its main thread only, and our view window
// lives on that thread too. mpv wakes us from its own threads, so the wakeup
// callback only posts a message to the view window; events are drained and
// every host function is called from the window procedure. mpv creates its
// video window on its VO thread - that window is subclassed and its mouse
// messages are posted to the view window.
//
// Zoom / pan: video-zoom is log2 of the scale relative to mpv's fit-to-window
// size, video-pan-x/y move the picture by a fraction of its scaled size.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <mmsystem.h>
#include <shlwapi.h>
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shlwapi.lib")

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <map>
#include <string>
#include <vector>

#include "mtn_plugin.h"
#include "mpv_api.h"

namespace {

constexpr const char *kPluginId = "mtn.mediaview";
constexpr const wchar_t *kViewClass = L"MtnMediaView";
constexpr const wchar_t *kChildProp = L"MtnMediaViewProc";
constexpr UINT WM_MPV_WAKEUP = WM_APP + 1;
constexpr UINT WM_CLOSE_TAB = WM_APP + 2;

const char *kVideoExts[] = {
  "3gp", "3g2", "avi", "divx", "f4v", "flv", "h264", "h265", "hevc", "m2ts", "m4v", "mkv", "mov", "mp4",
  "mpg", "mpeg", "mts", "mxf", "ogm", "ogv", "rm", "rmvb", "ts", "vob", "webm", "wmv", "y4m" };
const char *kImageExts[] = {
  "jpg", "jpeg", "jfif", "png", "bmp", "gif", "apng", "webp", "tif", "tiff", "avif", "heic", "heif", "jxl",
  "tga", "ico", "psd", "dds", "exr", "hdr", "pcx", "ppm", "pgm" };
// Animated pictures loop instead of stopping on their last frame.
const char *kLoopExts[] = { "gif", "apng", "webp" };

const int kRates[] = { 250, 333, 500, 667, 750, 1000, 1250, 1500, 2000, 3000, 4000 };

enum EndAction { END_STOP = 0, END_REPEAT = 1, END_NEXT = 2 };
const char *kEndNames[] = { "Stop", "Repeat", "Next" };

enum { OBS_TIME_POS = 1, OBS_DURATION, OBS_PAUSE, OBS_EOF, OBS_SPEED,
       OBS_VP_DW, OBS_VP_DH, OBS_VP_ROTATE, OBS_VOLUME, OBS_MUTE };

const MtnHostApi *g_host = nullptr;
HINSTANCE g_module = nullptr;
wchar_t   g_pluginDir[MAX_PATH];
MpvApi    g_mpv;
bool      g_classRegistered = false;

// One file of the list. A local file has a path and mpv opens it by name;
// any other (inside an archive, sftp) is read through the host, see the cache.
struct FileEntry {
  std::string uri;
  std::wstring name;
  std::wstring path;
  bool selected = false;    // marked in the panel (Insert)
  bool local() const { return !path.empty(); }
};

// A file the host has opened for us (vfs_open), see the cache further down.
// Closed when the last user lets go - the cache or a stream mpv still reads,
// on whichever thread that happens (vfs_close works from any).
struct RemoteFile {
  int64_t handle;
  int64_t size;
  RemoteFile(int64_t h, int64_t sz) : handle(h), size(sz) {}
  ~RemoteFile();
  RemoteFile(const RemoteFile &) = delete;
  RemoteFile &operator=(const RemoteFile &) = delete;
};
typedef std::shared_ptr<RemoteFile> Block;
// No data yet = the host is opening the file, and job is its vfs_open job.
struct CacheItem { int id = 0; Block data; int64_t job = 0; };

// One viewer tab.
struct Player {
  int id = 0;               // never reused: late answers of the host find their tab by it
  int64_t surface = 0;
  HWND view = nullptr;
  mpv_handle *mpv = nullptr;
  HWND mpvChild = nullptr;
  bool closing = false;     // Закрытие отложено до возврата из обработчика клавиши хоста.

  std::vector<FileEntry> files;
  int index = -1;
  std::wstring name;        // of the file on screen or being read
  bool waiting = false;     // the current file is still being read by the host
  bool loading = false;     // mpv ещё распознаёт файл и готовит воспроизведение.
  bool hiddenPause = false; // paused by us because the tab went out of sight

  // PageDown ждёт первый кадр изображения. Повторы объединяются в один переход.
  bool awaitingImage = false, pageDownPending = false;
  int64_t imageEntry = 0, playingEntry = 0; // ID запрошенного изображения и файла из START_FILE.

  // observed
  double timePos = 0, duration = 0, speed = 1.0, volume = 100;
  bool paused = false, eof = false, muted = false, loaded = false;
  int64_t vpDW = 0, vpDH = 0, vpRotate = 0;
  bool isImage = false;

  // view
  double zoom = 0, panX = 0, panY = 0;
  bool autoFit = true;      // the user has not zoomed: keep the default size on resize
  bool fitWindow = false;   // this file fills the view, also by enlarging a small picture
  bool prefFit = false;     // every file starts that way: set by "fit", cleared by "100%", remembered
  double rawFit = 1.0;      // scale that fills the view, before the noUpscale cap
  bool noUpscale = false;   // a picture: mpv shows it 1:1 when it is smaller than the view
  int userRotate = 0;
  int endAction = END_STOP;

  // mouse
  bool dragging = false;
  POINT dragLast{};
  DWORD lastClickTick = 0;
  POINT lastClickPt{};

  std::string lastStatus, lastTitle;
  std::string cursorUri;    // the file paged to: where the panel cursor goes on close
  std::map<std::string, bool> marks;   // Insert presses to carry into the panel on close, by URI

  std::mutex cacheLock;                     // mpv's threads look files up (StreamOpen)
  std::map<std::string, CacheItem> cache;   // by URI
  int cacheNextId = 1;
};

std::map<int, std::unique_ptr<Player>> g_players;
int g_nextPlayerId = 1;

// The tab the running code works for. Every entry point (a host callback, the
// window procedure) names its tab with Use; nothing else sets g. Main thread
// only - mpv's threads get their Player or window through their own argument.
Player *g = nullptr;

struct Use {
  Player *prev;
  explicit Use(Player *p) : prev(g) { g = p; }
  ~Use() { g = prev; }
};

Player *FindPlayer(int id)
{
  auto it = g_players.find(id);
  return it == g_players.end() ? nullptr : it->second.get();
}

// ---------------------------------------------------------------------------
// Helpers

void Trace(const wchar_t *fmt, ...)
{
  wchar_t buf[1024];
  va_list ap; va_start(ap, fmt);
  _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);
  OutputDebugStringW(buf);
}

std::wstring Utf8ToWide(const std::string &s)
{
  if (s.empty()) return L"";
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}

std::string WideToUtf8(const std::wstring &w)
{
  if (w.empty()) return "";
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}

int HexVal(char c)
{
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// file:///C:/dir/a%20b.txt -> C:\dir\a b.txt, file://server/share/x -> \\server\share\x.
// Empty for any other scheme: mpv reads local files only here.
std::wstring UriToPath(const char *uri)
{
  if (!uri || _strnicmp(uri, "file://", 7) != 0) return L"";
  const char *p = uri + 7;
  std::string out;
  if (*p == '/') ++p; else out = "\\\\";
  for (; *p; ++p) {
    if (*p == '%' && HexVal(p[1]) >= 0 && HexVal(p[2]) >= 0) {
      out += (char)(HexVal(p[1]) * 16 + HexVal(p[2]));
      p += 2;
    } else if (*p == '/') {
      out += '\\';
    } else {
      out += *p;
    }
  }
  return Utf8ToWide(out);
}

std::string PathToUri(const std::wstring &path)
{
  std::string u = WideToUtf8(path), out = "file:///";
  size_t i = 0;
  if (u.compare(0, 2, "\\\\") == 0) { out = "file://"; i = 2; }
  for (; i < u.size(); ++i) {
    unsigned char c = (unsigned char)u[i];
    if (c == '\\') out += '/';
    else if (isalnum(c) || strchr("-._~/:!$&'()*+,;=@", c)) out += (char)c;
    else { char b[4]; _snprintf_s(b, _countof(b), _TRUNCATE, "%%%02X", c); out += b; }
  }
  return out;
}

std::string ExtOf(const std::wstring &path)
{
  size_t dot = path.find_last_of(L".\\/");
  if (dot == std::wstring::npos || path[dot] != L'.') return "";
  std::string e = WideToUtf8(path.substr(dot + 1));
  for (char &c : e) c = (char)tolower((unsigned char)c);
  return e;
}

template <size_t N> bool InList(const char *const (&list)[N], const std::string &ext)
{
  for (const char *e : list) if (ext == e) return true;
  return false;
}

bool IsMediaPath(const std::wstring &path)
{
  std::string e = ExtOf(path);
  return InList(kVideoExts, e) || InList(kImageExts, e);
}

std::wstring NameOf(const std::wstring &path)
{
  size_t s = path.find_last_of(L"\\/");
  return s == std::wstring::npos ? path : path.substr(s + 1);
}

bool SamePath(const std::wstring &a, const std::wstring &b)
{
  return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

// ---------------------------------------------------------------------------
// Settings (the host keeps them in plugin-settings-mtn.mediaview.json)

std::string GetSetting(const char *key, const char *def)
{
  char buf[256];
  if (g_host && g_host->get_setting && g_host->get_setting(kPluginId, key, buf, sizeof buf) >= 0) return buf;
  return def;
}

void SetSetting(const char *key, const std::string &val)
{
  if (g_host && g_host->set_setting) g_host->set_setting(kPluginId, key, val.c_str());
}

// ---------------------------------------------------------------------------
// Panel listing: the media files of the active panel, in panel order.
// panel_list JSON: {"uri": "...", "cursor": N, "rows": [{"uri","name","dir","size"}, ...]}

struct JsonScan {
  const char *p;
  void Ws() { while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p; }
  bool String(std::string &out)
  {
    Ws();
    if (*p != '"') return false;
    out.clear();
    for (++p; *p && *p != '"'; ++p) {
      if (*p != '\\') { out += *p; continue; }
      ++p;
      switch (*p) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'b': case 'f': break;
        case 'u': {
          unsigned cp = 0;
          for (int i = 1; i <= 4 && HexVal(p[i]) >= 0; ++i) cp = cp * 16 + HexVal(p[i]);
          p += 4;
          wchar_t w[2] = { (wchar_t)cp, 0 };
          out += WideToUtf8(w);
          break;
        }
        case 0: return false;
        default: out += *p;
      }
    }
    if (*p != '"') return false;
    ++p;
    return true;
  }
  // Skips a scalar (number, true, false, null).
  std::string Scalar()
  {
    Ws();
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\n' && *p != '\r') ++p;
    return std::string(s, p);
  }
};

// Last path segment of a URI, percent-decoded.
std::wstring UriFileName(const char *uri)
{
  const char *s = strrchr(uri, '/');
  std::string out;
  for (const char *p = s ? s + 1 : uri; *p; ++p) {
    if (*p == '%' && HexVal(p[1]) >= 0 && HexVal(p[2]) >= 0) { out += (char)(HexVal(p[1]) * 16 + HexVal(p[2])); p += 2; }
    else out += *p;
  }
  return Utf8ToWide(out);
}

// A plain file on a disk or a share; "file:///x.zip!/inner.jpg" is not one.
bool IsLocalUri(const char *uri)
{
  return _strnicmp(uri, "file://", 7) == 0 && !strstr(uri, "!/");
}

// check: a file URI that names no file on disk is not local either.
FileEntry EntryFromUri(const char *uri, bool check)
{
  if (IsLocalUri(uri)) {
    std::wstring path = UriToPath(uri);
    if (!path.empty() && (!check || GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES))
      return { uri, NameOf(path), path };
  }
  return { uri, UriFileName(uri), L"" };
}

void ListFromPanel(std::vector<FileEntry> &out)
{
  if (!g_host || !g_host->panel_list) return;
  int64_t need = g_host->panel_list(-1, nullptr, 0);
  if (need <= 0) return;
  std::string buf((size_t)need + 1, '\0');
  if (g_host->panel_list(-1, &buf[0], need + 1) < 0) return;
  const char *rows = strstr(buf.c_str(), "\"rows\"");
  if (!rows) return;
  JsonScan js{ rows + 6 };
  js.Ws(); if (*js.p != ':') return; ++js.p;
  js.Ws(); if (*js.p != '[') return; ++js.p;
  for (;;) {
    js.Ws();
    if (*js.p == ',') { ++js.p; continue; }
    if (*js.p != '{') break;
    ++js.p;
    std::string uri; bool dir = false;
    for (;;) {
      js.Ws();
      if (*js.p == ',') { ++js.p; continue; }
      if (*js.p == '}') { ++js.p; break; }
      std::string key, val;
      if (!js.String(key)) return;
      js.Ws(); if (*js.p != ':') return; ++js.p;
      js.Ws();
      if (*js.p == '"') { if (!js.String(val)) return; }
      else if (*js.p == '{' || *js.p == '[') return;   // rows are flat
      else val = js.Scalar();
      if (key == "uri") uri = val;
      else if (key == "dir") dir = (val == "true");
    }
    if (dir || uri.empty()) continue;
    FileEntry e = EntryFromUri(uri.c_str(), false);
    if (IsMediaPath(e.name)) out.push_back(e);
  }
}

// Fallback when the panel does not show the file's folder: the folder itself, by name.
void ListFromFolder(const std::wstring &file, std::vector<FileEntry> &out)
{
  size_t s = file.find_last_of(L"\\/");
  if (s == std::wstring::npos) return;
  std::wstring dir = file.substr(0, s + 1);
  WIN32_FIND_DATAW fd;
  HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
  if (h == INVALID_HANDLE_VALUE) return;
  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    std::wstring p = dir + fd.cFileName;
    if (IsMediaPath(p)) out.push_back({ PathToUri(p), fd.cFileName, p });
  } while (FindNextFileW(h, &fd));
  FindClose(h);
  std::sort(out.begin(), out.end(), [](const FileEntry &a, const FileEntry &b) {
    return StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
  });
}

// The list to page through: the rows of the active panel when the file is one
// of them, else its folder (a local file), else the file alone.
// Marks the files the active panel has selected.
// panel_info JSON: {"active": "left", "left": {"uri", "cursor", "selected": [uri, ...]}, "right": {...}}
void MarkPanelSelection(std::vector<FileEntry> &files)
{
  if (!g_host || !g_host->panel_info) return;
  int64_t need = g_host->panel_info(nullptr, 0);
  if (need <= 0) return;
  std::string buf((size_t)need + 1, '\0');
  if (g_host->panel_info(&buf[0], need + 1) < 0) return;
  const char *side = strstr(buf.c_str(), "\"active\"");
  if (!side) return;
  bool right = false;
  { JsonScan js{ side + 8 }; js.Ws(); if (*js.p == ':') ++js.p; std::string v; right = js.String(v) && v == "right"; }
  const char *panel = strstr(buf.c_str(), right ? "\"right\"" : "\"left\"");
  // "active": "left" itself is no panel object: take the key that an object follows.
  while (panel) {
    JsonScan js{ panel + (right ? 7 : 6) };
    js.Ws();
    if (*js.p == ':') { ++js.p; js.Ws(); if (*js.p == '{') break; }
    panel = strstr(panel + 1, right ? "\"right\"" : "\"left\"");
  }
  const char *sel = panel ? strstr(panel, "\"selected\"") : nullptr;
  if (!sel) return;
  JsonScan js{ sel + 10 };
  js.Ws(); if (*js.p != ':') return; ++js.p;
  js.Ws(); if (*js.p != '[') return; ++js.p;
  std::vector<std::string> uris;
  for (;;) {
    js.Ws();
    if (*js.p == ',') { ++js.p; continue; }
    std::string uri;
    if (*js.p != '"' || !js.String(uri)) break;
    uris.push_back(uri);
  }
  if (uris.empty()) return;
  std::sort(uris.begin(), uris.end());
  for (FileEntry &f : files) f.selected = std::binary_search(uris.begin(), uris.end(), f.uri);
}

void BuildFileList(const FileEntry &e)
{
  auto find = [&]() {
    for (size_t i = 0; i < g->files.size(); ++i) {
      const FileEntry &f = g->files[i];
      if (f.uri == e.uri || (f.local() && e.local() && SamePath(f.path, e.path))) { g->index = (int)i; return true; }
    }
    return false;
  };
  g->files.clear();
  ListFromPanel(g->files);
  if (find()) { MarkPanelSelection(g->files); return; }
  g->files.clear();
  if (e.local()) ListFromFolder(e.path, g->files);
  if (find()) return;
  g->files.clear();
  g->files.push_back(e);
  g->index = 0;
}

// ---------------------------------------------------------------------------
// Files mpv cannot open by name (inside an archive, sftp, a scheme of another
// plugin). The host opens such a file (vfs_open, permission "vfs.read") and we
// read it in pieces (vfs_read_at) for mpv, which sees a stream protocol of
// ours: "mtnvfs://<id>.<ext>". No size limit and nothing is unpacked by the
// plugin - how the host gets at the bytes (it copies the file to a temporary
// one of its own, showing progress) is its business.
//
// The open files form a cache around the current one: the next g_cacheNext
// and the previous g_cachePrev remote files are opened ahead / kept open, see
// UpdateCache. The host allows a plugin 16 open files in all.

constexpr const char *kStreamProtocol = "mtnvfs";

bool g_permissionTold = false;

// mediaview.conf: cache-next, cache-prev, cache-max-mb
int g_cacheNext = 2, g_cachePrev = 2;
int64_t g_cacheMaxBytes = 256ll * 1024 * 1024;

RemoteFile::~RemoteFile()
{
  if (g_host && g_host->vfs_close) g_host->vfs_close(handle);
}

// mpv's threads from here to StreamOpen: vfs_read_at blocks only the caller.
struct StreamCursor { Block file; int64_t pos = 0; };

int64_t StreamRead(void *cookie, char *buf, uint64_t n)
{
  StreamCursor *c = static_cast<StreamCursor *>(cookie);
  int64_t got = g_host->vfs_read_at(c->file->handle, c->pos, reinterpret_cast<uint8_t *>(buf),
                                    (int64_t)std::min<uint64_t>(n, 16u * 1024 * 1024));
  if (got < 0) return -1;
  c->pos += got;
  return got;
}

int64_t StreamSeek(void *cookie, int64_t offset)
{
  StreamCursor *c = static_cast<StreamCursor *>(cookie);
  if (offset < 0 || offset > c->file->size) return MPV_ERROR_GENERIC;
  c->pos = offset;
  return offset;
}

int64_t StreamSize(void *cookie) { return static_cast<StreamCursor *>(cookie)->file->size; }
void StreamClose(void *cookie) { delete static_cast<StreamCursor *>(cookie); }

// The stream keeps its own reference: dropping a file from the cache never
// cuts one that is still playing.
int StreamOpen(void *user, char *uri, mpv_stream_cb_info *info)
{
  Player *player = static_cast<Player *>(user);   // mpv's thread: not g
  const char *idText = uri ? strstr(uri, "://") : nullptr;
  int id = idText ? atoi(idText + 3) : 0;
  Block file;
  {
    std::lock_guard<std::mutex> lock(player->cacheLock);
    for (auto &it : player->cache) if (it.second.id == id) { file = it.second.data; break; }
  }
  if (!file) return MPV_ERROR_LOADING_FAILED;
  info->cookie = new StreamCursor{ file, 0 };
  info->read_fn = StreamRead;
  info->seek_fn = StreamSeek;
  info->size_fn = StreamSize;
  info->close_fn = StreamClose;
  return 0;
}

// ---------------------------------------------------------------------------
// mpv plumbing

// Выполняет команду mpv и возвращает код результата.
int Command(const char *a, const char *b = nullptr, const char *c = nullptr, const char *d = nullptr,
            const char *e = nullptr)
{
  if (!g->mpv) return MPV_ERROR_GENERIC;
  const char *args[6] = { a, b, c, d, e, nullptr };
  return g_mpv.command(g->mpv, args);
}
void SetStr(const char *n, const char *v) { if (g->mpv) g_mpv.set_property_string(g->mpv, n, v); }
void SetDouble(const char *n, double v) { if (g->mpv) g_mpv.set_property(g->mpv, n, MPV_FORMAT_DOUBLE, &v); }
void SetInt(const char *n, int64_t v) { if (g->mpv) g_mpv.set_property(g->mpv, n, MPV_FORMAT_INT64, &v); }
void SetFlag(const char *n, bool v) { int f = v ? 1 : 0; if (g->mpv) g_mpv.set_property(g->mpv, n, MPV_FORMAT_FLAG, &f); }
bool PropFlag(const char *n) { int f = 0; return g->mpv && g_mpv.get_property(g->mpv, n, MPV_FORMAT_FLAG, &f) >= 0 && f; }
int64_t PropInt(const char *n, int64_t def = 0)
{
  int64_t v = def;
  if (g->mpv && g_mpv.get_property(g->mpv, n, MPV_FORMAT_INT64, &v) < 0) v = def;
  return v;
}

void Osd(const char *fmt, ...)
{
  char buf[256];
  va_list ap; va_start(ap, fmt);
  _vsnprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
  va_end(ap);
  Command("show-text", buf, "1200");
}

// mpv's thread: the view window is the context.
void WakeupCb(void *ctx)
{
  PostMessageW(static_cast<HWND>(ctx), WM_MPV_WAKEUP, 0, 0);
}

// Передаёт фокус главному окну MTN2, чтобы клавиши шли через обработчик поверхности.
void ReturnFocus(HWND from)
{
  HWND root = GetAncestor(from, GA_ROOT);
  if (!root) return;
  DWORD tid = GetWindowThreadProcessId(root, nullptr), me = GetCurrentThreadId();
  bool attached = (tid != me) && AttachThreadInput(me, tid, TRUE);
  SetFocus(root);
  if (attached) AttachThreadInput(me, tid, FALSE);
}

// В потоке вывода mpv пересылает события мыши контейнеру через его очередь.
// Дочернее окно занимает весь контейнер, поэтому координаты совпадают.
LRESULT CALLBACK MpvChildProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  WNDPROC orig = reinterpret_cast<WNDPROC>(GetPropW(hWnd, kChildProp));
  HWND parent = GetParent(hWnd);
  switch (msg) {
    case WM_MOUSEACTIVATE: return MA_ACTIVATE; // Активируем MTN2 и сохраняем первый клик.
    case WM_SETFOCUS:      ReturnFocus(hWnd); return 0;
    case WM_LBUTTONDOWN:   SetCapture(hWnd); break;
    case WM_LBUTTONUP:     ReleaseCapture(); break;
    case WM_NCDESTROY:     RemovePropW(hWnd, kChildProp); break;
  }
  if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST && parent) PostMessageW(parent, msg, wParam, lParam);
  return orig ? CallWindowProcW(orig, hWnd, msg, wParam, lParam) : DefWindowProcW(hWnd, msg, wParam, lParam);
}

void SubclassMpvChild()
{
  if (g->mpvChild || !g->view) return;
  HWND child = GetWindow(g->view, GW_CHILD);
  if (!child) return;
  LONG_PTR orig = GetWindowLongPtrW(child, GWLP_WNDPROC);
  SetPropW(child, kChildProp, reinterpret_cast<HANDLE>(orig));
  SetWindowLongPtrW(child, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MpvChildProc));
  g->mpvChild = child;
}

void SizeChildToView()
{
  HWND child = g->view ? GetWindow(g->view, GW_CHILD) : nullptr;
  if (!child) return;
  RECT rc; GetClientRect(g->view, &rc);
  SetWindowPos(child, nullptr, 0, 0, rc.right, rc.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
}

bool CreateMpv()
{
  g->mpv = g_mpv.create();
  if (!g->mpv) return false;

  char wid[32];
  _snprintf_s(wid, _countof(wid), _TRUNCATE, "%lld", (long long)(intptr_t)g->view);
  std::string logLevel = "error";

  struct { const char *k, *v; } opts[] = {
    { "wid",                    wid },
    { "config",                 "no" },
    { "terminal",               "no" },
    { "osc",                    "no" },
    { "osd-level",              "1" },
    { "osd-bar",                "no" },
    { "osd-font-size",          "32" },
    { "input-default-bindings", "no" },
    { "input-vo-keyboard",      "no" },
    { "input-cursor",           "no" },
    { "input-media-keys",       "no" },
    { "window-dragging",        "no" },
    { "cursor-autohide",        "no" },
    { "ytdl",                   "no" },
    { "audio-display",          "no" },
    { "keep-open",              "yes" },   // stay on the last frame; the end action decides what follows
    { "idle",                   "yes" },
    { "image-display-duration", "inf" },
    // The view of one file must not leak into the first frame of the next.
    { "reset-on-next-file",     "video-zoom,video-pan-x,video-pan-y,video-rotate" },
    // d3d11va is the vendor-neutral DXVA path and pairs zero-copy with the
    // default d3d11 context; "auto-safe" was seen to pick software decoding.
    // video-sync stays at mpv's default: the display-* modes misjudge the
    // refresh rate of an embedded window and slow the video down.
    { "hwdec",                  "d3d11va" },
    { "audio-pitch-correction", "yes" },
    { "border",                 "no" },
    { "stop-screensaver",       "no" },
  };
  for (auto &o : opts) g_mpv.set_option_string(g->mpv, o.k, o.v);

  // User overrides: <plugin dir>\mediaview.conf, "option=value" per line (mpv
  // option names, '#' comments). "log-level" is ours: mpv's log to DebugView.
  std::wstring conf = std::wstring(g_pluginDir) + L"\\mediaview.conf";
  if (FILE *f = _wfopen(conf.c_str(), L"rb")) {
    char line[512];
    while (fgets(line, sizeof line, f)) {
      char *s = line;
      while (*s == ' ' || *s == '\t') ++s;
      if (*s == '#' || *s == ';' || *s == '\r' || *s == '\n' || !*s) continue;
      char *eq = strchr(s, '=');
      if (!eq) continue;
      *eq = 0;
      char *val = eq + 1;
      for (char *e = s + strlen(s); e > s && (e[-1] == ' ' || e[-1] == '\t'); --e) e[-1] = 0;
      for (char *e = val + strlen(val); e > val && strchr("\r\n \t", e[-1]); --e) e[-1] = 0;
      if (strcmp(s, "log-level") == 0) { logLevel = val; continue; }
      if (strcmp(s, "cache-next") == 0) { g_cacheNext = std::max(0, std::min(atoi(val), 32)); continue; }
      if (strcmp(s, "cache-prev") == 0) { g_cachePrev = std::max(0, std::min(atoi(val), 32)); continue; }
      if (strcmp(s, "cache-max-mb") == 0) { g_cacheMaxBytes = std::max(0, atoi(val)) * 1024ll * 1024; continue; }
      int rc = g_mpv.set_option_string(g->mpv, s, val);
      Trace(L"mediaview: conf %S=%S -> %S", s, val, rc >= 0 ? "ok" : g_mpv.error_string(rc));
    }
    fclose(f);
  }

  g_mpv.request_log_messages(g->mpv, logLevel.c_str());
  g_mpv.set_wakeup_callback(g->mpv, WakeupCb, g->view);
  g_mpv.stream_cb_add_ro(g->mpv, kStreamProtocol, g, StreamOpen);
  if (g_mpv.initialize(g->mpv) < 0) {
    g_mpv.terminate_destroy(g->mpv);
    g->mpv = nullptr;
    return false;
  }
  g_mpv.observe_property(g->mpv, OBS_TIME_POS,  "time-pos",            MPV_FORMAT_DOUBLE);
  g_mpv.observe_property(g->mpv, OBS_DURATION,  "duration",            MPV_FORMAT_DOUBLE);
  g_mpv.observe_property(g->mpv, OBS_PAUSE,     "pause",               MPV_FORMAT_FLAG);
  g_mpv.observe_property(g->mpv, OBS_EOF,       "eof-reached",         MPV_FORMAT_FLAG);
  g_mpv.observe_property(g->mpv, OBS_SPEED,     "speed",               MPV_FORMAT_DOUBLE);
  g_mpv.observe_property(g->mpv, OBS_VP_DW,     "video-params/dw",     MPV_FORMAT_INT64);
  g_mpv.observe_property(g->mpv, OBS_VP_DH,     "video-params/dh",     MPV_FORMAT_INT64);
  g_mpv.observe_property(g->mpv, OBS_VP_ROTATE, "video-params/rotate", MPV_FORMAT_INT64);
  g_mpv.observe_property(g->mpv, OBS_VOLUME,    "volume",              MPV_FORMAT_DOUBLE);
  g_mpv.observe_property(g->mpv, OBS_MUTE,      "mute",                MPV_FORMAT_FLAG);

  SetDouble("volume", atof(GetSetting("volume", "100").c_str()));
  SetFlag("mute", GetSetting("mute", "0") == "1");
  return true;
}

// ---------------------------------------------------------------------------
// Geometry

// Size of the picture as mpv fits it into the view at zoom 0, and that fit scale.
bool FitSize(double &w, double &h, double &fit, double &viewW, double &viewH)
{
  if (!g->view) return false;
  RECT rc; GetClientRect(g->view, &rc);
  viewW = rc.right; viewH = rc.bottom;
  // video-params/rotate already includes our video-rotate.
  double vw = (double)g->vpDW, vh = (double)g->vpDH;
  if ((g->vpRotate % 180) != 0) std::swap(vw, vh);
  if (viewW <= 0 || viewH <= 0 || vw <= 0 || vh <= 0) return false;
  fit = std::min(viewW / vw, viewH / vh);
  g->rawFit = fit;
  if (g->noUpscale && fit > 1.0) fit = 1.0;   // video-unscaled=downscale-big
  w = vw * fit; h = vh * fit;
  return true;
}

void ApplyView()
{
  double w, h, fit, vw, vh;
  // Nothing to lay out until the new file reports its size: the picture still
  // on screen keeps its place, and mpv starts the next one at zoom 0 itself.
  if (!FitSize(w, h, fit, vw, vh)) return;
  if (g->fitWindow) g->zoom = std::log2(g->rawFit / fit);
  else if (g->autoFit) g->zoom = 0.0;
  g->zoom = std::max(-6.0, std::min(g->zoom, 8.0));
  double s = std::pow(2.0, g->zoom), sw = w * s, sh = h * s;
  // Keep the picture over the view: no pan while it fits, edge-to-edge otherwise.
  double limX = sw > vw ? (sw - vw) / (2 * sw) : 0, limY = sh > vh ? (sh - vh) / (2 * sh) : 0;
  g->panX = std::max(-limX, std::min(g->panX, limX));
  g->panY = std::max(-limY, std::min(g->panY, limY));
  SetDouble("video-zoom", g->zoom);
  SetDouble("video-pan-x", g->panX);
  SetDouble("video-pan-y", g->panY);
}

// Zooms to newZoom keeping the picture point under (cx, cy) (view pixels) in place.
void ZoomAt(double newZoom, double cx, double cy)
{
  double w, h, fit, vw, vh;
  if (!FitSize(w, h, fit, vw, vh)) return;
  newZoom = std::max(-6.0, std::min(newZoom, 8.0));
  double s0 = std::pow(2.0, g->zoom), s1 = std::pow(2.0, newZoom);
  double dx = cx - vw / 2, dy = cy - vh / 2;
  g->panX += dx / (w * s1) - dx / (w * s0);
  g->panY += dy / (h * s1) - dy / (h * s0);
  g->zoom = newZoom;
  g->autoFit = g->fitWindow = false;
  ApplyView();
  Osd("Zoom %.0f%%", fit * std::pow(2.0, g->zoom) * 100.0);
}

void ZoomStep(double delta, const POINT *at = nullptr)
{
  RECT rc{}; if (g->view) GetClientRect(g->view, &rc);
  ZoomAt(g->zoom + delta, at ? at->x : rc.right / 2.0, at ? at->y : rc.bottom / 2.0);
}

void ZoomFit()
{
  g->fitWindow = true; g->panX = g->panY = 0;
  if (!g->prefFit) { g->prefFit = true; SetSetting("fit", "1"); }
  ApplyView();
  Osd("Fit");
}

void ZoomActual()
{
  double w, h, fit, vw, vh;
  if (!FitSize(w, h, fit, vw, vh)) return;
  if (g->prefFit) { g->prefFit = false; SetSetting("fit", "0"); }
  ZoomAt(std::log2(1.0 / fit), vw / 2, vh / 2);
}

void PanBy(double dxPixels, double dyPixels)
{
  double w, h, fit, vw, vh;
  if (!FitSize(w, h, fit, vw, vh)) return;
  double s = std::pow(2.0, g->zoom);
  g->panX += dxPixels / (w * s);
  g->panY += dyPixels / (h * s);
  ApplyView();
}

bool Zoomed()
{
  double w, h, fit, vw, vh;
  if (!FitSize(w, h, fit, vw, vh)) return false;
  double s = std::pow(2.0, g->zoom);
  return w * s > vw + 0.5 || h * s > vh + 0.5;
}

bool IsFullscreen()
{
  if (!g->view) return false;
  if (g_host->surface_get_fullscreen && g->surface) return g_host->surface_get_fullscreen(g->surface) == 1;
  HWND root = GetAncestor(g->view, GA_ROOT);
  RECT a, b;
  if (!root || !GetWindowRect(g->view, &a) || !GetClientRect(root, &b)) return false;
  return a.right - a.left >= b.right && a.bottom - a.top >= b.bottom;
}

// ---------------------------------------------------------------------------
// Playback

std::string FmtTime(double sec)
{
  if (sec < 0) sec = 0;
  int t = (int)sec, h = t / 3600, m = t / 60 % 60, s = t % 60;
  char b[32];
  if (h) _snprintf_s(b, _countof(b), _TRUNCATE, "%d:%02d:%02d", h, m, s);
  else   _snprintf_s(b, _countof(b), _TRUNCATE, "%d:%02d", m, s);
  return b;
}

void UpdateInfo()
{
  if (!g->surface || !g_host) return;
  std::string title = WideToUtf8(g->name);
  char b[128];
  std::string st;
  if (!g->files.empty()) {
    int marked = 0;
    for (const FileEntry &f : g->files) if (f.selected) ++marked;
    bool cur = g->index >= 0 && g->index < (int)g->files.size() && g->files[g->index].selected;
    _snprintf_s(b, _countof(b), _TRUNCATE, "%s[%d/%d]  ", cur ? "* " : "", g->index + 1, (int)g->files.size()); st += b;
    if (marked) { _snprintf_s(b, _countof(b), _TRUNCATE, "Selected %d  ", marked); st += b; }
  }
  if (g->waiting || g->loading) st += "Loading...  ";
  else if (g->vpDW > 0) {
    bool swap = ((g->vpRotate - g->userRotate) % 180) != 0;
    _snprintf_s(b, _countof(b), _TRUNCATE, "%lldx%lld  ", (long long)(swap ? g->vpDH : g->vpDW), (long long)(swap ? g->vpDW : g->vpDH));
    st += b;
  }
  double w, h, fit, vw, vh;
  if (!g->waiting && FitSize(w, h, fit, vw, vh)) { _snprintf_s(b, _countof(b), _TRUNCATE, "%.0f%%  ", fit * std::pow(2.0, g->zoom) * 100.0); st += b; }
  if (!g->waiting && g->loaded && !g->isImage) {
    st += FmtTime(g->eof ? g->duration : g->timePos) + " / " + FmtTime(g->duration);
    st += g->eof ? "  End" : g->paused ? "  Paused" : "  Playing";
    if (std::fabs(g->speed - 1.0) > 0.001) { _snprintf_s(b, _countof(b), _TRUNCATE, "  %.4gx", g->speed); st += b; }
    _snprintf_s(b, _countof(b), _TRUNCATE, g->muted ? "  Vol mute" : "  Vol %d", (int)std::lround(g->volume)); st += b;
    st += std::string("  At end: ") + kEndNames[g->endAction];
  }
  if (st == g->lastStatus && title == g->lastTitle) return;
  g->lastStatus = st; g->lastTitle = title;
  g_host->surface_set_info(g->surface, title.c_str(), st.c_str());
}

// Starts the current file. A local one is opened by name; any other must be
// open in the cache already (Show sees to that).
void LoadCurrent()
{
  if (g->index < 0 || g->index >= (int)g->files.size()) return;
  const FileEntry &e = g->files[g->index];
  std::string target;
  if (e.local()) {
    target = WideToUtf8(e.path);
  } else {
    std::lock_guard<std::mutex> lock(g->cacheLock);
    auto it = g->cache.find(e.uri);
    if (it == g->cache.end() || !it->second.data) return;
    target = std::string(kStreamProtocol) + "://" + std::to_string(it->second.id) + "." + ExtOf(e.name);
  }
  g->waiting = false;
  g->loading = true;
  g->loaded = false; g->eof = false; g->isImage = false;
  g->vpDW = g->vpDH = g->vpRotate = 0;
  g->timePos = g->duration = 0;
  g->zoom = g->panX = g->panY = 0; g->autoFit = true; g->fitWindow = g->prefFit; g->userRotate = 0;
  // Options of this file only, so that neither the picture still on screen nor
  // the first frame of the new one is ever drawn with the other's settings:
  // a picture smaller than the view stays 1:1 unless "fit" is on (a video is
  // always scaled up to fit), an animated picture loops.
  std::string ext = ExtOf(e.name);
  g->noUpscale = InList(kImageExts, ext) && !g->prefFit;
  std::string opts = std::string("video-unscaled=") + (g->noUpscale ? "downscale-big" : "no") +
                     ",loop-file=" + (InList(kLoopExts, ext) ? "inf" : "no");
  SetFlag("pause", false);
  if (Command("loadfile", target.c_str(), "replace", "-1", opts.c_str()) < 0) {
    g->loading = g->awaitingImage = false;
  } else if (g->awaitingImage) {
    // replace создаёт единственную запись. События предыдущего файла ещё могут быть в очереди.
    g->imageEntry = PropInt("playlist/0/id");
    g->awaitingImage = g->imageEntry != 0;
  }
  UpdateInfo();
}

void OnVfsOpen(void *user, int64_t status, int64_t handle);
void ContinuePageDown();

struct VfsRequest { std::string uri; int player; };

// Asks the host to open a remote file unless it is open or being opened.
// False when the host refuses: no permission, 16 files open already.
bool Fetch(const std::string &uri)
{
  if (!g_host->vfs_open) return false;
  {
    std::lock_guard<std::mutex> lock(g->cacheLock);
    if (g->cache.count(uri)) return true;
    g->cache[uri].id = g->cacheNextId++;
  }
  VfsRequest *req = new VfsRequest{ uri, g->id };
  int64_t job = g_host->vfs_open(kPluginId, uri.c_str(), OnVfsOpen, req);
  std::lock_guard<std::mutex> lock(g->cacheLock);
  if (job > 0) {   // the callback follows, never from inside the call
    g->cache[uri].job = job;
    return true;
  }
  delete req;
  g->cache.erase(uri);
  if (job == -2 && !g_permissionTold && g_host->show_message) {
    g_permissionTold = true;
    g_host->show_message("Media viewer: to open files inside archives allow \"vfs.read\" in Options - Plugins - Permissions.", 1);
  }
  return false;
}

// Keeps the cache around the current file: the remote files among the next
// g_cacheNext and the previous g_cachePrev are opened ahead, the rest is let
// go (an opening still under way is cancelled). The nearest come first and
// forward before backward; once the nearer files fill g_cacheMaxBytes, the
// farther ones are neither kept nor opened.
void UpdateCache()
{
  std::vector<std::string> want;
  auto add = [&](int i) {
    if (i >= 0 && i < (int)g->files.size() && !g->files[i].local()) want.push_back(g->files[i].uri);
  };
  add(g->index);
  for (int d = 1; d <= std::max(g_cacheNext, g_cachePrev); ++d) {
    if (d <= g_cacheNext) add(g->index + d);
    if (d <= g_cachePrev) add(g->index - d);
  }
  std::vector<std::string> fetch;
  std::vector<int64_t> cancel;
  {
    std::lock_guard<std::mutex> lock(g->cacheLock);
    for (auto it = g->cache.begin(); it != g->cache.end();) {
      if (std::find(want.begin(), want.end(), it->first) != want.end()) { ++it; continue; }
      if (!it->second.data && it->second.job > 0) cancel.push_back(it->second.job);
      it = g->cache.erase(it);
    }
    int64_t total = 0;
    for (size_t i = 0; i < want.size(); ++i) {
      auto it = g->cache.find(want[i]);
      if (i > 0 && total >= g_cacheMaxBytes) {
        if (it != g->cache.end() && it->second.data) g->cache.erase(it);
        continue;
      }
      if (it == g->cache.end()) fetch.push_back(want[i]);
      else if (it->second.data) total += it->second.data->size;
    }
  }
  // Their callbacks still come, with "cancelled", and find nothing to fill.
  if (g_host->vfs_cancel) for (int64_t job : cancel) g_host->vfs_cancel(job);
  for (const std::string &uri : fetch) if (!Fetch(uri)) break;
}

void OnVfsOpen(void *user, int64_t status, int64_t handle)
{
  VfsRequest *req = static_cast<VfsRequest *>(user);
  std::string uri = req->uri;
  Player *player = FindPlayer(req->player);
  delete req;
  if (!g_host) return;
  // Ours from here on: closed with the last reference.
  Block file;
  if (status == 0) file = std::make_shared<RemoteFile>(handle, g_host->vfs_size(handle));
  if (!player) return;   // the tab is closed
  Use use(player);
  bool current = g->index >= 0 && g->index < (int)g->files.size() && g->files[g->index].uri == uri;
  if (!file) {
    { std::lock_guard<std::mutex> lock(g->cacheLock); g->cache.erase(uri); }
    if (current && g->waiting) {
      g->waiting = false;
      g->awaitingImage = false;
      if (status != 4 && g_host->show_message)   // 4 = cancelled, the user knows
        g_host->show_message(("Media viewer: " + WideToUtf8(g->name) + ": cannot read the file (status " +
                              std::to_string(status) + ")").c_str(), 1);
      UpdateInfo();
      ContinuePageDown();
    }
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g->cacheLock);
    auto it = g->cache.find(uri);
    if (it == g->cache.end()) return;   // paged away meanwhile
    it->second.data = file;
    it->second.job = 0;
  }
  if (current && g->waiting) LoadCurrent();
  UpdateCache();
  ContinuePageDown();
}

// Makes the file at index the current one: plays it at once when mpv can open
// it (a local file, one open in the cache), else when the host has opened it.
void Show(int index)
{
  g->index = index;
  const FileEntry &e = g->files[index];
  g->name = e.name;
  g->waiting = false;
  g->pageDownPending = false;   // Явный переход к другому файлу отменяет отложенный PageDown.
  g->imageEntry = 0;
  g->awaitingImage = InList(kImageExts, ExtOf(e.name));
  bool ready = e.local();
  if (!ready) {
    std::lock_guard<std::mutex> lock(g->cacheLock);
    auto it = g->cache.find(e.uri);
    ready = it != g->cache.end() && it->second.data;
  }
  if (ready) LoadCurrent();
  else if (Fetch(e.uri)) { g->waiting = true; UpdateInfo(); }
  else g->awaitingImage = false;
  UpdateCache();
}

// Shows the file at index; the panel cursor follows when the tab closes.
bool GoTo(int index)
{
  if (index < 0 || index >= (int)g->files.size() || index == g->index) return false;
  Show(index);
  // The panel cannot be moved while our tab covers it (panel_set_cursor only
  // works with the panels on screen): the cursor is put there on close.
  g->cursorUri = g->files[index].uri;
  return true;
}

// Выполняет один отложенный переход после показа кадра или ошибки открытия.
void ContinuePageDown()
{
  if (!g->pageDownPending || g->awaitingImage || g->closing) return;
  g->pageDownPending = false;
  GoTo(g->index + 1);
}

void Play()
{
  if (g->eof) Command("seek", "0", "absolute");
  SetFlag("pause", false);
}

void TogglePause()
{
  if (g->eof || g->paused) Play(); else SetFlag("pause", true);
}

void SeekBy(double sec)
{
  char b[32];
  _snprintf_s(b, _countof(b), _TRUNCATE, "%.3f", sec);
  Command("seek", b, "relative+exact");   // a seek never touches the pause flag
}

void ChangeRate(int step)
{
  int cur = (int)std::lround(g->speed * 1000), n = (int)_countof(kRates), i = 0;
  if (step == 0) cur = 1000;
  else {
    while (i < n - 1 && kRates[i] < cur) ++i;
    if (step > 0 && kRates[i] == cur) ++i;
    else if (step < 0) --i;
    cur = kRates[std::max(0, std::min(i, n - 1))];
  }
  SetDouble("speed", cur / 1000.0);
  Osd("Speed %.4gx", cur / 1000.0);
}

void ChangeVolume(int delta)
{
  double v = std::max(0.0, std::min(g->volume + delta, 100.0));
  g->volume = v;
  SetDouble("volume", v);
  SetSetting("volume", std::to_string((int)std::lround(v)));
  Osd("Volume %d", (int)std::lround(v));
}

void Rotate(int delta)
{
  g->userRotate = ((g->userRotate + delta) % 360 + 360) % 360;
  g->panX = g->panY = 0;
  SetInt("video-rotate", g->userRotate);
  ApplyView();
}

// Full screen is a mode the user switches and that is remembered for the next
// viewer; only this function changes it.
void ToggleFullscreen()
{
  if (!g->surface || !g_host->surface_set_fullscreen) return;
  bool on = !IsFullscreen();
  g_host->surface_set_fullscreen(g->surface, on ? 1 : 0);
  SetSetting("fullscreen", on ? "1" : "0");
}

void OnEof()
{
  if (g->isImage) return;
  switch (g->endAction) {
    case END_REPEAT: Play(); break;
    case END_NEXT:   GoTo(g->index + 1); break;
    default: break;
  }
}

void OnFileLoaded()
{
  g->loaded = true;
  g->isImage = PropFlag("current-tracks/video/image");
  g->vpDW = PropInt("video-params/dw");
  g->vpDH = PropInt("video-params/dh");
  g->vpRotate = PropInt("video-params/rotate");
  ApplyView();
}

void DrainEvents()
{
  if (!g->mpv) return;
  // mpv creates its window on its VO thread at an unpredictable moment.
  if (!g->mpvChild) SubclassMpvChild();
  for (;;) {
    mpv_event *ev = g_mpv.wait_event(g->mpv, 0);
    if (!ev || ev->event_id == MPV_EVENT_NONE) break;
    switch (ev->event_id) {
      case MPV_EVENT_START_FILE: {
        auto *e = static_cast<mpv_event_start_file *>(ev->data);
        g->playingEntry = e ? e->playlist_entry_id : 0;
        break;
      }
      case MPV_EVENT_PROPERTY_CHANGE: {
        auto *p = static_cast<mpv_event_property *>(ev->data);
        if (!p->data) break;
        switch (ev->reply_userdata) {
          case OBS_TIME_POS:  g->timePos = *static_cast<double *>(p->data); break;
          case OBS_DURATION:  g->duration = *static_cast<double *>(p->data); break;
          case OBS_PAUSE:     g->paused = *static_cast<int *>(p->data) != 0; break;
          case OBS_SPEED:     g->speed = *static_cast<double *>(p->data); break;
          case OBS_VOLUME:    g->volume = *static_cast<double *>(p->data); break;
          case OBS_MUTE:      g->muted = *static_cast<int *>(p->data) != 0; break;
          case OBS_VP_DW:     g->vpDW = *static_cast<int64_t *>(p->data); ApplyView(); break;
          case OBS_VP_DH:     g->vpDH = *static_cast<int64_t *>(p->data); ApplyView(); break;
          case OBS_VP_ROTATE: g->vpRotate = *static_cast<int64_t *>(p->data); ApplyView(); break;
          case OBS_EOF: {
            bool eof = *static_cast<int *>(p->data) != 0, was = g->eof;
            g->eof = eof;
            if (eof && !was && g->loaded) OnEof();
            break;
          }
        }
        break;
      }
      case MPV_EVENT_FILE_LOADED:
        g->loading = false;
        OnFileLoaded();
        break;
      case MPV_EVENT_PLAYBACK_RESTART:
        // При стандартных настройках mpv к этому событию первый кадр уже выведен.
        if (g->imageEntry && g->playingEntry == g->imageEntry) g->awaitingImage = false;
        break;
      case MPV_EVENT_VIDEO_RECONFIG:
        SubclassMpvChild();
        SizeChildToView();
        ApplyView();
        break;
      case MPV_EVENT_END_FILE: {
        g->loading = false;
        auto *e = static_cast<mpv_event_end_file *>(ev->data);
        if (e && e->playlist_entry_id == g->imageEntry) g->awaitingImage = false;
        if (e && e->reason == MPV_END_FILE_REASON_ERROR) {
          std::string m = "Cannot play " + WideToUtf8(g->name) + ": " + g_mpv.error_string(e->error);
          if (g_host && g_host->show_message) g_host->show_message(m.c_str(), 1);
        }
        break;
      }
      case MPV_EVENT_LOG_MESSAGE: {
        auto *m = static_cast<mpv_event_log_message *>(ev->data);
        if (m) Trace(L"mediaview[%S]: %S", m->prefix, m->text);
        break;
      }
      default:
        break;
    }
  }
  ContinuePageDown();
  UpdateInfo();
}

// Освобождает mpv до удаления родительского окна; повторный вход безопасен.
void DestroyMpv()
{
  if (!g->mpv) return;
  SetSetting("mute", g->muted ? "1" : "0");
  mpv_handle *mpv = g->mpv;
  // Ожидание рабочих потоков может доставить оконные сообщения повторно.
  g->mpv = nullptr;
  g_mpv.set_wakeup_callback(mpv, nullptr, nullptr);
  g_mpv.terminate_destroy(mpv);   // Дожидаемся потоков и закрытия файла до удаления Player.
  g->mpvChild = nullptr;
}

// ---------------------------------------------------------------------------
// View window (main thread)

void OnMouse(UINT msg, WPARAM wParam, LPARAM lParam)
{
  POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
  switch (msg) {
    case WM_LBUTTONDOWN: {
      DWORD now = GetTickCount();
      if (now - g->lastClickTick <= GetDoubleClickTime() &&
          std::abs(pt.x - g->lastClickPt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) &&
          std::abs(pt.y - g->lastClickPt.y) <= GetSystemMetrics(SM_CYDOUBLECLK)) {
        g->lastClickTick = 0;
        g->dragging = false;
        ToggleFullscreen();
        break;
      }
      g->lastClickTick = now; g->lastClickPt = pt;
      g->dragging = true; g->dragLast = pt;
      break;
    }
    case WM_LBUTTONDBLCLK:
      g->dragging = false;
      ToggleFullscreen();
      break;
    case WM_MOUSEMOVE:
      if (g->dragging && (wParam & MK_LBUTTON)) {
        PanBy(pt.x - g->dragLast.x, pt.y - g->dragLast.y);
        g->dragLast = pt;
      } else {
        g->dragging = false;
      }
      break;
    case WM_LBUTTONUP:
      g->dragging = false;
      break;
    case WM_MBUTTONDOWN:
      if (g->fitWindow) ZoomActual(); else ZoomFit();
      break;
    case WM_MOUSEWHEEL: {
      ScreenToClient(g->view, &pt);   // wheel coordinates are screen ones
      ZoomStep(GET_WHEEL_DELTA_WPARAM(wParam) / (double)WHEEL_DELTA * 0.25, &pt);
      break;
    }
  }
}

void CloseTabFromPlugin();

// Обрабатывает сообщения окна просмотра в основном потоке MTN2.
LRESULT CALLBACK ViewProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
  if (msg == WM_NCCREATE)
    SetWindowLongPtrW(hWnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams));
  Player *player = reinterpret_cast<Player *>(GetWindowLongPtrW(hWnd, GWLP_USERDATA));
  if (!player) return DefWindowProcW(hWnd, msg, wParam, lParam);
  Use use(player);
  // После Esc не обрабатываем события mpv и таймеры перед удалением окна.
  if (g->closing && msg != WM_CLOSE_TAB && msg != WM_DESTROY && msg != WM_NCDESTROY)
    return DefWindowProcW(hWnd, msg, wParam, lParam);
  switch (msg) {
    case WM_CLOSE_TAB:
      CloseTabFromPlugin();   // Удаляет окно и Player; дальше к ним не обращаемся.
      return 0;
    case WM_MPV_WAKEUP:
      DrainEvents();
      return 0;
    case WM_TIMER: {
      // The host hides its container when another tab (or a dialog) covers
      // ours, and stops the surface timer: a video must not play on unseen.
      bool visible = IsWindowVisible(hWnd) != FALSE;
      if (!visible && g->loaded && !g->isImage && !g->paused && !g->eof) {
        g->hiddenPause = true;
        SetFlag("pause", true);
      } else if (visible && g->hiddenPause) {
        g->hiddenPause = false;
        SetFlag("pause", false);
      }
      return 0;
    }
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE; // Активируем MTN2 и до появления окна вывода mpv.
    case WM_SETFOCUS:
      ReturnFocus(hWnd);
      return 0;
    case WM_SIZE:
      SubclassMpvChild();
      SizeChildToView();
      ApplyView();
      return 0;
    case WM_ERASEBKGND: {
      RECT rc; GetClientRect(hWnd, &rc);
      FillRect(reinterpret_cast<HDC>(wParam), &rc, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
      return 1;
    }
    case WM_DESTROY:
      // Останавливаем периодические вызовы до ожидания рабочих потоков mpv.
      g->closing = true;
      KillTimer(hWnd, 1);
      // The host destroys its container (and us with it) when the tab closes,
      // before the closed callback: mpv must go while its parent still exists.
      DestroyMpv();
      g->view = nullptr;
      SetWindowLongPtrW(hWnd, GWLP_USERDATA, 0);   // the Player may be gone before the last messages
      return 0;
  }
  if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) {
    OnMouse(msg, wParam, lParam);
    return 0;
  }
  return DefWindowProcW(hWnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Host callbacks

// Letters arrive as typed; map the Russian layout onto the same physical keys.
std::string NormalizeKey(const char *key)
{
  static const struct { const char *from, *to; } map[] = {
    { "х", "[" }, { "ъ", "]" }, { "Х", "[" }, { "Ъ", "]" }, { "к", "r" }, { "К", "R" }, { "а", "f" }, { "А", "f" },
    { "ь", "m" }, { "Ь", "m" }, { "ф", "a" }, { "Ф", "a" }, { "у", "e" }, { "У", "e" }, { "б", "<" }, { "ю", ">" },
    { "Б", "<" }, { "Ю", ">" }, { ",", "<" }, { ".", ">" }, { "ж", ";" }, { "э", "'" }, { "Ж", ";" }, { "Э", "'" },
    { ":", ";" }, { "\"", "'" }, { "F", "f" }, { "M", "m" }, { "A", "a" }, { "E", "e" }, { "{", "[" }, { "}", "]" },
    { "|", "\\" }, { "=", "+" }, { "_", "-" },
  };
  for (auto &m : map) if (strcmp(key, m.from) == 0) return m.to;
  return key;
}

// Обрабатывает клавиши просмотра; Esc/F10 закрывают вкладку без смены режима.
int64_t OnKey(void *user, const char *rawKey)
{
  Use use(static_cast<Player *>(user));
  if (!rawKey) return 0;
  std::string k = NormalizeKey(rawKey);
  if (k == "Esc" || k == "F10") {
    // Хост ещё использует поверхность после возврата OnKey: не удаляем её здесь.
    if (!g->closing && g->view && PostMessageW(g->view, WM_CLOSE_TAB, 0, 0)) g->closing = true;
    return 1;
  }
  if (g->closing) return 1;
  if (!g->mpv) return 0;
  bool video = g->loaded && !g->isImage;
  const double step = 60;   // pan step in pixels

  if (k == "Alt+Left")  { PanBy(step, 0); }
  else if (k == "Alt+Right") { PanBy(-step, 0); }
  else if (k == "Alt+Up")    { PanBy(0, step); }
  else if (k == "Alt+Down")  { PanBy(0, -step); }
  else if (k == "Left" || k == "Right") {
    int dir = k == "Left" ? -1 : 1;
    if (video) SeekBy(dir * 5.0);
    else if (Zoomed()) PanBy(-dir * step, 0);
    else GoTo(g->index + dir);
  }
  else if (k == "Shift+Left")  { if (video) SeekBy(-1); }
  else if (k == "Shift+Right") { if (video) SeekBy(1); }
  else if (k == "Ctrl+Left")   { if (video) SeekBy(-60); }
  else if (k == "Ctrl+Right")  { if (video) SeekBy(60); }
  else if (k == "Up" || k == "Down") {
    int dir = k == "Up" ? 1 : -1;
    if (video) ChangeVolume(dir * 5);
    else PanBy(0, dir * step);
  }
  else if (k == "Space") { if (video) TogglePause(); else GoTo(g->index + 1); }
  else if (k == "PageDown") {
    g->pageDownPending = true;
    ContinuePageDown();
  }
  else if (k == "Insert") {
    // As in the panels: mark / unmark the file and step to the next one. The
    // panel itself is updated when the tab closes (panel_select needs the
    // panels on screen).
    if (g->index >= 0 && g->index < (int)g->files.size()) {
      FileEntry &f = g->files[g->index];
      f.selected = !f.selected;
      g->marks[f.uri] = f.selected;
      Osd(f.selected ? "Selected" : "Unselected");
      GoTo(g->index + 1);
    }
  }
  else if (k == "PageUp" || k == "Backspace") { GoTo(g->index - 1); }
  else if (k == "Home") { GoTo(0); }
  else if (k == "End")  { GoTo((int)g->files.size() - 1); }
  else if (k == "+") { ZoomStep(0.25); }
  else if (k == "-") { ZoomStep(-0.25); }
  else if (k == "*" || k == "0") { ZoomFit(); }
  else if (k == "/" || k == "1") { ZoomActual(); }
  else if (k == "r" || k == ">") { Rotate(90); }
  else if (k == "R" || k == "<") { Rotate(-90); }
  else if (k == "[")  { if (video) ChangeRate(-1); }
  else if (k == "]")  { if (video) ChangeRate(1); }
  else if (k == "\\") { if (video) ChangeRate(0); }
  else if (k == "'")  { if (video) Command("frame-step"); }
  else if (k == ";")  { if (video) Command("frame-back-step"); }
  else if (k == "m")  { Command("cycle", "mute"); }
  else if (k == "a")  { Command("cycle", "aid"); Command("show-text", "Audio: ${aid} ${current-tracks/audio/lang}", "1500"); }
  else if (k == "e")  {
    g->endAction = (g->endAction + 1) % 3;
    SetSetting("endAction", std::to_string(g->endAction));
    Osd("At end: %s", kEndNames[g->endAction]);
  }
  else if (k == "f" || k == "Enter") { ToggleFullscreen(); }
  else return 0;
  UpdateInfo();
  return 1;
}

void OnTick(void *user)
{
  Use use(static_cast<Player *>(user));
  if (!g->surface || g->closing) return;
  UpdateInfo();
}

// What a closed tab leaves for the panel it was opened from.
struct PanelSync { std::string cursorUri; std::map<std::string, bool> marks; };

// Runs once the host is back on the panels after the tab closed.
void SyncPanel(void *user)
{
  std::unique_ptr<PanelSync> sync(static_cast<PanelSync *>(user));
  if (!g_host) return;
  // Only rows the active panel shows right now. For any other URI the host
  // would take the panel to that file's folder - and the active panel need
  // not be the one the tab was opened from (other tabs, the other side).
  std::vector<FileEntry> rows;
  ListFromPanel(rows);
  auto shown = [&](const std::string &uri) {
    for (const FileEntry &row : rows) if (row.uri == uri) return true;
    return false;
  };
  if (g_host->panel_select)
    for (const auto &m : sync->marks)
      if (shown(m.first)) g_host->panel_select(-1, m.second ? 3 : 4, m.first.c_str());
  if (g_host->panel_set_cursor && !sync->cursorUri.empty() && shown(sync->cursorUri))
    g_host->panel_set_cursor(-1, sync->cursorUri.c_str());
}

// The tab of player is gone (the view window and mpv went with the host's
// container): hand cursor and marks over to the panel, forget the Player.
void FinishClose(Player *player)
{
  if ((!player->cursorUri.empty() || !player->marks.empty()) && g_host && g_host->post_to_main) {
    PanelSync *sync = new PanelSync{ player->cursorUri, player->marks };
    if (g_host->post_to_main(kPluginId, SyncPanel, sync) != 0) delete sync;
  }
  // Openings still under way would hold the host's file slots until they finish.
  if (g_host && g_host->vfs_cancel)
    for (auto &it : player->cache) if (!it.second.data && it.second.job > 0) g_host->vfs_cancel(it.second.job);
  g_players.erase(player->id);
}

void OnClosed(void *user)
{
  FinishClose(static_cast<Player *>(user));
}

// Closes the current tab; the host sends no closed callback for that.
void CloseTabFromPlugin()
{
  Player *player = g;
  int64_t surface = player->surface;
  // Таймер хоста больше не нужен, в том числе во время уничтожения контейнера.
  if (surface && g_host->surface_set_timer) g_host->surface_set_timer(surface, 0);
  player->surface = 0;
  if (surface) g_host->surface_close(surface);
  FinishClose(player);
}

bool OpenSurface(const std::wstring &name)
{
  int64_t mode = 256 + (GetSetting("fullscreen", "0") == "1" ? 1 : 0);
  std::string title = WideToUtf8(name);
  g->surface = g_host->surface_open_ex(kPluginId, title.c_str(), mode, OnKey, OnTick, OnClosed, nullptr, g);
  if (g->surface <= 0) { g->surface = 0; return false; }
  HWND container = reinterpret_cast<HWND>(g_host->surface_native_handle(g->surface));
  if (container) {
    // The host resizes this window to the area of the tab whenever it changes.
    RECT rc; GetClientRect(container, &rc);
    g->view = CreateWindowExW(0, kViewClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN,
                             0, 0, std::max(rc.right, 16L), std::max(rc.bottom, 16L), container, nullptr, g_module, g);
  }
  if (!g->view || !CreateMpv()) {
    int64_t s = g->surface;
    g->surface = 0;
    g_host->surface_close(s);   // destroys the view; no closed callback follows
    return false;
  }
  SetTimer(g->view, 1, 300, nullptr);
  g->prefFit = GetSetting("fit", "0") == "1";
  g->endAction = std::max(0, std::min(atoi(GetSetting("endAction", "0").c_str()), 2));
  g_host->surface_set_timer(g->surface, 250);
  return true;
}

int64_t OnDocument(void *, const char *uri, const char *, char *, int64_t)
{
  if (!g_mpv.Loaded() || !uri) return 0;
  FileEntry e = EntryFromUri(uri, true);
  if (!IsMediaPath(e.name)) return 0;

  // Every file opened from the panels gets a tab (and a Player) of its own.
  Player *player = new Player;
  player->id = g_nextPlayerId++;
  g_players[player->id].reset(player);
  Use use(player);
  // A remote file needs the host's leave to be read: ask before a tab opens.
  // The host answers panel_list only while the panels are on screen, and the
  // tab of the surface replaces them: take the list first.
  bool ok = e.local() || Fetch(e.uri);
  if (ok) { BuildFileList(e); ok = OpenSurface(e.name); }
  if (!ok) { g_players.erase(player->id); return 0; }
  Show(g->index);
  return 1;
}

std::string ExtensionList()
{
  std::string s;
  for (const char *e : kVideoExts) { s += s.empty() ? "." : ",."; s += e; }
  for (const char *e : kImageExts) { s += ",."; s += e; }
  return s;
}

}  // namespace

// MpvApi -------------------------------------------------------------------

bool MpvApi::Load(const wchar_t *dir)
{
  std::wstring path = std::wstring(dir) + L"\\libmpv-2.dll";
  hModule = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (!hModule) {
    Trace(L"mediaview: cannot load %s (error %u)", path.c_str(), GetLastError());
    return false;
  }
#define RESOLVE(name) \
  name = reinterpret_cast<pfn_mpv_##name>(GetProcAddress(hModule, "mpv_" #name)); \
  if (!name) { Trace(L"mediaview: missing export mpv_" L#name); Unload(); return false; }
  RESOLVE(client_api_version)
  RESOLVE(create)
  RESOLVE(initialize)
  RESOLVE(terminate_destroy)
  RESOLVE(set_option_string)
  RESOLVE(command)
  RESOLVE(set_property)
  RESOLVE(set_property_string)
  RESOLVE(get_property)
  RESOLVE(get_property_string)
  RESOLVE(free)
  RESOLVE(observe_property)
  RESOLVE(wait_event)
  RESOLVE(set_wakeup_callback)
  RESOLVE(error_string)
  RESOLVE(request_log_messages)
  RESOLVE(stream_cb_add_ro)
#undef RESOLVE
  if ((client_api_version() >> 16) != 2) { Unload(); return false; }
  return true;
}

void MpvApi::Unload()
{
  if (hModule) FreeLibrary(hModule);
  *this = MpvApi();
}

// Plugin entry points -------------------------------------------------------

extern "C" {

MTN_EXPORT int64_t mtn_plugin_get_abi_version(void) { return MTN_ABI_VERSION; }

MTN_EXPORT int64_t mtn_plugin_init(const MtnHostApi *host)
{
  // Needs MTN2 0.3.16 (dev builds from 0.3.15.6) or newer. Older builds also report ABI 2, but their
  // table ends earlier (before the surface functions up to 0.3.15.2, before
  // vfs_open up to 0.3.15.5), so these checks cannot recognise them reliably.
  if (!host || host->abi_version < 2 || !host->register_document_provider || !host->surface_open_ex ||
      !host->surface_native_handle || !host->surface_set_info || !host->surface_set_timer || !host->surface_close)
    return -1;
  g_host = host;

  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&mtn_plugin_init), &module);
  g_module = module;
  GetModuleFileNameW(module, g_pluginDir, MAX_PATH);
  if (wchar_t *p = wcsrchr(g_pluginDir, L'\\')) *p = 0;

  if (!g_mpv.Load(g_pluginDir)) {
    if (host->show_message) host->show_message("Media viewer: libmpv-2.dll is missing in the plugin folder.", 1);
    return -2;
  }

  // mpv paces frames with timed waits; the default 15.6 ms scheduler quantum
  // caps a 90 fps clip at ~64 fps. Timer resolution is per process.
  timeBeginPeriod(1);

  WNDCLASSW wc{};
  wc.lpfnWndProc = ViewProc;
  wc.hInstance = g_module;
  wc.lpszClassName = kViewClass;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.style = CS_DBLCLKS;
  g_classRegistered = RegisterClassW(&wc) != 0;

  host->register_document_provider(kPluginId, ExtensionList().c_str(), 1, OnDocument, nullptr, 50);
  return 0;
}

MTN_EXPORT void mtn_plugin_shutdown(void)
{
  // The host has closed the tabs (and with them the view windows) by now.
  for (auto &it : g_players) {
    Use use(it.second.get());
    if (g->view) DestroyWindow(g->view);
    DestroyMpv();
  }
  g_players.clear();
  if (g_classRegistered) { UnregisterClassW(kViewClass, g_module); g_classRegistered = false; }
  if (g_mpv.Loaded()) { g_mpv.Unload(); timeEndPeriod(1); }
  g_host = nullptr;
}

}  // extern "C"
