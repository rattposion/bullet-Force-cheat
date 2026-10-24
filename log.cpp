// ============================================================================
//  log.cpp
//  Log unico do projeto. Thread-safe. LogInit() e idempotente.
//
//  v2:
//   - abre com _SH_DENYNO (compartilhamento liberado -> o menu pode ler)
//   - mantem um RING BUFFER em memoria com as ultimas linhas, para o menu
//     ler direto (imune a lock de arquivo)
//  >>> FIX v3: mutexes RECURSIVOS. Log() e' chamado de dentro dos __except;
//      se o AV acontecer DENTRO do RingPush (com g_ringMx preso), o Log do
//      handler tentava pegar o mesmo mutex nao-recursivo -> deadlock e o
//      frame nunca fechava.
// ============================================================================

#include <windows.h>
#include <share.h>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>

extern HMODULE g_hModule;

void Log(const char* fmt, ...);
void LogInit();

static FILE* g_log = nullptr;

static std::vector<std::string> g_q;
static std::recursive_mutex      g_qMx;      // >>> FIX
static std::atomic<bool>         g_stop{ false };
static HANDLE                    g_ev = nullptr;
static HANDLE                    g_th = nullptr;

static const size_t kMaxQueue = 8192;

// ---------------------------------------------------------------------------
//  RING BUFFER (o menu le daqui - sem tocar no arquivo)
// ---------------------------------------------------------------------------
static const size_t kRingMax = 1024;
static std::recursive_mutex g_ringMx;        // >>> FIX
static std::string  g_ring[kRingMax];
static size_t       g_ringHead = 0;
static size_t       g_ringCount = 0;

static void RingPush(const char* line)
{
    std::lock_guard<std::recursive_mutex> lk(g_ringMx);
    g_ring[g_ringHead] = line;
    g_ringHead = (g_ringHead + 1) % kRingMax;
    if (g_ringCount < kRingMax) ++g_ringCount;
}

size_t LogSnapshot(char* dst, size_t cap)
{
    if (!dst || cap < 2) return 0;
    std::lock_guard<std::recursive_mutex> lk(g_ringMx);

    size_t n = 0;
    const size_t start = (g_ringCount == kRingMax) ? g_ringHead : 0;
    for (size_t i = 0; i < g_ringCount; ++i) {
        const std::string& s = g_ring[(start + i) % kRingMax];
        if (n + s.size() + 1 >= cap) break;
        memcpy(dst + n, s.data(), s.size());
        n += s.size();
        dst[n++] = '\n';
    }
    dst[n] = 0;
    return n;
}

void LogClearRing()
{
    std::lock_guard<std::recursive_mutex> lk(g_ringMx);
    g_ringCount = 0;
    g_ringHead = 0;
}

// ---------------------------------------------------------------------------
static void BuildPath(char* path, size_t n) {
    path[0] = 0;
    HMODULE mod = g_hModule ? g_hModule : GetModuleHandleA(nullptr);
    GetModuleFileNameA(mod, path, (DWORD)n);

    char* s = strrchr(path, '\\');
    if (s) s[1] = 0; else path[0] = 0;
    strncat_s(path, n, "BFInternal.log", _TRUNCATE);
}

// ---------------------------------------------------------------------------
static DWORD WINAPI LogThread(LPVOID) {
    std::vector<std::string> local;

    for (;;) {
        WaitForSingleObject(g_ev, 200);

        local.clear();
        {
            std::lock_guard<std::recursive_mutex> lk(g_qMx);
            local.swap(g_q);
        }

        if (g_log && !local.empty()) {
            for (size_t i = 0; i < local.size(); ++i) fputs(local[i].c_str(), g_log);
            fflush(g_log);
        }

        if (g_stop.load()) {
            bool more = false;
            { std::lock_guard<std::recursive_mutex> lk(g_qMx); more = !g_q.empty(); }
            if (!more) break;
        }
    }

    if (g_log) fflush(g_log);
    return 0;
}

// ---------------------------------------------------------------------------
void LogInit() {
    if (g_log) return;                       // idempotente

    char path[MAX_PATH] = { 0 };
    BuildPath(path, sizeof(path));

    // _SH_DENYNO: permite que OUTROS handles abram o arquivo (o tap do menu)
    g_log = _fsopen(path, "w", _SH_DENYNO);
    if (!g_log) return;

    if (!g_ev) g_ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (g_ev && !g_th) {
        DWORD id = 0;
        g_th = CreateThread(nullptr, 0, &LogThread, nullptr, 0, &id);
    }

    Log("log aberto: %s", path);
}

// ---------------------------------------------------------------------------
static void WriteSync(const char* line) {
    if (!g_log) return;
    fputs(line, g_log);
    fflush(g_log);
}

void Log(const char* fmt, ...) {
    if (!g_log) return;

    SYSTEMTIME st;
    GetLocalTime(&st);

    char body[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(body, sizeof(body), _TRUNCATE, fmt, ap);
    va_end(ap);

    char line[1024];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "[%02d:%02d:%02d.%03d] %s\n",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, body);

    RingPush(line);                            // <-- menu le daqui

    if (!g_th) { WriteSync(line); return; }    // fallback sincrono

    {
        std::lock_guard<std::recursive_mutex> lk(g_qMx);
        if (g_q.size() < kMaxQueue) g_q.emplace_back(line);
    }
    SetEvent(g_ev);
}