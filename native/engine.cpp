// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"
#include <libtorrent/session.hpp>
#include <libtorrent/alert_types.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/read_resume_data.hpp>
#include <libtorrent/write_resume_data.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/hex.hpp>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
namespace torrent {
namespace lt = libtorrent;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;
namespace {
std::string key(const lt::info_hash_t &hash) {
  if (hash.has_v2()) return lt::aux::to_hex(hash.v2);
  return lt::aux::to_hex(hash.v1);
}
std::vector<char> read(const fs::path &path) {
  auto length = fs::file_size(path);
  if (length > 32 * 1024 * 1024) throw std::runtime_error("State or torrent exceeds 32 MiB: " + path.string());
  std::ifstream f(path, std::ios::binary);
  std::vector<char> data(length);
  if (!f.read(data.data(), data.size())) throw std::runtime_error("Cannot read " + path.string());
  return data;
}
void write(const fs::path &path, const std::vector<char> &data) {
  auto temp = path.string() + ".tmp";
  int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) throw std::runtime_error("Cannot create resume state: " + temp);
  size_t offset = 0;
  while (offset < data.size()) {
    auto n = ::write(fd, data.data() + offset, data.size() - offset);
    if (n <= 0) { close(fd); unlink(temp.c_str()); throw std::runtime_error("Cannot write resume state"); }
    offset += size_t(n);
  }
  int result = fsync(fd); close(fd);
  if (result || rename(temp.c_str(), path.c_str())) throw std::runtime_error("Cannot commit resume state");
  int dir = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dir >= 0) { fsync(dir); close(dir); }
}
std::string state(const lt::torrent_status &s) {
  if (s.errc) return "Error";
  if (s.flags & lt::torrent_flags::paused) return "Paused";
  switch (s.state) {
    case lt::torrent_status::checking_files: return "Checking files";
    case lt::torrent_status::downloading_metadata: return "Finding metadata";
    case lt::torrent_status::downloading: return "Downloading";
    case lt::torrent_status::finished: return "Finished";
    case lt::torrent_status::seeding: return "Seeding";
    case lt::torrent_status::checking_resume_data: return "Checking resume";
    default: return "Starting";
  }
}
}
struct Engine::Impl {
  Options options;
  int lock = -1;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::deque<std::function<void()>> commands;
  Snapshot view;
  std::vector<std::string> messages;
  std::thread worker;
  std::unique_ptr<lt::session> session;
  std::map<std::string, lt::torrent_handle> handles;
  std::set<std::string> saving, resave;
  bool stopping = false;
  Clock::time_point deadline, lastSave = Clock::now();
  explicit Impl(Options value) : options(std::move(value)) {
    if (options.state.empty()) options.state = defaultState();
    fs::create_directories(options.state);
    chmod(options.state.c_str(), 0700);
    lock = open((options.state / "session.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (lock < 0 || flock(lock, LOCK_EX | LOCK_NB)) {
      if (lock >= 0) close(lock);
      throw std::runtime_error("Dome Torrent state is already in use or cannot be locked");
    }
    try { worker = std::thread([this] { run(); }); }
    catch (...) { close(lock); throw; }
  }
  ~Impl() {
    enqueue([this] { beginStop(); });
    if (worker.joinable()) worker.join();
    close(lock);
  }
  void enqueue(std::function<void()> f) {
    std::lock_guard<std::mutex> guard(mutex);
    if (!view.stopped) commands.push_back(std::move(f));
    wake.notify_one();
  }
  void message(std::string s) {
    messages.push_back(std::move(s));
    if (messages.size() > 40) messages.erase(messages.begin());
  }
  fs::path file(const std::string &id) const { return options.state / (id + ".resume"); }
  void save(const std::string &id, lt::torrent_handle h) {
    if (!saving.insert(id).second) { resave.insert(id); return; }
    h.save_resume_data(lt::torrent_handle::save_info_dict | lt::torrent_handle::flush_disk_cache);
  }
  void add(lt::add_torrent_params p, bool restored, std::string stableId = {}) {
    p.flags &= ~lt::torrent_flags::auto_managed;
    p.flags |= lt::torrent_flags::duplicate_is_error;
    auto hash = p.ti ? p.ti->info_hashes() : p.info_hashes;
    if (!hash.has_v1() && !hash.has_v2()) throw std::runtime_error("Torrent has no supported info hash");
    const auto id = stableId.empty() ? key(hash) : stableId;
    lt::error_code ec;
    auto h = session->add_torrent(p, ec);
    if (ec) throw std::runtime_error("Cannot add torrent: " + ec.message());
    try {
      // Persist the magnet before metadata exists, so restart can rediscover it.
      p.info_hashes = hash;
      if (!restored) write(file(id), lt::write_resume_data_buf(p));
    } catch (...) { session->remove_torrent(h); throw; }
    handles[id] = h;
    save(id, h);
    message(restored ? "Restored transfer" : "Added transfer");
  }
  lt::torrent_handle handle(const std::string &id) {
    auto i = handles.find(id);
    if (i == handles.end()) throw std::runtime_error("Select an available transfer");
    return i->second;
  }
  std::string idFor(lt::torrent_handle h) {
    for (const auto &item : handles) if (item.second == h) return item.first;
    return {};
  }
  void alerts() {
    std::vector<lt::alert *> alerts;
    session->pop_alerts(&alerts);
    for (auto a : alerts) {
      if (auto r = lt::alert_cast<lt::save_resume_data_alert>(a)) {
        auto id = idFor(r->handle);
        if (!id.empty()) {
          saving.erase(id);
          try { write(file(id), lt::write_resume_data_buf(r->params)); }
          catch (const std::exception &e) { message(e.what()); }
          if (resave.erase(id)) save(id, r->handle);
        }
      } else if (auto r = lt::alert_cast<lt::save_resume_data_failed_alert>(a)) {
        auto id = idFor(r->handle);
        saving.erase(id);
        message("Resume save failed: " + r->error.message());
        if (!id.empty() && resave.erase(id)) save(id, r->handle);
      } else if (auto r = lt::alert_cast<lt::metadata_received_alert>(a)) {
        auto id = idFor(r->handle);
        if (!id.empty()) save(id, r->handle);
      } else if (auto r = lt::alert_cast<lt::torrent_finished_alert>(a)) {
        auto id = idFor(r->handle);
        if (!id.empty()) save(id, r->handle);
      } else if (lt::alert_cast<lt::torrent_error_alert>(a) || lt::alert_cast<lt::listen_failed_alert>(a)) {
        message(a->message());
      }
    }
  }
  void publish(bool stopped = false) {
    Snapshot next;
    next.messages = messages;
    next.stopped = stopped;
    for (auto &[id, h] : handles) {
      auto s = h.status();
      Row r;
      r.id = id; r.name = s.name.empty() ? id : s.name; r.state = state(s);
      r.path = s.save_path; r.error = s.errc ? s.errc.message() : "";
      r.magnet = lt::make_magnet_uri(h); r.progress = s.progress;
      r.done = s.total_wanted_done; r.total = s.total_wanted;
      r.downloaded = s.all_time_download; r.uploaded = s.all_time_upload;
      r.downRate = s.download_payload_rate; r.upRate = s.upload_payload_rate;
      r.peers = s.num_peers; r.seeds = s.num_seeds;
      r.metadata = s.has_metadata; r.paused = bool(s.flags & lt::torrent_flags::paused);
      if (auto info = h.torrent_file())
        for (auto index : info->files().file_range()) r.files.push_back(info->files().file_path(index));
      next.rows.push_back(std::move(r));
    }
    std::lock_guard<std::mutex> guard(mutex);
    view = std::move(next);
  }
  void beginStop() {
    if (stopping) return;
    stopping = true;
    deadline = Clock::now() + std::chrono::seconds(10);
    // Preserve each torrent's pause state; session shutdown stops the network.
    for (auto &[id, h] : handles) save(id, h);
  }
  void run() {
    try {
      lt::settings_pack settings;
      settings.set_str(lt::settings_pack::user_agent, "DomeTorrent/0.1 libtorrent/2");
      settings.set_str(lt::settings_pack::listen_interfaces, options.localOnly ? "127.0.0.1:0" : "0.0.0.0:6881,[::]:6881");
      settings.set_bool(lt::settings_pack::enable_dht, !options.localOnly);
      settings.set_bool(lt::settings_pack::enable_lsd, !options.localOnly);
      settings.set_bool(lt::settings_pack::enable_upnp, !options.localOnly);
      settings.set_bool(lt::settings_pack::enable_natpmp, !options.localOnly);
      settings.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, options.localOnly);
      settings.set_int(lt::settings_pack::alert_mask, lt::alert_category::error | lt::alert_category::storage | lt::alert_category::status);
      session = std::make_unique<lt::session>(settings);
      for (const auto &entry : fs::directory_iterator(options.state)) {
        if (entry.path().extension() != ".resume") continue;
        try {
          auto id = entry.path().stem().string();
          if ((id.size() != 40 && id.size() != 64) || id.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid resume filename");
          auto p = lt::read_resume_data(read(entry.path()));
          auto hash = p.ti ? p.ti->info_hashes() : p.info_hashes;
          if (id != key(hash) && (!hash.has_v1() || id != lt::aux::to_hex(hash.v1)))
            throw std::runtime_error("Resume hash does not match filename");
          add(std::move(p), true, id);
        }
        catch (const std::exception &e) { message("Could not restore " + entry.path().filename().string() + ": " + e.what()); }
      }
      for (;;) {
        std::deque<std::function<void()>> batch;
        { std::unique_lock<std::mutex> guard(mutex);
          wake.wait_for(guard, std::chrono::milliseconds(150), [this] { return !commands.empty(); });
          batch.swap(commands);
        }
        for (auto &f : batch) try { f(); } catch (const std::exception &e) { message(e.what()); }
        alerts();
        if (!stopping && Clock::now() - lastSave > std::chrono::seconds(30)) {
          for (auto &[id, h] : handles) save(id, h);
          lastSave = Clock::now();
        }
        bool done = stopping && (saving.empty() || Clock::now() >= deadline);
        if (done && !saving.empty()) message("Resume save timed out; previous checkpoint retained");
        publish(done);
        if (done) break;
      }
    } catch (const std::exception &e) {
      message(e.what());
      std::lock_guard<std::mutex> guard(mutex);
      view.messages = messages; view.stopped = true;
    }
    session.reset();
  }
};
Engine::Engine(Options options) : impl(std::make_unique<Impl>(std::move(options))) {}
Engine::~Engine() = default;
void Engine::add(std::string source, fs::path destination, bool paused) {
  impl->enqueue([s=impl.get(), source=std::move(source), destination=std::move(destination), paused] {
    if (s->stopping) return;
    lt::add_torrent_params p;
    if (source.rfind("magnet:", 0) == 0) p = lt::parse_magnet_uri(source);
    else { auto data = read(source); p.ti = std::make_shared<lt::torrent_info>(data.data(), int(data.size())); }
    if (destination.empty()) throw std::runtime_error("Choose a download folder");
    fs::create_directories(destination);
    p.save_path = fs::absolute(destination).string();
    if (paused) p.flags |= lt::torrent_flags::paused;
    else p.flags &= ~lt::torrent_flags::paused;
    s->add(std::move(p), false);
  });
}
void Engine::pause(std::string id, bool paused) {
  impl->enqueue([s=impl.get(), id=std::move(id), paused] {
    if (s->stopping) return;
    auto h = s->handle(id);
    h.unset_flags(lt::torrent_flags::auto_managed);
    if (paused) h.pause(); else { h.clear_error(); h.resume(); }
    s->save(id, h);
  });
}
void Engine::recheck(std::string id) {
  impl->enqueue([s=impl.get(), id=std::move(id)] { if (!s->stopping) s->handle(id).force_recheck(); });
}
void Engine::remove(std::string id) {
  impl->enqueue([s=impl.get(), id=std::move(id)] {
    if (s->stopping) return;
    auto h = s->handle(id);
    fs::remove(s->file(id));
    s->handles.erase(id); s->saving.erase(id); s->resave.erase(id);
    s->session->remove_torrent(h); // No delete_files flag.
    s->message("Removed transfer; downloaded files kept");
  });
}
void Engine::connectPeer(std::string id, unsigned short port) {
  impl->enqueue([s=impl.get(), id=std::move(id), port] { s->handle(id).connect_peer({lt::make_address("127.0.0.1"), port}); });
}
void Engine::stop() { impl->enqueue([s=impl.get()] { s->beginStop(); }); }
Snapshot Engine::snapshot() const { std::lock_guard<std::mutex> guard(impl->mutex); return impl->view; }
fs::path defaultState() {
  auto x = getenv("XDG_STATE_HOME"), h = getenv("HOME");
  return fs::path(x && *x ? x : (h ? std::string(h) + "/.local/state" : ".")) / "de-xr/dome-torrent";
}
fs::path defaultDownloads() {
  auto h = getenv("HOME");
  return fs::path(h ? h : ".") / "Downloads";
}
}
