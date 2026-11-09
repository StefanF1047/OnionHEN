#include "external_sprx_ui.hpp"

#include "plugin_sprx_pages.hpp"
#include "shellui_state.hpp"
#include "toolbox_i18n.hpp"
#include "settings_page_refresh.hpp"

#include <onion/ipc_client.hpp>

#include <algorithm>
#include <cstdlib>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace onion::shellui::external_sprx {
namespace {

/* The plugin whose config page is open; the game picker needs it because its
 * control ids only carry the Title ID. */
std::string g_active_sprx;

/* Last loader settings fetched from the daemon; the control-value resolver reads
 * these so the timeout / expose fields render stored values. */
int g_loader_timeout_s = 7;
bool g_loader_expose_data = true;
std::string g_loader_scan_paths;
bool g_loader_loaded = false;

using plugin_pages::kSprxDelayPrefix;
using plugin_pages::kSprxDeletePrefix;
using plugin_pages::kSprxEnabledPrefix;
using plugin_pages::split_control_id;

bool refresh_inventory() {
  std::vector<SprxInventoryItem> sprx;
  const bool loaded = IPC_Client::getInstance(false).ListSprx(sprx);
  if (loaded)
    g_ui.external_sprx = std::move(sprx);
  return loaded;
}

SprxInventoryItem *find_sprx(std::string_view id) {
  const auto it =
      std::ranges::find(g_ui.external_sprx, id, &SprxInventoryItem::id);
  return it == g_ui.external_sprx.end() ? nullptr : &*it;
}

} // namespace

bool append_inventory(ps5ui::Page &page) {
  int timeout_s = 7;
  bool expose_data = true;
  std::string scan_paths;
  if (IPC_Client::getInstance(false).GetSprxLoader(timeout_s, expose_data,
                                                   scan_paths)) {
    g_loader_timeout_s = timeout_s;
    g_loader_expose_data = expose_data;
    g_loader_scan_paths = scan_paths;
    g_loader_loaded = true;
    plugin_pages::fill_sprx_loader_settings(page, timeout_s, expose_data,
                                            scan_paths);
  }

  if (!refresh_inventory()) {
    page.label("id_external_sprx_unavailable",
               toolbox_i18n::tr("sprx.unavailable"));
    return false;
  }
  if (refresh_inventory()) {
    LOG_INFO("[sprx-ui] loader timeout=%d expose=%d paths=%s", timeout_s,
             expose_data ? 1 : 0, scan_paths.c_str());
    for (const SprxInventoryItem &entry : g_ui.external_sprx)
      LOG_INFO("[sprx-ui] %s enabled=%d delay=%d main=%d title=%s",
               entry.id.c_str(), entry.enabled ? 1 : 0, entry.delay_ms,
               entry.main_thread ? 1 : 0, entry.title_id.c_str());
  }

  if (g_ui.external_sprx.empty()) {
    page.label("id_external_sprx_empty", toolbox_i18n::tr("sprx.empty"));
  } else {
    plugin_pages::append_sprx_list_links(page, g_ui.external_sprx);
  }
  page.button(std::string(plugin_pages::kSprxRescanId),
              toolbox_i18n::tr("sprx.rescan"), std::nullopt,
              toolbox_i18n::tr("sprx.rescan.sub"));
  return true;
}

static bool config_model(ps5ui::Node &model, std::string_view id) {
  const bool loaded = refresh_inventory();
  const SprxInventoryItem *found = loaded ? find_sprx(id) : nullptr;
  g_active_sprx = std::string(id);

  ps5ui::Page page("id_sprx_config",
                   found ? plugin_pages::sprx_display_name(*found)
                         : std::string(id));
  if (!loaded) {
    page.label("id_external_sprx_unavailable",
               toolbox_i18n::tr("sprx.unavailable"));
  } else if (!found) {
    page.label("id_external_sprx_missing", toolbox_i18n::tr("sprx.missing"));
  } else {
    plugin_pages::fill_sprx_config(page, *found);
  }
  model = page.root();
  return loaded;
}

void generate_config_xml(std::string &xml_buffer, std::string_view id) {
  ps5ui::Node model;
  config_model(model, id);
  xml_buffer = settings::publish(model, [key = std::string(id)](ps5ui::Node &next) {
    return config_model(next, key);
  });
}

DispatchResult dispatch(std::string_view control_id, std::string_view value) {
  DispatchResult result;
  IPC_Client &client = IPC_Client::getInstance(false);
  std::string game_title;
  {
    std::string index_text;
    if (split_control_id(control_id, plugin_pages::kSprxRemoveDirPrefix,
                         index_text, 1, 3)) {
      int timeout_s = 7;
      bool expose_data = true;
      std::string scan_paths;
      (void)client.GetSprxLoader(timeout_s, expose_data, scan_paths);

      std::vector<std::string> paths;
      std::string current;
      for (const char c : scan_paths) {
        if (c == ';' || c == ',') {
          if (!current.empty())
            paths.push_back(current);
          current.clear();
        } else if (c != ' ' && c != '\t') {
          current.push_back(c);
        }
      }
      if (!current.empty())
        paths.push_back(current);

      const size_t index =
          static_cast<size_t>(std::strtoul(index_text.c_str(), nullptr, 10));
      result.owned = true;
      result.action = Action::SettingsChanged;
      if (index >= paths.size()) {
        result.success = false;
        return result;
      }
      paths.erase(paths.begin() + static_cast<long>(index));
      std::string joined;
      for (const std::string &path : paths) {
        if (!joined.empty())
          joined.push_back(';');
        joined += path;
      }
      result.success = client.SetSprxLoader(timeout_s, expose_data, joined);
      return result;
    }
  }
  if (control_id == plugin_pages::kSprxTimeoutId ||
      control_id == plugin_pages::kSprxExposeId ||
      control_id == plugin_pages::kSprxPathsId) {
    int timeout_s = 7;
    bool expose_data = true;
    std::string scan_paths;
    (void)client.GetSprxLoader(timeout_s, expose_data, scan_paths);
    if (control_id == plugin_pages::kSprxTimeoutId) {
      const std::string text(value);
      char *end = nullptr;
      const long parsed = std::strtol(text.c_str(), &end, 10);
      timeout_s = end == text.c_str() ? 7 : static_cast<int>(parsed);
    } else if (control_id == plugin_pages::kSprxExposeId) {
      expose_data = value == "1" || value == "true";
    } else {
      /* Additive: typing a folder appends it, so the defaults survive. */
      const std::string added(value);
      if (!added.empty() && scan_paths.find(added) == std::string::npos) {
        if (!scan_paths.empty())
          scan_paths.push_back(';');
        scan_paths += added;
      }
    }
    result.owned = true;
    result.action = Action::SettingsChanged;
    result.success = client.SetSprxLoader(timeout_s, expose_data, scan_paths);
    return result;
  }
  if (control_id == plugin_pages::kSprxRescanId) {
    result.owned = true;
    result.action = Action::Rescanned;
    result.success = client.RescanSprx();
    if (result.success)
      (void)refresh_inventory();
    return result;
  }
  if (split_control_id(control_id, kSprxEnabledPrefix, result.id, 1, 31)) {
    result.owned = true;
    result.action = Action::EnabledChanged;
    const bool enabled = value == "1" || value == "true";
    result.success = client.SetSprxEnabled(result.id, enabled);
    if (result.success)
      if (SprxInventoryItem *entry = find_sprx(result.id))
        entry->enabled = enabled;
  } else if (split_control_id(control_id, plugin_pages::kSprxGameListPrefix,
                              result.id, 1, 31)) {
    /* A Legacy Settings list reports its own id with the chosen item as the
       value, so the game picker must be handled here, not on the item id. */
    result.owned = true;
    result.action = Action::SettingsChanged;
    const std::string chosen(value);
    if (chosen.empty()) {
      result.success = false;
    } else {
      result.success = client.SetSprxTitle(result.id, chosen);
      if (result.success)
        if (SprxInventoryItem *entry = find_sprx(result.id))
          entry->title_id = chosen;
    }
  } else if (split_control_id(control_id, plugin_pages::kSprxGamePrefix,
                              game_title, 9, 9)) {
    result.owned = true;
    result.action = Action::SettingsChanged;
    if (g_active_sprx.empty()) {
      result.success = false;
    } else {
      result.id = g_active_sprx;
      result.success = client.SetSprxTitle(g_active_sprx, game_title);
      if (result.success)
        if (SprxInventoryItem *entry = find_sprx(g_active_sprx))
          entry->title_id = game_title;
    }
  } else if (split_control_id(control_id, plugin_pages::kSprxMainThreadPrefix,
                              result.id, 1, 31)) {
    result.owned = true;
    result.action = Action::MainThreadChanged;
    const bool main_thread = value == "1" || value == "true";
    result.success = client.SetSprxMainThread(result.id, main_thread);
    if (result.success)
      if (SprxInventoryItem *entry = find_sprx(result.id))
        entry->main_thread = main_thread;
  } else if (split_control_id(control_id, kSprxDelayPrefix, result.id, 1, 31)) {
    result.owned = true;
    result.action = Action::DelayChanged;
    const std::string text(value);
    char *end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    long delay_ms = end == text.c_str() ? 0 : parsed;
    if (delay_ms < 0)
      delay_ms = 0;
    if (delay_ms > 60000)
      delay_ms = 60000;
    result.success = client.SetSprxDelay(result.id, static_cast<int>(delay_ms));
    if (result.success)
      if (SprxInventoryItem *entry = find_sprx(result.id))
        entry->delay_ms = static_cast<int>(delay_ms);
  } else if (split_control_id(control_id, kSprxDeletePrefix, result.id, 1,
                              31)) {
    result.owned = true;
    result.action = Action::Deleted;
    result.success = client.DeleteSprx(result.id);
    if (result.success)
      std::erase_if(g_ui.external_sprx, [&](const SprxInventoryItem &entry) {
        return entry.id == result.id;
      });
  }
  return result;
}

bool cached_loader_settings(int &timeout_s, bool &expose_data,
                            std::string &scan_paths) {
  timeout_s = g_loader_timeout_s;
  expose_data = g_loader_expose_data;
  scan_paths = g_loader_scan_paths;
  return g_loader_loaded;
}

} // namespace onion::shellui::external_sprx
