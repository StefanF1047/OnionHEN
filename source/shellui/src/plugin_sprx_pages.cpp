#include "plugin_sprx_pages.hpp"

#include "toolbox_i18n.hpp"
#include "toolbox_route.hpp"

#include <onion/ipc_client.hpp>

#include <vector>

namespace onion::shellui::plugin_pages {

std::string plugin_list_status(bool running, bool auto_start) {
  if (running && auto_start)
    return toolbox_i18n::tr("plugins.external.status.running_autostart");
  if (running)
    return toolbox_i18n::tr("plugins.external.status.running");
  if (auto_start)
    return toolbox_i18n::tr("plugins.external.status.stopped_autostart");
  return toolbox_i18n::tr("plugins.external.status.stopped");
}

std::string sprx_list_status(const SprxInventoryItem &entry) {
  /* Only a positive state is shown; "matches / does not match the current
     game" was noise that read like an error. */
  return entry.loaded_for_current_target ? toolbox_i18n::tr("sprx.status.loaded")
                                         : std::string();
}

void append_plugin_list_links(
    ps5ui::Page &page, const std::vector<PluginInventoryItem> &plugins) {
  for (const PluginInventoryItem &plugin : plugins)
    page.link(std::string(kPluginItemPrefix) + plugin.plugin_id, plugin.name,
              toolbox::external_plugin_config_xml(plugin.plugin_id),
              plugin_list_status(plugin.running, plugin.auto_start));
}

void fill_plugin_config(ps5ui::Page &page, const PluginInventoryItem &plugin,
                        const dynamic_ui::PluginSettingsLink *settings) {
  page.toggle(std::string(kPluginRunPrefix) + plugin.plugin_id,
              toolbox_i18n::tr("plugins.external.run"), plugin.running,
              toolbox_i18n::tr("plugins.external.run.sub"))
      .toggle(std::string(kPluginAutoStartPrefix) + plugin.plugin_id,
              toolbox_i18n::tr("plugins.external.autostart"), plugin.auto_start,
              toolbox_i18n::tr("plugins.external.autostart.sub"));
  if (settings)
    page.link("id_external_plugin_settings_" + plugin.plugin_id, settings->title,
              settings->resource,
              settings->description.empty()
                  ? std::optional<std::string>{}
                  : std::optional<std::string>{settings->description});
  page.button(std::string(kPluginDeletePrefix) + plugin.plugin_id,
              toolbox_i18n::tr("plugins.external.delete"), std::nullopt,
              std::nullopt, std::nullopt, ps5ui::Style::None,
              toolbox_i18n::tr("plugins.external.delete.confirm"),
              toolbox_i18n::tr("account.activate.confirm_phrase"));
}

std::string sprx_display_name(const SprxInventoryItem &entry) {
  const size_t slash = entry.path.find_last_of('/');
  const std::string name =
      slash == std::string::npos ? entry.path : entry.path.substr(slash + 1);
  return name.empty() ? entry.id : name;
}

void append_sprx_list_links(ps5ui::Page &page,
                            const std::vector<SprxInventoryItem> &sprx) {
  for (const SprxInventoryItem &entry : sprx) {
    const std::string status = sprx_list_status(entry);
    page.link(std::string(kSprxItemPrefix) + entry.id, sprx_display_name(entry),
              toolbox::sprx_config_xml(entry.id),
              status.empty() ? std::optional<std::string>{}
                             : std::optional<std::string>{status});
  }
}

void fill_sprx_loader_settings(ps5ui::Page &page, int timeout_s,
                               bool expose_data,
                               const std::string &scan_paths) {
  page.group("id_sprx_loader", toolbox_i18n::tr("sprx.loader.group"),
             [timeout_s, expose_data, scan_paths](ps5ui::Group &g) {
               g.text_field(std::string(kSprxTimeoutId),
                            toolbox_i18n::tr("sprx.timeout"),
                            toolbox_i18n::tr("sprx.timeout.sub"), "number", "1",
                            "4", std::nullopt, std::nullopt, std::nullopt,
                            std::to_string(timeout_s))
                   .toggle(std::string(kSprxExposeId),
                           toolbox_i18n::tr("sprx.expose"), expose_data,
                           toolbox_i18n::tr("sprx.expose.sub"))
                   .text_field(std::string(kSprxPathsId),
                               toolbox_i18n::tr("sprx.add_folder"),
                               toolbox_i18n::tr("sprx.add_folder.sub"), "default",
                               "1", "255", std::nullopt, std::nullopt,
                               std::nullopt, std::string());
               /* One row per folder with a Remove action; adding a folder below
                * appends, so the defaults can never be lost. */
               size_t index = 0;
               std::string current;
               for (size_t i = 0; i <= scan_paths.size(); ++i) {
                 const bool end = i == scan_paths.size();
                 const char c = end ? ';' : scan_paths[i];
                 if (c != ';' && c != ',') {
                   if (c != ' ' && c != '\t')
                     current.push_back(c);
                   continue;
                 }
                 if (!current.empty()) {
                   g.button(std::string(kSprxRemoveDirPrefix) +
                                std::to_string(index++),
                            current, toolbox_i18n::tr("sprx.remove_folder"));
                   current.clear();
                 }
               }
             },
             toolbox_i18n::tr("sprx.loader.group.sub"));
}

void fill_sprx_config(ps5ui::Page &page, const SprxInventoryItem &entry) {
  std::vector<SprxGameItem> games;
  (void)IPC_Client::getInstance(false).ListSprxGames(games);

  const std::string status = sprx_list_status(entry);
  if (!status.empty())
    page.label("id_external_sprx_status_" + entry.id, status);
  page.label("id_external_sprx_path_" + entry.id, entry.path);

  if (!games.empty()) {
    page.list(std::string(kSprxGameListPrefix) + entry.id,
              toolbox_i18n::tr("sprx.game"),
              [games](ps5ui::ListBuilder &list) {
                for (const SprxGameItem &game : games)
                  list.item(std::string(kSprxGamePrefix) + game.id, game.name,
                            game.id);
              },
              toolbox_i18n::tr("sprx.game.sub"), entry.title_id);
  }

  page.toggle(std::string(kSprxEnabledPrefix) + entry.id,
              toolbox_i18n::tr("sprx.enabled"), entry.enabled,
              toolbox_i18n::tr("sprx.enabled.sub"))
      .toggle(std::string(kSprxMainThreadPrefix) + entry.id,
              toolbox_i18n::tr("sprx.main_thread"), entry.main_thread,
              toolbox_i18n::tr("sprx.main_thread.sub"))
      .text_field(std::string(kSprxDelayPrefix) + entry.id,
                  toolbox_i18n::tr("sprx.delay"),
                  toolbox_i18n::tr("sprx.delay.sub"), "number", "1", "5",
                  std::nullopt, std::nullopt, std::nullopt,
                  std::to_string(entry.delay_ms))
      .button(std::string(kSprxDeletePrefix) + entry.id,
              toolbox_i18n::tr("sprx.delete"), std::nullopt,
              toolbox_i18n::tr("sprx.delete.sub"), std::nullopt,
              ps5ui::Style::None, toolbox_i18n::tr("sprx.delete.confirm"));
}

} // namespace onion::shellui::plugin_pages
