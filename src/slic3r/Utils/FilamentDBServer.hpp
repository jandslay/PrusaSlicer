///|/ Copyright (c) 2026
///|/
///|/ PrusaSlicer Filament Edition
///|/
#ifndef slic3r_Utils_FilamentDBServer_hpp_
#define slic3r_Utils_FilamentDBServer_hpp_

#include <functional>
#include <memory>
#include <string>

#include <boost/filesystem/path.hpp>

namespace Slic3r {

// Everything needed to run the Filament DB backend without its Electron GUI.
//
// The Filament DB desktop app is an Electron shell around a Next.js server and
// an embedded MongoDB. All three pieces sit on disk unpacked (electron-builder
// runs with asar:false), so the backend can be started on its own:
//
//   mongod --dbpath <data_dir> --port <p> --bind_ip 127.0.0.1
//   ELECTRON_RUN_AS_NODE=1 <app_exe> <server_js>     (env: PORT, MONGODB_URI, ...)
//
// `app_exe` doubles as the Node runtime -- an Electron binary started with
// ELECTRON_RUN_AS_NODE=1 behaves like plain node, so no separate Node install
// is needed.
struct FilamentDBServerPaths
{
    boost::filesystem::path app_exe;    // "Filament DB" executable (also the Node runtime)
    boost::filesystem::path server_js;  // <app>/resources/app/standalone/server.js
    boost::filesystem::path mongod_exe; // from the mongodb-memory-server binary cache
    boost::filesystem::path data_dir;   // <appdata>/filament-db/mongodb-data

    bool complete() const;
    // Human-readable list of the pieces that could not be found, for the log
    // and for the Preferences hint. Empty when complete().
    std::string missing() const;
};

// Locate the pieces. AppConfig overrides win; anything left empty is probed at
// the platform's usual locations. Never throws.
FilamentDBServerPaths detect_filamentdb_server_paths();

// Owns the Filament DB backend processes for the lifetime of this application.
//
// Only ever starts something when nothing already answers on the configured
// URL, so a Filament DB desktop app the user started themselves is left alone
// -- and, just as importantly, we never fight it over the MongoDB data
// directory lock.
class FilamentDBServer
{
public:
    static FilamentDBServer& instance();

    FilamentDBServer(const FilamentDBServer&) = delete;
    FilamentDBServer& operator=(const FilamentDBServer&) = delete;

    // True when `url` answers the preset endpoint the slicer actually needs.
    // `out_status`, when given, receives the HTTP status that was observed (0
    // when the connection never got that far), which lets a caller tell a
    // foreign server apart from one that is still booting.
    static bool is_reachable(const std::string& url, int timeout_ms = 1500,
                             unsigned *out_status = nullptr);

    // Make sure the backend is coming up, WITHOUT waiting for it.
    //
    // Spawning is a matter of milliseconds; becoming answerable is not. A warm
    // backend serves its first request after ~3 s, but the very first start
    // after a reboot takes ~45 s, because Windows has to fault the Filament DB
    // installation (>200 MB, packaged unpacked) into its file cache. Blocking
    // the splash screen for that is not acceptable, so waiting is left to the
    // caller: a short wait_ready() for the warm case, and watch_until_ready()
    // for the rest.
    //
    // Returns false only if the backend could not be started at all; `error`
    // then says why. A missing Filament DB must never stop the slicer.
    bool start(const std::string& url, std::string& error);

    // Poll until `url` answers, at most `timeout_ms`. Blocking.
    bool wait_ready(const std::string& url, int timeout_ms);

    // Poll in the background and call `on_ready` once, from the worker thread,
    // as soon as `url` answers. Gives up after `timeout_ms`. At most one
    // watcher runs at a time; shutdown() stops it.
    void watch_until_ready(const std::string& url, int timeout_ms,
                           std::function<void()> on_ready);

    // Stop what this process started, gracefully, with a bounded fallback to a
    // hard kill. A no-op when we started nothing. Safe to call more than once.
    //
    // `grace_ms` < 0 keeps the generous defaults and is what application exit
    // wants. A concrete value caps the graceful phase, for callers that are
    // themselves on a deadline; 0 means "kill now" -- WiredTiger's journal
    // makes that recoverable.
    void shutdown(int grace_ms = -1);

    // True when the backend running right now was started by us.
    bool started_by_us() const;

private:
    FilamentDBServer();
    ~FilamentDBServer();

    // Poll `url` until it answers, giving up early if a child we started died.
    bool wait_until_reachable(const std::string &url, int timeout_ms);

    // Spawn mongod and the bundled server. Does not wait for either.
    bool spawn_backend(const FilamentDBServerPaths &paths, const std::string &url,
                       std::string &error);

    // Empty while both children are alive, otherwise why the start failed.
    std::string child_failure_reason() const;

    struct Impl;
    std::unique_ptr<Impl> p;
};

// True when the user wants the slicer to bring the backend up by itself
// (AppConfig key `filamentdb_autostart`, on by default).
bool filamentdb_autostart_enabled();

} // namespace Slic3r

#endif // slic3r_Utils_FilamentDBServer_hpp_
