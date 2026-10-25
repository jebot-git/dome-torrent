// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"
#include <libtorrent/session.hpp>
#include <libtorrent/create_torrent.hpp>
#include <libtorrent/torrent_info.hpp>
#include <libtorrent/torrent_status.hpp>
#include <libtorrent/magnet_uri.hpp>
#include <libtorrent/bencode.hpp>
#include <libtorrent/hex.hpp>
#include <cassert>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <thread>
#include <unistd.h>
namespace lt = libtorrent;
namespace fs = std::filesystem;
using namespace std::chrono_literals;
void waitFor(const std::function<bool()> &condition, const std::string &message, int seconds = 20) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
  do { if (condition()) return; std::this_thread::sleep_for(50ms); } while (std::chrono::steady_clock::now() < deadline);
  throw std::runtime_error("Timeout: " + message);
}
lt::settings_pack localSettings() {
  lt::settings_pack s;
  s.set_str(lt::settings_pack::listen_interfaces, "127.0.0.1:0");
  s.set_bool(lt::settings_pack::enable_dht, false);
  s.set_bool(lt::settings_pack::enable_lsd, false);
  s.set_bool(lt::settings_pack::enable_upnp, false);
  s.set_bool(lt::settings_pack::enable_natpmp, false);
  s.set_bool(lt::settings_pack::allow_multiple_connections_per_ip, true);
  s.set_bool(lt::settings_pack::enable_outgoing_utp, false);
  return s;
}
std::string contents(const fs::path &path) {
  std::ifstream f(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
void stop(torrent::Engine &engine) {
  engine.stop(); waitFor([&] { return engine.snapshot().stopped; }, "save and stop", 15);
}
void transfer(const fs::path &root, int variant) {
  bool v2 = variant == 1;
  bool hybrid = variant == 2;
  auto base = root / (hybrid ? "hybrid" : v2 ? "v2" : "v1");
  fs::create_directories(base / "seed");
  std::string name = v2 ? "metadata-v2.bin" : "příloha-v1.bin";
  std::string payload;
  for (int i = 0; i < 256 * 1024; ++i) payload += char((i * 17 + i / 133) % 251);
  { std::ofstream f(base / "seed" / name, std::ios::binary); f << payload; }
  lt::file_storage files; files.add_file(name, payload.size());
  lt::create_torrent create(files, 16 * 1024, hybrid ? lt::create_flags_t{} : v2 ? lt::create_torrent::v2_only : lt::create_torrent::v1_only);
  lt::set_piece_hashes(create, (base / "seed").string());
  auto data = create.generate_buf();
  { std::ofstream out(base / "fixture.torrent", std::ios::binary); out.write(data.data(), data.size()); }
  auto info = std::make_shared<lt::torrent_info>(data.data(), int(data.size()));
  lt::session seed(localSettings());
  lt::add_torrent_params p; p.ti = info; p.save_path = (base / "seed").string();
  p.flags &= ~(lt::torrent_flags::paused | lt::torrent_flags::auto_managed);
  p.flags |= lt::torrent_flags::seed_mode;
  auto seeder = seed.add_torrent(p);
  waitFor([&] { return seed.listen_port() != 0; }, "seeder listen");
  auto magnet = lt::make_magnet_uri(*info);
  // A v1-only link can discover hybrid metadata with an additional v2 hash.
  // Its checkpoint filename must stay stable across that discovery and restart.
  if (hybrid) magnet = "magnet:?xt=urn:btih:" + lt::aux::to_hex(info->info_hashes().v1);
  assert(magnet.find(v2 ? "urn:btmh:" : "urn:btih:") != std::string::npos);
  std::string id;
  {
    torrent::Engine client({base / "state", true});
    bool locked = false;
    try { torrent::Engine second({base / "state", true}); } catch (...) { locked = true; }
    assert(locked);
    client.add("magnet:?xt=urn:btih:invalid", base / "get");
    waitFor([&] { return !client.snapshot().messages.empty(); }, "invalid magnet error");
    assert(client.snapshot().rows.empty());
    client.add(magnet, base / "get");
    waitFor([&] { return client.snapshot().rows.size() == 1; }, "magnet added");
    auto row = client.snapshot().rows.front(); id = row.id;
    assert(!row.metadata); // No torrent file or public discovery in downloader.
    assert(fs::exists(base / "state" / (id + ".resume")));
    client.add(magnet, base / "get");
    waitFor([&] { auto s = client.snapshot(); return !s.messages.empty() && s.messages.back().find("Cannot add torrent") != std::string::npos; }, "duplicate rejected");
    assert(client.snapshot().rows.size() == 1);
    client.connectPeer(id, seed.listen_port());
    waitFor([&] { auto s = client.snapshot(); return !s.rows.empty() && s.rows[0].metadata && s.rows[0].progress == 1.0; }, "magnet metadata and payload transfer", 35);
    assert(contents(base / "get" / name) == payload);
    auto s = client.snapshot(); assert(!s.rows[0].files.empty());
    client.pause(id, true);
    waitFor([&] { return client.snapshot().rows[0].paused; }, "pause");
    client.pause(id, false);
    waitFor([&] { return !client.snapshot().rows[0].paused; }, "resume");
    client.recheck(id);
    waitFor([&] { auto r = client.snapshot().rows[0]; return r.progress == 1.0 && r.state == "Seeding"; }, "recheck");
    client.pause(id, true);
    waitFor([&] { return client.snapshot().rows[0].paused; }, "pause before restart");
    stop(client);
  }
  {
    torrent::Engine client({base / "state", true});
    waitFor([&] { return client.snapshot().rows.size() == 1; }, "restore transfer");
    auto row = client.snapshot().rows.front();
    assert(row.id == id && row.paused && row.metadata);
    assert(row.path == (base / "get").string());
    client.remove(id);
    waitFor([&] { return client.snapshot().rows.empty(); }, "remove transfer");
    stop(client);
    assert(contents(base / "get" / name) == payload);
    assert(!fs::exists(base / "state" / (id + ".resume")));
  }
  {
    torrent::Engine client({base / "state", true});
    // The file-input workflow uses the very same persisted engine.
    client.add((base / "fixture.torrent").string(), base / "get", true);
    waitFor([&] { return client.snapshot().rows.size() == 1; }, "torrent file input");
    assert(client.snapshot().rows[0].metadata);
    client.remove(client.snapshot().rows[0].id);
    waitFor([&] { return client.snapshot().rows.empty(); }, "remove torrent file transfer");
    stop(client);
  }
  std::cout << "PASS " << (hybrid ? "hybrid from v1 link" : v2 ? "v2" : "v1") << " magnet metadata, byte-exact payload, duplicate, controls, restart and retained files\n";
}
void pendingMagnet(const fs::path &root) {
  auto base = root / "pending";
  std::string id;
  {
    torrent::Engine client({base / "state", true});
    client.add("magnet:?xt=urn:btih:0123456789012345678901234567890123456789&dn=Pending%20metadata", base / "get", true);
    waitFor([&] { return client.snapshot().rows.size() == 1; }, "pending magnet add");
    id = client.snapshot().rows[0].id; stop(client);
  }
  { std::ofstream f(base / "state" / "bad.resume"); f << "broken"; }
  {
    torrent::Engine client({base / "state", true});
    waitFor([&] { return client.snapshot().rows.size() == 1; }, "pending magnet restore");
    auto row = client.snapshot().rows[0];
    assert(row.id == id && !row.metadata && row.paused && row.magnet.find(id) != std::string::npos);
    assert(!client.snapshot().messages.empty());
    stop(client);
  }
  std::cout << "PASS unresolved magnet restart and corrupt checkpoint isolation\n";
}
int main() try {
  char pattern[] = "/tmp/dome-torrent-test-XXXXXX";
  char *temp = mkdtemp(pattern); if (!temp) throw std::runtime_error("mkdtemp");
  fs::path root = temp;
  try { transfer(root, 0); transfer(root, 1); transfer(root, 2); pendingMagnet(root); }
  catch (...) { std::cerr << "Fixture retained at " << root << '\n'; throw; }
  fs::remove_all(root);
  return 0;
} catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
