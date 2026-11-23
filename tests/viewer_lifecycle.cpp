// Проверяет закрытие вкладки настоящими сообщениями Win32 без запуска MTN2.
#include "../src/plugin.cpp"

namespace {
int failures = 0;
int closes = 0, destroys = 0, cancels = 0, fileCloses = 0;
bool inKey = false;
bool surfaceTimerStopped = false;
HWND testView = nullptr;

// Проверка остаётся активной и при Release-сборке.
void Check(bool ok, const char *message)
{
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); ++failures; }
}

// Имитирует сохранение настройки без файловой системы хоста.
int64_t SaveSetting(const char *, const char *, const char *) { return 0; }

// Имитирует отключение периодических уведомлений поверхности.
int64_t StopSurfaceTimer(int64_t surface, int64_t interval)
{
  Check(surface == 1 && interval == 0, "surface timer is explicitly stopped");
  surfaceTimerStopped = true;
  return 0;
}

// Хост удаляет контейнер при surface_close, но не вызывает OnClosed.
int64_t CloseSurface(int64_t)
{
  Check(!inKey, "surface_close must run after OnKey returns");
  Check(surfaceTimerStopped, "surface timer stops before closing its container");
  ++closes;
  DestroyWindow(testView);
  testView = nullptr;
  return 0;
}

// Отмена открытия должна предшествовать удалению Player и его кэша.
int64_t CancelOpen(int64_t job)
{
  Check(job == 42, "pending VFS job is cancelled");
  ++cancels;
  return 0;
}

// Дескриптор VFS освобождается только после остановки потоков mpv.
int64_t CloseFile(int64_t handle)
{
  Check(handle == 17 && destroys == 1, "VFS file closes after mpv terminates");
  ++fileCloses;
  return 0;
}

// После отключения уведомлений mpv не должен отправлять новые пробуждения.
void ClearWakeup(mpv_handle *, void (*callback)(void *), void *)
{
  Check(callback == nullptr, "mpv wakeup is disabled before termination");
}

// Воспроизводит повторную доставку сообщений во время ожидания потоков mpv.
void Terminate(mpv_handle *)
{
  ++destroys;
  Check(g && !g->mpv, "mpv is detached before waiting for its threads");
  Check(!KillTimer(testView, 1), "window timer stops before mpv termination");
  SendMessageW(testView, WM_MPV_WAKEUP, 0, 0);
  SendMessageW(testView, WM_TIMER, 1, 0);
  DestroyMpv();   // Повторный вызов не должен повторно освобождать mpv.
}

// Создаёт просмотр с открытым файлом и незавершённым заданием VFS.
Player *MakePlayer()
{
  closes = destroys = cancels = fileCloses = 0;
  surfaceTimerStopped = false;
  auto player = std::make_unique<Player>();
  player->id = g_nextPlayerId++;
  player->surface = 1;
  player->mpv = reinterpret_cast<mpv_handle *>(1);
  player->cache["pending"].job = 42;
  player->cache["opened"].data = std::make_shared<RemoteFile>(17, 120ll * 1024 * 1024);
  Player *result = player.get();
  g_players.emplace(player->id, std::move(player));
  testView = CreateWindowExW(0, kViewClass, L"", 0, 0, 0, 16, 16,
                             nullptr, nullptr, g_module, result);
  result->view = testView;
  SetTimer(testView, 1, 300, nullptr);
  Check(testView != nullptr, "test viewer window is created");
  return result;
}

// Обрабатывает отложенное закрытие и проверяет освобождение всех ресурсов.
void DrainClose()
{
  MSG message;
  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
  Check(g_players.empty(), "Player is released");
  Check(destroys == 1 && cancels == 1 && fileCloses == 1, "mpv and VFS resources released exactly once");
  Check(g == nullptr, "current Player context is restored");
}

// Повтор клавиши до обработки очереди не должен породить повторное закрытие.
void TestKey(const char *key)
{
  Player *player = MakePlayer();
  inKey = true;
  Check(OnKey(player, key) == 1, "close key is consumed");
  Check(OnKey(player, key) == 1, "repeated close key is consumed");
  inKey = false;
  Check(closes == 0 && g_players.size() == 1, "surface and Player survive key callback");
  OnTick(player);   // Уведомление, уже поставленное до Esc, должно быть безвредно.
  DrainClose();
  Check(closes == 1, "only one deferred surface close");
}

// Хост может закрыть вкладку раньше доставки уже поставленного сообщения.
void TestHostClose()
{
  Player *player = MakePlayer();
  OnKey(player, "Esc");
  DestroyWindow(testView);
  testView = nullptr;
  OnClosed(player);
  DrainClose();
  Check(closes == 0, "stale close message does not call surface_close");
}

// Проверяет ответ Win32 на первый клик по неактивному просмотру и доставку этого клика.
void TestMouseActivation()
{
  Player player;
  HWND root = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPED, 0, 0, 100, 100,
                              nullptr, nullptr, g_module, nullptr);
  player.view = CreateWindowExW(0, kViewClass, L"", WS_CHILD, 0, 0, 100, 100,
                                root, nullptr, g_module, &player);
  HWND child = CreateWindowExW(0, L"STATIC", L"", WS_CHILD, 0, 0, 100, 100,
                               player.view, nullptr, g_module, nullptr);
  Check(root && player.view && child, "viewer window hierarchy is created");
  if (root && player.view && child) {
    Use use(&player);
    SubclassMpvChild();
    const auto host = reinterpret_cast<WPARAM>(root);
    const auto activation = MAKELPARAM(HTCLIENT, WM_LBUTTONDOWN);
    // До появления кадра клик принимает контейнер, после появления — дочернее окно mpv.
    Check(SendMessageW(player.view, WM_MOUSEACTIVATE, host, activation) == MA_ACTIVATE,
          "click on the viewer container activates the host and is retained");
    Check(SendMessageW(child, WM_MOUSEACTIVATE, host, activation) == MA_ACTIVATE,
          "click on the mpv child activates the host and is retained");

    // Доставляем первое нажатие через окно mpv и проверяем начало перетаскивания.
    player.lastClickTick = GetTickCount() - GetDoubleClickTime() - 1;
    SendMessageW(child, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(10, 10));
    MSG message{};
    while (PeekMessageW(&message, player.view, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_REMOVE))
      DispatchMessageW(&message);
    Check(player.dragging, "first click on the mpv child reaches the viewer");
  }
  if (root) DestroyWindow(root);
}

int64_t testEntry = 0;
std::vector<mpv_event> testEvents;
size_t nextEvent = 0;

// Имитирует создание новой записи плейлиста для каждой команды loadfile.
int TestLoadCommand(mpv_handle *, const char **args)
{
  if (strcmp(args[0], "loadfile") == 0) ++testEntry;
  return 0;
}

// Возвращает ID запрошенного файла и признаки загруженного изображения.
int TestImageProperty(mpv_handle *, const char *name, mpv_format format, void *data)
{
  if (strcmp(name, "playlist/0/id") == 0) {
    *static_cast<int64_t *>(data) = testEntry;
    return 0;
  }
  if (format == MPV_FORMAT_FLAG) {
    *static_cast<int *>(data) = 1;
    return 0;
  }
  return -1;
}

// Доставляет заданную тестом последовательность событий mpv.
mpv_event *NextTestEvent(mpv_handle *, double)
{
  return nextEvent < testEvents.size() ? &testEvents[nextEvent++] : nullptr;
}

// Проверяет объединение повторов и привязку перехода к первому кадру нужного файла.
void TestPageDown()
{
  g_mpv.command = TestLoadCommand;
  g_mpv.get_property = TestImageProperty;
  g_mpv.set_property = [](mpv_handle *, const char *, mpv_format, void *) { return 0; };
  g_mpv.wait_event = NextTestEvent;
  Player player;
  player.mpv = reinterpret_cast<mpv_handle *>(1);
  player.files = {
    {"a", L"a.png", L"a.png"}, {"b", L"b.png", L"b.png"},
    {"c", L"c.png", L"c.png"}, {"d", L"d.png", L"d.png"},
  };
  Use use(&player);
  Show(0);
  GoTo(1); // Первый файл заменён до обработки его событий.
  for (int i = 0; i < 10; ++i) OnKey(&player, "PageDown");
  Check(player.index == 1 && player.pageDownPending, "PageDown waits during image loading");

  mpv_event_start_file previous{testEntry - 1}, current{testEntry};
  mpv_event_end_file stopped{MPV_END_FILE_REASON_STOP, 0, previous.playlist_entry_id, 0, 0};
  auto drain = [](std::initializer_list<mpv_event> events) {
    testEvents.assign(events);
    nextEvent = 0;
    DrainEvents();
  };
  drain({{MPV_EVENT_START_FILE, 0, 0, &previous},
         {MPV_EVENT_PLAYBACK_RESTART, 0, 0, nullptr},
         {MPV_EVENT_END_FILE, 0, 0, &stopped},
         {MPV_EVENT_START_FILE, 0, 0, &current},
         {MPV_EVENT_FILE_LOADED, 0, 0, nullptr}});
  Check(player.index == 1 && player.pageDownPending, "stale events and FILE_LOADED cannot release PageDown");

  drain({{MPV_EVENT_PLAYBACK_RESTART, 0, 0, nullptr}});
  Check(player.index == 2 && !player.pageDownPending, "first frame releases exactly one PageDown");

  // Ошибка декодирования тоже освобождает переход, иначе листание застрянет.
  OnKey(&player, "PageDown");
  mpv_event_end_file failed{MPV_END_FILE_REASON_ERROR, -1, testEntry, 0, 0};
  g_mpv.error_string = [](int) { return "invalid image"; };
  drain({{MPV_EVENT_END_FILE, 0, 0, &failed}});
  Check(player.index == 3 && !player.pageDownPending, "failed image releases pending PageDown");
}
} // namespace

// Запускает сценарии отмены до окончания загрузки, включая повторный вход.
int main()
{
  MtnHostApi host{};
  host.set_setting = SaveSetting;
  host.surface_close = CloseSurface;
  host.surface_set_timer = StopSurfaceTimer;
  host.vfs_cancel = CancelOpen;
  host.vfs_close = CloseFile;
  g_host = &host;
  g_module = GetModuleHandleW(nullptr);
  g_mpv.set_wakeup_callback = ClearWakeup;
  g_mpv.terminate_destroy = Terminate;
  WNDCLASSW wc{};
  wc.lpfnWndProc = ViewProc;
  wc.hInstance = g_module;
  wc.lpszClassName = kViewClass;
  if (!RegisterClassW(&wc)) return 2;
  TestKey("Esc");
  TestKey("F10");
  TestHostClose();
  TestMouseActivation();
  TestPageDown();
  UnregisterClassW(kViewClass, g_module);
  g_host = nullptr;
  if (failures) return 1;
  std::puts("PASS: viewer lifecycle, VFS cleanup, mouse activation and PageDown");
  return 0;
}
