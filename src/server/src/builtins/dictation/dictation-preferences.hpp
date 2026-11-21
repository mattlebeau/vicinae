#pragma once
#include <algorithm>
#include <ranges>
#include <string>
#include <vector>
#include <QCoreApplication>
#include "services/ai/ai-model-selector-utils.hpp"
#include "builtins/dictation/dictation.hpp"
#include "command/preference-schema.hpp"
#include "service-registry.hpp"
#include "services/ai/ai-service.hpp"
#include "services/ai/speech-language-catalogue.hpp"
#include "services/media-control/media-control-service.hpp"
#include "services/paste/paste-service.hpp"

struct DictationPreferences {
  std::string model = std::string(Dictation::NO_MODEL);
  std::string language = std::string(Dictation::AUTO_LANGUAGE);
  bool sound = true;
  bool pauseMedia = true;
  Dictation::DictationAction dictationAction = Dictation::DictationAction::PasteToActiveWindow;
  std::string livePreview = std::string(Dictation::LIVE_PREVIEW_OFF);
  double hudScale = 1.0;
};

template <> struct PreferenceSchema<DictationPreferences> {
  PreferenceMeta model{
      .title = tr("Transcription model"),
      .description =
          tr("Model used to turn your voice into text. You can configure cloud options, or Vicinae "
             "can download and run local transcription models for you."),
      .required = false,
      .sections =
          [] {
            auto sections =
                buildModelDropdownSections(ServiceRegistry::instance()->ai(), AI::Capability::Transcription);
            sections.insert(sections.begin(),
                            Preference::DropdownData::Section{
                                .options = {Preference::DropdownData::Option{
                                    .title = tr("None"),
                                    .value = Dictation::qs(Dictation::NO_MODEL),
                                    .icon = ImageURL::builtin(BuiltinIcon::MicrophoneDisabled),
                                }}});
            return sections;
          },
  };
  PreferenceMeta language{
      .title = tr("Language"),
      .description =
          tr("Language you dictate in. Some models can auto-detect it, others need it set explicitly."),
      .required = false,
      .sections =
          [] {
            const auto option = [](const SpeechLanguage &lang) {
              return Preference::DropdownData::Option{.title = SpeechLanguageCatalogue::displayName(lang),
                                                      .value = Dictation::qs(lang.code)};
            };

            std::vector<Preference::DropdownData::Option> top;
            top.reserve(2);
            top.emplace_back(Preference::DropdownData::Option{
                .title = tr("Auto-detect"), .value = Dictation::qs(Dictation::AUTO_LANGUAGE)});
            if (const auto *lang = SpeechLanguageCatalogue::systemLanguage()) top.emplace_back(option(*lang));

            auto all = SpeechLanguageCatalogue::entries() | std::views::transform(option) |
                       std::ranges::to<std::vector>();
            std::ranges::sort(all, [](const auto &a, const auto &b) {
              return QString::localeAwareCompare(a.title, b.title) < 0;
            });

            return std::vector<Preference::DropdownData::Section>{
                {.options = std::move(top)},
                {.title = tr("All languages"), .options = std::move(all)},
            };
          },
  };
  PreferenceMeta sound{
      .title = tr("Sound Effects"),
      .description = tr("Whether to play a sound effect when starting or stopping dictation."),
  };
  PreferenceMeta pauseMedia{
      .title = tr("Pause media"),
      .description = tr("Pause media players when recording and resume them after. The audio output is "
                        "muted instead when players cannot be controlled."),
      .available =
          [] {
            auto *registry = ServiceRegistry::instance();
            return registry->mediaControl()->available() || registry->audioControl()->available();
          },
  };
  PreferenceMeta dictationAction{
      .title = tr("Dictation Action"),
      .description = tr("What to do after dictation"),
      .options =
          [] {
            using Action = Dictation::DictationAction;
            std::vector<Preference::DropdownData::Option> options;
            if (ServiceRegistry::instance()->pasteService()->supportsPaste()) {
              options.emplace_back(option(Action::PasteToActiveWindow, tr("Paste to active window")));
            }
            options.emplace_back(option(Action::CopyToClipboard, tr("Copy to clipboard")));
            return options;
          },
  };
  PreferenceMeta livePreview{
      .title = tr("Live transcription"),
      .description = tr("Show the text in the dictation HUD while you speak. The pasted text still comes "
                        "from the transcription model once you stop. Cloud models are billed for every "
                        "piece of audio sent for the preview."),
      .required = false,
      .sections =
          [] {
            auto sections =
                buildModelDropdownSections(ServiceRegistry::instance()->ai(), AI::Capability::Transcription);
            sections.insert(sections.begin(),
                            Preference::DropdownData::Section{.options = {
                                                                  Preference::DropdownData::Option{
                                                                      .title = tr("Off"),
                                                                      .value = Dictation::qs(Dictation::LIVE_PREVIEW_OFF),
                                                                  },
                                                                  Preference::DropdownData::Option{
                                                                      .title = tr("Same as transcription model"),
                                                                      .value = Dictation::qs(Dictation::LIVE_PREVIEW_SAME),
                                                                  },
                                                              }});
            return sections;
          },
  };
  PreferenceMeta hudScale{
      .title = tr("HUD size"),
      .description = tr("Scale of the dictation HUD, from 1 (default) to 4. Decimals such as 1.5 work."),
      .placeholder = "1.0",
      .required = false,
  };
  Q_DECLARE_TR_FUNCTIONS(DictationPreferences)
};
