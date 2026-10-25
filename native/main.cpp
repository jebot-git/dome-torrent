// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"
#include "desktop-menu.h"
#include "gpu-window.h"
#include "status-icon.h"
#include <FL/Fl_Hold_Browser.H>
#include <FL/Fl_Input.H>
#include <FL/Fl_Check_Button.H>
#include <FL/Fl_Native_File_Chooser.H>
#include <FL/Fl_Progress.H>
#include <FL/Fl_Text_Display.H>
#include <FL/fl_ask.H>
#include <gio/gio.h>
#include <csignal>
#include <iomanip>
#include <sstream>
#include <thread>
namespace fs = std::filesystem;
namespace {
volatile std::sig_atomic_t interrupted = 0;
std::string literal(std::string text) {
  std::string out;
  for (unsigned char c : text) {
    if (c == '@') out += '@';
    out += c < 32 ? ' ' : char(c);
  }
  return out;
}
std::string bytes(std::int64_t n) {
  const char *units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
  double value = double(n); int i = 0;
  while (value >= 1024 && i < 4) { value /= 1024; ++i; }
  std::ostringstream out;
  out << std::fixed << std::setprecision(i ? 1 : 0) << value << ' ' << units[i];
  return out.str();
}
std::string localPath(const std::string &source) {
  if (source.rfind("file:", 0) != 0) return source;
  GError *error = nullptr;
  char *name = g_filename_from_uri(source.c_str(), nullptr, &error);
  if (!name) {
    std::string message = error ? error->message : "Invalid file URL";
    if (error) g_error_free(error);
    throw std::runtime_error(message);
  }
  std::string result = name; g_free(name); return result;
}
class Client : public dx::GpuWindow {
  torrent::Engine &engine;
  DesktopMenuBar menu{0, 0, 1100, 0};
  Fl_Input source{155, 20, 785, 34, "Magnet or file:"};
  Fl_Button browse{955, 20, 125, 34, "Torrent..."};
  Fl_Input destination{155, 67, 635, 34, "Save to:"};
  Fl_Button folder{805, 67, 135, 34, "Folder..."};
  Fl_Button add{955, 67, 125, 34, "Add"};
  Fl_Check_Button startPaused{155, 112, 300, 30, "Add paused"};
  Fl_Box headings{20, 150, 1060, 27, "Name / progress / state / download / upload / peers"};
  Fl_Hold_Browser transfers{20, 180, 1060, 190};
  Fl_Button pause{20, 385, 120, 34, "Pause"};
  Fl_Button resume{150, 385, 120, 34, "Resume"};
  Fl_Button recheck{280, 385, 130, 34, "Recheck"};
  Fl_Button copy{420, 385, 180, 34, "Copy magnet"};
  Fl_Button open{610, 385, 180, 34, "Open folder"};
  Fl_Button remove{800, 385, 180, 34, "Remove..."};
  Fl_Progress progress{20, 435, 1060, 26};
  Fl_Text_Display details{20, 475, 1060, 100};
  Fl_Text_Buffer buffer;
  Fl_Box status{20, 590, 1060, 45};
  std::unique_ptr<DesktopMenu> desktop;
  std::unique_ptr<StatusIcon> tray;
  torrent::Snapshot snapshot;
  std::string selectedId, feedback;
  fs::path settings;
  bool quitting = false;
  static void timer(void *p) {
    auto self = static_cast<Client *>(p);
    self->refresh();
    Fl::repeat_timeout(.3, timer, p);
  }
  void say(const std::string &text) { feedback = text; status.copy_label(literal(text).c_str()); }
  const torrent::Row *selected() const {
    for (const auto &row : snapshot.rows) if (row.id == selectedId) return &row;
    return nullptr;
  }
  void select() {
    int row = transfers.value() - 1;
    selectedId = row >= 0 && size_t(row) < snapshot.rows.size() ? snapshot.rows[size_t(row)].id : "";
    showDetails();
  }
  void showDetails() {
    auto row = selected();
    for (auto button : {&pause, &resume, &recheck, &copy, &open, &remove}) {
      if (row && !quitting) button->activate(); else button->deactivate();
    }
    if (!row) { buffer.text("Add a magnet link or a .torrent file to begin.\nSelect a transfer to inspect its files and info hash."); progress.value(0); progress.copy_label(""); return; }
    if (row->paused) pause.deactivate(); else resume.deactivate();
    if (!row->metadata) recheck.deactivate();
    std::ostringstream out;
    out << row->name << "\nInfo hash: " << row->id << "\nFolder: " << row->path
        << "\n" << bytes(row->done) << " / " << (row->metadata ? bytes(row->total) : "waiting for metadata")
        << "   Uploaded: " << bytes(row->uploaded) << "   Seeds: " << row->seeds << "\n";
    if (!row->error.empty()) out << "Error: " << row->error << "\n";
    if (!row->metadata) out << (row->paused ? "Resume to find peers and download metadata.\n" : "Waiting for metadata from a peer.\n");
    else { out << "Files:\n"; for (const auto &file : row->files) out << "  " << file << "\n"; }
    buffer.text(out.str().c_str());
    progress.value(float(row->progress));
    auto label = std::to_string(int(row->progress * 100)) + "% — " + row->state;
    progress.copy_label(label.c_str());
  }
  void rememberFolder() {
    try {
      fs::create_directories(settings.parent_path());
      auto temp = settings.string() + ".tmp";
      std::ofstream out(temp); out << Json{{"download_directory", destination.value()}}.dump(2) << '\n';
      out.close();
      if (!out) throw std::runtime_error("Could not save download folder");
      fs::rename(temp, settings);
    } catch (const std::exception &e) { say(e.what()); }
  }
  void addSource() {
    if (quitting) return;
    std::string text = source.value();
    auto first = text.find_first_not_of(" \r\n\t"), last = text.find_last_not_of(" \r\n\t");
    if (first == std::string::npos) { say("Paste a magnet link or select a torrent file."); return; }
    try {
      engine.add(localPath(text.substr(first, last - first + 1)), destination.value(), startPaused.value());
      rememberFolder();
      source.value(""); say("Adding transfer...");
    } catch (const std::exception &e) { say(e.what()); }
  }
  void chooseTorrent() {
    Fl_Native_File_Chooser dialog;
    dialog.title("Open torrent"); dialog.type(Fl_Native_File_Chooser::BROWSE_FILE);
    dialog.filter("BitTorrent files\t*.torrent");
    if (!dialog.show()) { source.value(dialog.filename()); source.take_focus(); }
  }
  void chooseFolder() {
    Fl_Native_File_Chooser dialog;
    dialog.title("Download folder"); dialog.type(Fl_Native_File_Chooser::BROWSE_DIRECTORY);
    dialog.directory(destination.value());
    if (!dialog.show()) { destination.value(dialog.filename()); rememberFolder(); }
  }
  void pauseSelected(bool paused) {
    if (selected()) engine.pause(selectedId, paused);
  }
  void recheckSelected() { if (selected()) engine.recheck(selectedId); }
  void copyMagnet() { if (auto row = selected()) { Fl::copy(row->magnet.c_str(), int(row->magnet.size()), 1); say("Magnet link copied."); } }
  void openFolder() { if (auto row = selected()) if (dx::spawn({"xdg-open", row->path}) < 0) say("Could not open download folder."); }
  void removeSelected() {
    if (selected() && fl_choice("Remove this transfer? Downloaded files will be kept.", "Cancel", "Remove", nullptr) == 1) engine.remove(selectedId);
  }
  void closeWindow() { if (tray->available()) hide(); else quit(); }
public:
  bool exitRequested = false;
  int exitCode = 0;
  Client(torrent::Engine &value) : dx::GpuWindow(1100, 650, "Dome Torrent"), engine(value) {
    auto config = getenv("XDG_CONFIG_HOME"), home = getenv("HOME");
    settings = fs::path(config && *config ? config : (home ? std::string(home) + "/.config" : ".")) / "de-xr/dome-torrent/settings.json";
    auto download = torrent::defaultDownloads().string();
    try { std::ifstream input(settings); if (input) { Json j; input >> j; download = j.value("download_directory", download); } } catch (...) {}
    destination.value(download.c_str());
    source.when(FL_WHEN_ENTER_KEY_ALWAYS);
    source.callback([](Fl_Widget *, void *p) { static_cast<Client *>(p)->addSource(); }, this);
    transfers.format_char(0);
    transfers.callback([](Fl_Widget *, void *p) { static_cast<Client *>(p)->select(); }, this);
    headings.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE);
    status.align(FL_ALIGN_LEFT | FL_ALIGN_INSIDE | FL_ALIGN_WRAP);
    details.buffer(&buffer); details.wrap_mode(Fl_Text_Display::WRAP_AT_BOUNDS, 0);
    progress.minimum(0); progress.maximum(1);
#define BUTTON(widget, action) widget.callback([](Fl_Widget *, void *p) { static_cast<Client *>(p)->action; }, this)
    BUTTON(browse, chooseTorrent()); BUTTON(folder, chooseFolder()); BUTTON(add, addSource());
    BUTTON(pause, pauseSelected(true)); BUTTON(resume, pauseSelected(false)); BUTTON(recheck, recheckSelected());
    BUTTON(copy, copyMagnet()); BUTTON(open, openFolder()); BUTTON(remove, removeSelected());
#undef BUTTON
#define MENU(path, key, action) menu.add(path, key, [](Fl_Widget *, void *p) { static_cast<Client *>(p)->action; }, this)
    MENU("File/Show transfers", 0, show());
    MENU("File/Add magnet", FL_CTRL | 'l', focusSource());
    MENU("File/Open torrent...", FL_CTRL | 'o', chooseTorrent());
    MENU("File/Close to tray", FL_CTRL | 'w', closeWindow());
    MENU("File/Quit", FL_CTRL | 'q', quit());
    MENU("Transfer/Pause", 0, pauseSelected(true));
    MENU("Transfer/Resume", 0, pauseSelected(false));
    MENU("Transfer/Recheck", 0, recheckSelected());
    MENU("Transfer/Copy magnet", 0, copyMagnet());
    MENU("Transfer/Remove...", FL_Delete, removeSelected());
#undef MENU
    end(); resizable(transfers); size_range(1100, 650);
    callback([](Fl_Widget *, void *p) { static_cast<Client *>(p)->closeWindow(); }, this);
    desktop = std::make_unique<DesktopMenu>(*this, menu);
    tray = std::make_unique<StatusIcon>(*desktop); tray->setTitle("Dome Torrent"); tray->activate = [this] { show(); };
    dx::style(this); showDetails(); say("Ready — magnet links and .torrent files supported.");
    Fl::add_timeout(.1, timer, this);
  }
  ~Client() { Fl::remove_timeout(timer, this); details.buffer(nullptr); }
  void focusSource() { show(); source.take_focus(); }
  void quit() {
    if (quitting) return;
    quitting = true; say("Saving transfer state...");
    for (auto widget : {static_cast<Fl_Widget *>(&add), static_cast<Fl_Widget *>(&browse), static_cast<Fl_Widget *>(&source)}) widget->deactivate();
    engine.stop();
  }
  bool trayAvailable() const { return tray->available(); }
  void accept(const std::vector<std::string> &sources) {
    for (const auto &item : sources) {
      try { engine.add(localPath(item), destination.value(), startPaused.value()); }
      catch (const std::exception &e) { say(e.what()); }
    }
    if (!sources.empty()) show();
  }
  void refresh() {
    auto next = engine.snapshot();
    bool messageChanged = next.messages != snapshot.messages;
    snapshot = std::move(next);
    int scroll = transfers.vposition(); transfers.clear();
    for (size_t i = 0; i < snapshot.rows.size(); ++i) {
      const auto &r = snapshot.rows[i];
      auto text = r.name + "  |  " + (r.metadata ? std::to_string(int(r.progress * 100)) + "%" : "metadata") + "  |  " + r.state + "  |  ↓ " + bytes(r.downRate) + "/s  ↑ " + bytes(r.upRate) + "/s  |  " + std::to_string(r.peers) + " peers";
      transfers.add(text.c_str());
      if (r.id == selectedId) transfers.select(int(i + 1));
    }
    transfers.vposition(scroll); showDetails();
    if (messageChanged && !snapshot.messages.empty()) say(snapshot.messages.back());
    if (snapshot.stopped) { if (!quitting) { exitCode = 1; fprintf(stderr, "Dome Torrent engine stopped: %s\n", snapshot.messages.empty() ? "unknown error" : snapshot.messages.back().c_str()); } exitRequested = true; hide(); }
    if (interrupted) quit();
  }
};
std::vector<std::string> sources(int argc, char **argv, bool &tray, bool &localOnly) {
  std::vector<std::string> result;
  bool operands = false;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (!operands && arg == "--") { operands = true; continue; }
    if (!operands && arg == "--tray") tray = true;
    else if (!operands && arg == "--local-only") localOnly = true;
    else if (!operands && !arg.empty() && arg.front() == '-') throw std::runtime_error("Unknown option: " + arg);
    else result.push_back(arg);
  }
  return result;
}
}
int main(int argc, char **argv) try {
  for (int i = 1; i < argc; ++i) if (std::string(argv[i]) == "--help") {
    puts("Dome Torrent [--tray] [--local-only] [--] [magnet-link | file.torrent ...]\n"
         "FLTK BitTorrent client using libtorrent 2. Magnet metadata uses DHT, trackers and peers.\n"
         "--local-only disables discovery and port mapping; for isolated testing.\n"
         "Closing keeps transfers in the tray when available; Ctrl+Q saves and quits."); return 0;
  }
  bool trayOnly = false, localOnly = false;
  auto initial = sources(argc, argv, trayOnly, localOnly);
  auto application = g_application_new("org.domed.Torrent", G_APPLICATION_HANDLES_COMMAND_LINE);
  GError *error = nullptr;
  if (!g_application_register(application, nullptr, &error)) {
    std::string message = error ? error->message : "Cannot register application";
    if (error) g_error_free(error);
    throw std::runtime_error(message);
  }
  if (g_application_get_is_remote(application)) {
    int result = g_application_run(application, argc, argv); g_object_unref(application); return result;
  }
  Fl::lock(); Fl_Window::default_xclass("org.domed.Torrent"); dx::init();
  torrent::Engine engine({torrent::defaultState(), localOnly});
  Client window(engine);
  g_signal_connect(application, "command-line", G_CALLBACK((+[](GApplication *, GApplicationCommandLine *line, gpointer p) -> int {
    int argc = 0; char **argv = g_application_command_line_get_arguments(line, &argc);
    try {
      bool tray = false, local = false;
      auto input = sources(argc, argv, tray, local);
      const char *cwd = g_application_command_line_get_cwd(line);
      for (auto &item : input) if (item.rfind("magnet:", 0) != 0 && item.rfind("file:", 0) != 0 && fs::path(item).is_relative()) item = (fs::path(cwd ? cwd : ".") / item).string();
      auto self = static_cast<Client *>(p); self->accept(input); if (input.empty()) self->show();
    } catch (const std::exception &e) { g_application_command_line_printerr(line, "%s\n", e.what()); g_strfreev(argv); return 1; }
    g_strfreev(argv); return 0;
  })), &window);
  std::signal(SIGINT, [](int) { interrupted = 1; });
  std::signal(SIGTERM, [](int) { interrupted = 1; });
  window.accept(initial);
  if (!trayOnly) window.show();
  auto fallback = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!window.exitRequested) {
    Fl::wait(.05);
    while (g_main_context_iteration(nullptr, FALSE)) {}
    if (trayOnly && (window.trayAvailable() || std::chrono::steady_clock::now() >= fallback)) {
      if (!window.trayAvailable()) window.show();
      trayOnly = false;
    }
  }
  g_object_unref(application);
  return window.exitCode;
} catch (const std::exception &e) { fprintf(stderr, "Dome Torrent: %s\n", e.what()); return 1; }
