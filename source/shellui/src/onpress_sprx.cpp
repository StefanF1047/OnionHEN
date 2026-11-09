#include "onpress.hpp"

#include "external_sprx_ui.hpp"
#include "settings_page_refresh.hpp"
#include "toolbox_i18n.hpp"

namespace {

OnPressResult external_sprx_control(OnPressContext &ctx) {
  /* Commit after every SPRX action. settings_commit re-renders the page, which
     is what makes the controls read back the stored values and makes a delete /
     rescan / folder change visible without leaving and re-entering the menu. */
  ctx.dirty = true;
  const onion::shellui::external_sprx::DispatchResult result =
      onion::shellui::external_sprx::dispatch(ctx.id, ctx.value);
  if (!result.owned) return OnPressResult::NotMine;

  using onion::shellui::external_sprx::Action;
  if (!result.success) {
    const std::string message =
        toolbox_i18n::format("sprx.operation_failed_fmt", result.id.c_str());
    notify("%s", message.c_str());
    return OnPressResult::Consumed;
  }

  /* Every one of these changes stored state, so re-render the page we are on.
     Without this the ShellUI keeps showing the XML it rendered on entry. */
  auto refresh_page = [&ctx]() {
    if (ctx.instance != nullptr)
      onion::shellui::settings::refresh(ctx.instance);
  };

  /* Every action now reports its own outcome; only Deleted removes an entry. */
  switch (result.action) {
  case Action::EnabledChanged:
    refresh_page();
    return OnPressResult::Consumed;
  case Action::DelayChanged: {
    refresh_page();
    const std::string message =
        toolbox_i18n::format("sprx.delay_saved_fmt", result.id.c_str());
    notify("%s", message.c_str());
    return OnPressResult::Consumed;
  }
  case Action::MainThreadChanged:
  case Action::SettingsChanged:
    refresh_page();
    return OnPressResult::Consumed;
  case Action::Rescanned:
    refresh_page();
    notify("%s", toolbox_i18n::tr("sprx.rescan_done"));
    return OnPressResult::Consumed;
  case Action::Deleted: {
    refresh_page();
    const std::string message =
        toolbox_i18n::format("sprx.deleted_fmt", result.id.c_str());
    notify("%s", message.c_str());
    return OnPressResult::Consumed;
  }
  case Action::None:
  default:
    return OnPressResult::Consumed;
  }
}

const OnPressPrefixEntry kSprxPrefix[] = {
    {"id_external_sprx_", external_sprx_control},
};

} // namespace

const OnPressPrefixEntry *onpress_sprx_prefix(size_t *count) {
  *count = sizeof(kSprxPrefix) / sizeof(kSprxPrefix[0]);
  return kSprxPrefix;
}
