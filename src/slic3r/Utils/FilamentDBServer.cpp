///|/ Copyright (c) 2026
///|/
///|/ PrusaSlicer Filament Edition
///|/
#include "FilamentDBServer.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/asio/error.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/filesystem/operations.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>
#ifdef _WIN32
    // boost/process.hpp is the platform-INDEPENDENT convenience header and
    // deliberately leaves out the extension headers, so it never declares
    // boost::process::windows. Pull it in explicitly for create_no_window.
    #include <boost/process/windows.hpp>
#endif

#include "libslic3r/AppConfig.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/Utils/Http.hpp"

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#else
    #include <csignal>
#endif

namespace Slic3r {

namespace bp = boost::process;
namespace fs = boost::filesystem;

// AppConfig keys. Only `filamentdb_autostart` has a default (see
// AppConfig::set_defaults); the three path overrides stay empty unless the user
// fills them in, in which case they win over auto-detection.
static const char *KEY_AUTOSTART  = "filamentdb_autostart";
static const char *KEY_APP_EXE    = "filamentdb_app_path";
static const char *KEY_MONGOD_EXE = "filamentdb_mongod_path";
static const char *KEY_DATA_DIR   = "filamentdb_data_dir";

// The endpoint the slicer's preset sync actually uses. Polling it is the most
// meaningful readiness signal there is: a 200 proves the Next.js server is up
// AND that it reached MongoDB. The app has no dedicated health route.
static const char *PRESET_ENDPOINT = "api/filaments/prusaslicer";

// Defined further down; the fallback path in start() needs it earlier.
static bool start_desktop_app(const fs::path &app_exe, std::string &error);

// ---------------------------------------------------------------------------
// Small URL helpers
// ---------------------------------------------------------------------------

// Where the host part of `url` begins, skipping the scheme.
static size_t url_host_begin(const std::string &url)
{
    const size_t scheme = url.find("//");
    return (scheme == std::string::npos) ? 0 : scheme + 2;
}

static std::string host_from_url(const std::string &url)
{
    size_t start = url_host_begin(url);
    if (start < url.size() && url[start] == '[') {
        // IPv6 literal: the address sits inside the brackets.
        const size_t close = url.find(']', start);
        if (close != std::string::npos)
            return url.substr(start + 1, close - start - 1);
    }
    const size_t end = url.find_first_of(":/?", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

// The port spelled out in the URL, or -1 when it carries none.
static int explicit_port_from_url(const std::string &url)
{
    size_t start = url_host_begin(url);
    if (start < url.size() && url[start] == '[') {
        // Skip the bracketed IPv6 literal, or its colons would read as a port.
        const size_t close = url.find(']', start);
        if (close == std::string::npos)
            return -1;
        start = close + 1;
    }
    const size_t path  = url.find_first_of("/?", start);
    const size_t colon = url.find(':', start);
    if (colon == std::string::npos || (path != std::string::npos && colon > path))
        return -1;

    int port = 0;
    for (size_t i = colon + 1; i < url.size() && std::isdigit(static_cast<unsigned char>(url[i])); ++i)
        port = port * 10 + (url[i] - '0');
    return (port > 0 && port < 65536) ? port : -1;
}

// The port the slicer will actually talk to. When the URL names none, that is
// whatever libcurl defaults to for the scheme -- NOT the Filament DB's usual
// 3456. Guessing 3456 here would start a server on a port the probe never
// dials, and the mismatch would be invisible.
static int port_from_url(const std::string &url)
{
    const int explicit_port = explicit_port_from_url(url);
    if (explicit_port > 0)
        return explicit_port;
    return boost::istarts_with(url, "https://") ? 443 : 80;
}

// Only a server on this machine is ours to start. A URL pointing somewhere else
// belongs to someone else's instance and must be left alone.
static bool is_loopback_host(const std::string &host)
{
    return boost::iequals(host, "localhost") || host == "127.0.0.1" || host == "::1"
        || boost::starts_with(host, "127.");
}

static std::string join_url(const std::string &base, const char *suffix)
{
    std::string url = base;
    if (!url.empty() && url.back() != '/')
        url += '/';
    return url + suffix;
}

static fs::path env_path(const char *name)
{
    const char *v = std::getenv(name);
    return (v == nullptr || *v == '\0') ? fs::path() : fs::path(v);
}

// fs::exists that reports failure instead of throwing. Every probe below walks
// paths built from the environment or the registry, which can be unreachable
// network drives or plain garbage -- neither is worth an exception.
static bool path_exists(const fs::path &p)
{
    if (p.empty())
        return false;
    boost::system::error_code ec;
    return fs::exists(p, ec) && !ec;
}

// Is anything already listening on this loopback port? A better question than
// "does it answer HTTP", because a server that is still booting refuses the
// connection while very much being on its way up.
//
// Deliberately a plain blocking connect. An earlier version wrapped this in an
// asio timer so a hypothetical dropped SYN could not stall start-up, and
// reported any ambiguous outcome as "occupied" -- which under load produced a
// false positive, skipped the spawn altogether and left the user with a dead
// Filament DB and one line in a log. Loopback does not drop: a refusal takes
// single-digit milliseconds. Anything genuinely undecidable is reported as
// FREE, because starting a second backend at worst costs a mongod that exits
// on the lock file (and says so), whereas not starting one costs the feature.
static bool is_port_bound(int port)
{
    if (port <= 0)
        return false;

    for (const char *addr : { "127.0.0.1", "::1" }) {
        try {
            boost::system::error_code ec;
            const auto address = boost::asio::ip::make_address(addr, ec);
            if (ec)
                continue;

            boost::asio::io_context      io;
            boost::asio::ip::tcp::socket socket(io);
            socket.connect(
                boost::asio::ip::tcp::endpoint(address, static_cast<unsigned short>(port)), ec);
            if (!ec) {
                boost::system::error_code ignored;
                socket.close(ignored);
                return true;
            }
        } catch (const std::exception &) {
            // An unusable address family means nothing is there on it.
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// Path detection
// ---------------------------------------------------------------------------

bool FilamentDBServerPaths::complete() const
{
    return !app_exe.empty() && !server_js.empty() && !mongod_exe.empty() && !data_dir.empty();
}

std::string FilamentDBServerPaths::missing() const
{
    std::vector<std::string> gaps;
    if (app_exe.empty())    gaps.emplace_back("the Filament DB application");
    if (server_js.empty())  gaps.emplace_back("its bundled server (standalone/server.js)");
    if (mongod_exe.empty()) gaps.emplace_back("the MongoDB binary (mongod)");
    if (data_dir.empty())   gaps.emplace_back("the database directory");

    std::string out;
    for (size_t i = 0; i < gaps.size(); ++i) {
        if (i > 0)
            out += (i + 1 == gaps.size()) ? " and " : ", ";
        out += gaps[i];
    }
    return out;
}

#ifdef _WIN32
// Walk the uninstall registry, which is where the NSIS installer records the
// app. Matching on DisplayName rather than the appId-derived GUID keeps this
// working if electron-builder ever re-derives that GUID.
static fs::path find_app_exe_from_registry()
{
    struct Root { HKEY root; REGSAM extra; };
    static const Root roots[] = {
        { HKEY_CURRENT_USER,  0 },                 // per-user install (the default)
        { HKEY_LOCAL_MACHINE, 0 },
        { HKEY_LOCAL_MACHINE, KEY_WOW64_32KEY },
    };

    for (const Root &r : roots) {
        HKEY uninstall = nullptr;
        if (::RegOpenKeyExW(r.root, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                            0, KEY_READ | r.extra, &uninstall) != ERROR_SUCCESS)
            continue;

        fs::path found;
        for (DWORD i = 0; found.empty(); ++i) {
            wchar_t sub_name[512];
            DWORD   sub_len = static_cast<DWORD>(std::size(sub_name));
            if (::RegEnumKeyExW(uninstall, i, sub_name, &sub_len, nullptr, nullptr, nullptr, nullptr)
                != ERROR_SUCCESS)
                break;

            HKEY sub = nullptr;
            if (::RegOpenKeyExW(uninstall, sub_name, 0, KEY_READ | r.extra, &sub) != ERROR_SUCCESS)
                continue;

            auto read = [sub](const wchar_t *value) -> std::wstring {
                wchar_t buf[1024];
                DWORD   size = sizeof(buf);
                DWORD   type = 0;
                if (::RegQueryValueExW(sub, value, nullptr, &type,
                                       reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS
                    && (type == REG_SZ || type == REG_EXPAND_SZ)) {
                    // The value is not required to be null-terminated.
                    const size_t chars = size / sizeof(wchar_t);
                    return std::wstring(buf, ::wcsnlen(buf, chars));
                }
                return {};
            };

            if (boost::starts_with(read(L"DisplayName"), std::wstring(L"Filament DB"))) {
                // InstallLocation is empty for this installer, so DisplayIcon is
                // the only value that leads anywhere. It carries an ",<index>"
                // icon suffix, and other Electron apps point it at an .ico
                // instead of the executable -- so fall back to its directory.
                std::wstring icon = read(L"DisplayIcon");
                if (!icon.empty()) {
                    const size_t comma = icon.find_last_of(L',');
                    // A suffix only, never a comma inside a directory name.
                    if (comma != std::wstring::npos
                        && icon.find_first_of(L"\\/", comma) == std::wstring::npos)
                        icon.erase(comma);
                    const fs::path p(icon);
                    found = boost::iends_with(p.string(), ".exe")
                          ? p
                          : p.parent_path() / L"Filament DB.exe";
                }
                if (!path_exists(found)) {
                    const std::wstring loc = read(L"InstallLocation");
                    found = loc.empty() ? fs::path() : fs::path(loc) / L"Filament DB.exe";
                }
                if (!path_exists(found))
                    found.clear();
            }
            ::RegCloseKey(sub);
        }
        ::RegCloseKey(uninstall);
        if (!found.empty())
            return found;
    }
    return {};
}
#endif // _WIN32

// The Electron binary; started with ELECTRON_RUN_AS_NODE=1 it doubles as the
// Node runtime that runs the bundled Next.js server. (The shipped build has the
// runAsNode fuse enabled, so this is a supported mode rather than a trick.)
static fs::path detect_app_exe()
{
#ifdef _WIN32
    if (fs::path p = find_app_exe_from_registry(); !p.empty())
        return p;
    for (const char *base : { "LOCALAPPDATA", "PROGRAMFILES" }) {
        const fs::path p = env_path(base) / "Programs" / "Filament DB" / "Filament DB.exe";
        if (path_exists(p))
            return p;
        const fs::path q = env_path(base) / "Filament DB" / "Filament DB.exe";
        if (path_exists(q))
            return q;
    }
#elif defined(__APPLE__)
    for (const fs::path &base : { fs::path("/Applications"), env_path("HOME") / "Applications" }) {
        const fs::path p = base / "Filament DB.app" / "Contents" / "MacOS" / "Filament DB";
        if (path_exists(p))
            return p;
    }
#else
    for (const char *p : { "/opt/Filament DB/filament-db", "/usr/lib/filament-db/filament-db",
                           "/usr/bin/filament-db" })
        if (path_exists(fs::path(p)))
            return fs::path(p);
#endif
    return {};
}

// <app>/resources/app/standalone/server.js, with the macOS bundle layout as the
// second candidate. electron-builder runs with asar:false, so this really is a
// plain file on disk rather than an archive member.
static fs::path detect_server_js(const fs::path &app_exe)
{
    if (app_exe.empty())
        return {};
    const fs::path tail = fs::path("app") / "standalone" / "server.js";
    const fs::path candidates[] = {
        app_exe.parent_path() / "resources" / tail,                    // Windows / Linux
        app_exe.parent_path().parent_path() / "Resources" / tail,      // macOS bundle
    };
    for (const fs::path &c : candidates)
        if (path_exists(c))
            return c;
    return {};
}

// mongodb-memory-server downloads mongod into a shared cache. The file name
// carries the version, so pick the newest rather than hardcoding one.
static fs::path detect_mongod()
{
    std::vector<fs::path> dirs;
    if (fs::path d = env_path("MONGOMS_DOWNLOAD_DIR"); !d.empty())
        dirs.push_back(d);
#ifdef _WIN32
    dirs.push_back(env_path("USERPROFILE") / ".cache" / "mongodb-binaries");
    dirs.push_back(env_path("LOCALAPPDATA") / "mongodb-binaries");
#else
    dirs.push_back(env_path("HOME") / ".cache" / "mongodb-binaries");
#endif

    fs::path    best;
    std::string best_key;
    for (const fs::path &dir : dirs) {
        boost::system::error_code ec;
        if (dir.empty() || !fs::is_directory(dir, ec))
            continue;
        for (fs::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (!boost::starts_with(name, "mongod"))
                continue;
            if (!fs::is_regular_file(it->path(), ec))
                continue;
            // Compare version components numerically -- "8.10.0" is newer than
            // "8.2.1", which a plain string compare gets backwards.
            std::string key;
            for (size_t i = 0; i < name.size();) {
                if (std::isdigit(static_cast<unsigned char>(name[i]))) {
                    size_t j = i;
                    while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j])))
                        ++j;
                    key += std::string(8 - std::min<size_t>(8, j - i), '0');
                    key += name.substr(i, j - i);
                    i = j;
                } else {
                    key += name[i++];
                }
            }
            if (best.empty() || best_key < key) {
                best     = it->path();
                best_key = key;
            }
        }
    }
    return best;
}

// The very directory the desktop app uses, so both see the same filaments.
static fs::path detect_data_dir()
{
#ifdef _WIN32
    const fs::path base = env_path("APPDATA") / "filament-db";
#elif defined(__APPLE__)
    const fs::path base = env_path("HOME") / "Library" / "Application Support" / "filament-db";
#else
    const fs::path base = env_path("HOME") / ".config" / "filament-db";
#endif
    const fs::path dir = base / "mongodb-data";
    boost::system::error_code ec;
    return (fs::is_directory(dir, ec) && !ec) ? dir : fs::path();
}

FilamentDBServerPaths detect_filamentdb_server_paths()
{
    FilamentDBServerPaths out;
    try {
        const AppConfig *cfg = GUI::wxGetApp().app_config;
        auto override_path = [cfg](const char *key) -> fs::path {
            if (cfg == nullptr)
                return {};
            std::string v = cfg->get(key);
            boost::trim(v);
            return v.empty() ? fs::path() : fs::path(v);
        };

        out.app_exe    = override_path(KEY_APP_EXE);
        out.mongod_exe = override_path(KEY_MONGOD_EXE);
        out.data_dir   = override_path(KEY_DATA_DIR);

        if (!path_exists(out.app_exe))
            out.app_exe = detect_app_exe();
        if (!path_exists(out.mongod_exe))
            out.mongod_exe = detect_mongod();

        boost::system::error_code ec;
        if (out.data_dir.empty() || !fs::is_directory(out.data_dir, ec) || ec)
            out.data_dir = detect_data_dir();

        out.server_js = detect_server_js(out.app_exe);
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "FilamentDB server: path detection failed: " << ex.what();
    }
    return out;
}

// ---------------------------------------------------------------------------
// FilamentDBServer
// ---------------------------------------------------------------------------

struct FilamentDBServer::Impl
{
    std::unique_ptr<bp::child> mongod;
    std::unique_ptr<bp::child> server;
    bool started_by_us{false};
    std::thread       watcher;
    std::atomic<bool> stop_watcher{false};
#ifdef _WIN32
    // Both children join this job, which is set to kill them when the last
    // handle closes -- so they die with the slicer even if it crashes and no
    // destructor ever runs. An orphaned mongod would keep the shared database
    // locked and stop the Filament DB app from starting at all.
    HANDLE job{nullptr};
#endif
};

FilamentDBServer::FilamentDBServer() : p(new Impl) {}

FilamentDBServer::~FilamentDBServer() { shutdown(); }

FilamentDBServer& FilamentDBServer::instance()
{
    static FilamentDBServer s_instance;
    return s_instance;
}

bool FilamentDBServer::started_by_us() const { return p->started_by_us; }

bool FilamentDBServer::is_reachable(const std::string &url, int timeout_ms, unsigned *out_status)
{
    if (out_status != nullptr)
        *out_status = 0;
    if (url.empty())
        return false;

    bool     ok   = false;
    unsigned seen = 0;
    // Round up, so 1500 ms does not silently become 1 s. Connecting to loopback
    // is instant, but this endpoint queries MongoDB and serialises the whole
    // preset bundle, so the transfer leg needs the larger share.
    const long total   = std::max<long>(1, (timeout_ms + 999) / 1000);
    const long connect = std::max<long>(1, std::min<long>(total, 2));
    try {
        Http::get(join_url(url, PRESET_ENDPOINT))
            .timeout_connect(connect)
            .timeout_max(total)
            .on_complete([&](std::string /*body*/, unsigned status) {
                seen = status;
                ok   = (status == 200);
            })
            .on_error([&](std::string /*body*/, std::string /*error*/, unsigned status) {
                seen = status;
            })
            .perform_sync();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(debug) << "FilamentDB server: probe failed: " << ex.what();
    }
    if (out_status != nullptr)
        *out_status = seen;
    return ok;
}

// Ask the OS for an unused port by binding to port 0 and reading back what we
// got. There is a small race between closing and mongod binding it, which is
// the same race every "find a free port" helper has and is tolerable here.
static int find_free_port()
{
    try {
        boost::asio::io_context io;
        boost::asio::ip::tcp::acceptor acceptor(
            io, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 0));
        const int port = acceptor.local_endpoint().port();
        acceptor.close();
        return port;
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "FilamentDB server: could not pick a port: " << ex.what();
        return 0;
    }
}

bool FilamentDBServer::start(const std::string &url, std::string &error)
{
    error.clear();
    if (url.empty()) {
        error = "no Filament DB URL configured";
        return false;
    }

    // A URL that is not on this machine belongs to somebody else's server.
    const std::string host = host_from_url(url);
    if (!is_loopback_host(host)) {
        error = "the Filament DB URL points at " + host + ", which is not this machine";
        return false;
    }

    // Somebody already owns this port -- the user's own Filament DB app, or a
    // backend from an earlier run. Never start a second one: it would collide
    // on the MongoDB lock, and launching the desktop app again only produces
    // its "already running" error box. It is on its way up; that is enough.
    const int port = port_from_url(url);
    if (is_port_bound(port)) {
        BOOST_LOG_TRIVIAL(info) << "FilamentDB server: port " << port << " is already served";
        return true;
    }

    if (p->started_by_us && p->mongod && p->server)
        return true;    // ours, already spawned

    BOOST_LOG_TRIVIAL(info) << "FilamentDB server: port " << port << " is free, starting the backend";

    const FilamentDBServerPaths paths = detect_filamentdb_server_paths();

    if (paths.complete()) {
        if (spawn_backend(paths, url, error))
            return true;
        BOOST_LOG_TRIVIAL(warning) << "FilamentDB server: headless start failed: " << error;
    } else {
        error = "could not locate " + paths.missing();
        BOOST_LOG_TRIVIAL(warning) << "FilamentDB server: " << error;
    }

    // Fall back to the app's own supported entry point. It puts a window on the
    // screen, but it survives Filament DB updates that move the internals the
    // headless path depends on.
    if (paths.app_exe.empty())
        return false;

    std::string fallback_error;
    if (start_desktop_app(paths.app_exe, fallback_error)) {
        error.clear();
        return true;
    }
    if (!fallback_error.empty())
        error += error.empty() ? fallback_error : ("; " + fallback_error);
    return false;
}

bool FilamentDBServer::wait_ready(const std::string &url, int timeout_ms)
{
    return wait_until_reachable(url, timeout_ms);
}

void FilamentDBServer::watch_until_ready(const std::string &url, int timeout_ms,
                                         std::function<void()> on_ready)
{
    if (url.empty() || p->watcher.joinable())
        return;

    p->stop_watcher = false;
    p->watcher = std::thread([this, url, timeout_ms, on_ready]() {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::milliseconds(timeout_ms);
        while (!p->stop_watcher && std::chrono::steady_clock::now() < deadline) {
            if (is_reachable(url, 4000)) {
                BOOST_LOG_TRIVIAL(info) << "FilamentDB server: became reachable";
                if (on_ready && !p->stop_watcher)
                    on_ready();
                return;
            }
            // A child of ours that died is never coming back.
            try {
                if (p->started_by_us
                    && ((p->mongod && !p->mongod->running())
                        || (p->server && !p->server->running()))) {
                    BOOST_LOG_TRIVIAL(warning)
                        << "FilamentDB server: " << child_failure_reason();
                    return;
                }
            } catch (const std::exception &) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!p->stop_watcher)
            BOOST_LOG_TRIVIAL(warning) << "FilamentDB server: gave up waiting for " << url;
    });
}

bool FilamentDBServer::wait_until_reachable(const std::string &url, int timeout_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        // Give the probe a real share of what is left: the preset endpoint is
        // not a cheap health check.
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now()).count();
        if (is_reachable(url, static_cast<int>(std::min<long long>(5000, std::max<long long>(1000, left)))))
            return true;
        // A dead child will never come back -- stop waiting on it.
        try {
            if (p->started_by_us
                && ((p->mongod && !p->mongod->running()) || (p->server && !p->server->running())))
                return false;
        } catch (const std::exception &) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
    return false;
}

// Empty when both children are still alive, otherwise a description of the one
// that died -- including the most likely cause for mongod, which is that the
// Filament DB application already holds the database.
std::string FilamentDBServer::child_failure_reason() const
{
    try {
        if (p->mongod && !p->mongod->running())
            return "the database process exited immediately (exit code "
                 + std::to_string(p->mongod->exit_code())
                 + ") -- the Filament DB application is most likely already using it";
        if (p->server && !p->server->running())
            return "the Filament DB server process exited immediately (exit code "
                 + std::to_string(p->server->exit_code()) + ")";
    } catch (const std::exception &) {
        // Reporting is best effort; fall through to the generic message.
    }
    return {};
}

bool FilamentDBServer::spawn_backend(const FilamentDBServerPaths &paths,
                                     const std::string &url,
                                     std::string &error)
{
    const int mongo_port = find_free_port();
    if (mongo_port == 0) {
        error = "no free TCP port for the database";
        return false;
    }

    try {
#ifdef _WIN32
        if (p->job == nullptr) {
            p->job = ::CreateJobObjectW(nullptr, nullptr);
            if (p->job != nullptr) {
                JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                ::SetInformationJobObject(p->job, JobObjectExtendedLimitInformation,
                                          &limits, sizeof(limits));
            }
        }
#endif

        // The URL host is ASCII by construction -- start() has already
        // rejected anything that is not a loopback name.
        const std::string  host      = host_from_url(url);
        const std::string  mongo_uri = "mongodb://127.0.0.1:" + std::to_string(mongo_port)
                                     + "/filament-db";
        const std::string  http_port = std::to_string(port_from_url(url));

        // MongoDB first: the server connects lazily, but starting it the other
        // way round only makes the first request fail for no reason.
        //
        // On Windows everything goes through the WIDE chain -- executable,
        // arguments and environment together. Narrowing a path with
        // fs::path::string() would push it through the ANSI code page, which
        // silently destroys any character that page cannot represent, so an
        // install under a Cyrillic or CJK user name would never start. A
        // partial switch does not help: it just moves the loss into
        // boost::process::detail::convert.
#ifdef _WIN32
        const std::vector<std::wstring> mongo_args{
            L"--dbpath",        paths.data_dir.wstring(),
            L"--port",          std::to_wstring(mongo_port),
            L"--bind_ip",       L"127.0.0.1",
            L"--storageEngine", L"wiredTiger" };
        p->mongod.reset(new bp::child(
            paths.mongod_exe,
            bp::args(mongo_args),
            bp::std_out > bp::null, bp::std_err > bp::null, bp::std_in < bp::null,
            bp::windows::create_no_window));
        if (p->job != nullptr)
            ::AssignProcessToJobObject(p->job, p->mongod->native_handle());

        bp::wenvironment env = boost::this_process::wenvironment();
        env[L"ELECTRON_RUN_AS_NODE"] = L"1";
        env[L"NODE_ENV"]             = L"production";
        env[L"PORT"]                 = std::wstring(http_port.begin(), http_port.end());
        env[L"HOSTNAME"]             = std::wstring(host.begin(), host.end());
        env[L"MONGODB_URI"]          = std::wstring(mongo_uri.begin(), mongo_uri.end());

        const std::vector<std::wstring> server_args{ paths.server_js.wstring() };
        p->server.reset(new bp::child(
            paths.app_exe,
            bp::args(server_args),
            env,
            bp::std_out > bp::null, bp::std_err > bp::null, bp::std_in < bp::null,
            bp::windows::create_no_window));
        if (p->job != nullptr)
            ::AssignProcessToJobObject(p->job, p->server->native_handle());
#else
        const std::vector<std::string> mongo_args{
            "--dbpath",        paths.data_dir.string(),
            "--port",          std::to_string(mongo_port),
            "--bind_ip",       "127.0.0.1",
            "--storageEngine", "wiredTiger" };
        p->mongod.reset(new bp::child(
            paths.mongod_exe,
            bp::args(mongo_args),
            bp::std_out > bp::null, bp::std_err > bp::null, bp::std_in < bp::null));

        // A copy of our environment; the native one must not be modified.
        bp::environment env = boost::this_process::environment();
        // Makes the Electron binary behave as plain Node, so no separate Node
        // installation is needed to run the bundled server.
        env["ELECTRON_RUN_AS_NODE"] = "1";
        env["NODE_ENV"]             = "production";
        env["PORT"]                 = http_port;
        // Bind exactly the host the slicer will ask for. This MUST be set:
        // server.js defaults to 0.0.0.0, which would put an unauthenticated
        // filament database on every network interface.
        env["HOSTNAME"]             = host;
        env["MONGODB_URI"]          = mongo_uri;

        const std::vector<std::string> server_args{ paths.server_js.string() };
        p->server.reset(new bp::child(
            paths.app_exe,
            bp::args(server_args),
            env,
            bp::std_out > bp::null, bp::std_err > bp::null, bp::std_in < bp::null));
#endif
    } catch (const std::exception &ex) {
        error = std::string("could not start the Filament DB backend: ") + ex.what();
        shutdown();
        return false;
    }

    p->started_by_us = true;
    BOOST_LOG_TRIVIAL(info) << "FilamentDB server: spawned headless, MongoDB on port " << mongo_port;
    return true;
}

static bool start_desktop_app(const fs::path &app_exe, std::string &error)
{
    BOOST_LOG_TRIVIAL(info) << "FilamentDB server: falling back to the desktop application";
#ifdef _WIN32
    // Minimised and without stealing focus -- the user asked for the slicer, not
    // for a database window in their face.
    const std::wstring exe = app_exe.wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask  = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = exe.c_str();
    info.nShow  = SW_SHOWMINNOACTIVE;
    if (!::ShellExecuteExW(&info)) {
        error = "could not launch the Filament DB application";
        return false;
    }
    if (info.hProcess != nullptr)
        ::CloseHandle(info.hProcess);
    return true;
#else
    try {
        // A path, not a string: the fs::path overload is the only one that marks
        // this as an executable rather than a command line to be word-split, and
        // the macOS candidates all contain a space.
        bp::spawn(app_exe);
        return true;
    } catch (const std::exception &ex) {
        error = std::string("could not launch the Filament DB application: ") + ex.what();
        return false;
    }
#endif
}

// Ask nicely, then insist.
//
// On POSIX that means SIGTERM, which mongod handles as a clean shutdown, with
// SIGKILL only if it refuses to go. On Windows there is no equivalent: a GUI
// process has no console, so GenerateConsoleCtrlEvent is unavailable, and
// TerminateProcess is all that is left. That is exactly what the Filament DB
// app itself does -- Node's child.kill() terminates unconditionally on Windows
// -- and WiredTiger's journal makes it crash-safe either way.
static void stop_child(std::unique_ptr<bp::child> &child, int grace_ms)
{
    if (!child)
        return;
    try {
        if (child->running()) {
#ifndef _WIN32
            ::kill(child->id(), SIGTERM);
            const auto deadline = std::chrono::steady_clock::now()
                                + std::chrono::milliseconds(grace_ms);
            while (child->running() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
#else
            (void) grace_ms;
#endif
            if (child->running())
                child->terminate();
        }
        child->wait();
    } catch (const std::exception &ex) {
        BOOST_LOG_TRIVIAL(warning) << "FilamentDB server: error while stopping a child: " << ex.what();
    }
    child.reset();
}

void FilamentDBServer::shutdown(int grace_ms)
{
    // Stop the watcher before the children, so it cannot observe a half-torn
    // down backend and fire its ready callback into a shutting-down GUI.
    p->stop_watcher = true;
    if (p->watcher.joinable())
        p->watcher.join();

    if (!p->started_by_us && !p->mongod && !p->server)
        return;

    // A caller on a deadline splits its own budget; application exit keeps the
    // generous defaults. stop_child degenerates to an immediate kill at 0.
    const int server_grace = grace_ms < 0 ? 3000  : std::min(grace_ms, 1500);
    const int mongo_grace  = grace_ms < 0 ? 10000 : std::max(0, grace_ms - server_grace);

    // The Next.js server first: it is stateless, and stopping it prevents new
    // writes from landing while the database is going down.
    stop_child(p->server, server_grace);
    // MongoDB gets longer -- it flushes and closes its WiredTiger files.
    stop_child(p->mongod, mongo_grace);

#ifdef _WIN32
    if (p->job != nullptr) {
        ::CloseHandle(p->job);
        p->job = nullptr;
    }
#endif

    p->started_by_us = false;
    BOOST_LOG_TRIVIAL(info) << "FilamentDB server: stopped";
}

bool filamentdb_autostart_enabled()
{
    const AppConfig *cfg = GUI::wxGetApp().app_config;
    return cfg != nullptr && cfg->get_bool(KEY_AUTOSTART);
}

} // namespace Slic3r
