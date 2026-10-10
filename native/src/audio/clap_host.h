#pragma once

// CLAP plugin host (https://github.com/free-audio/clap, MIT): finds .clap files, loads one, runs it as a
// mixer insert. Audio goes in as one stereo port (a mono plugin gets the mid signal and its output is
// duplicated; a second input port, if the plugin has one, is the sidechain). Plugin parameters are
// exposed under their display names, state is the plugin's own blob.
//
// In the mixer block an insert of type "clap" names its plugin with params "pluginPath" (the .clap file)
// and "pluginId"; `state` holds the saved state, base64.

#include "audio/effects.h"
#include "core/schema.h"

#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

namespace sf::audio {

struct ClapPluginInfo {
  QString path; // the .clap file
  QString id;
  QString name;
  QString vendor;
  QString version;
  QStringList features;
};

// %COMMONPROGRAMFILES%\CLAP, %LOCALAPPDATA%\Programs\Common\CLAP and every folder in CLAP_PATH.
QStringList defaultClapSearchPaths();
// Lists the plugins of every .clap file under `paths` (recursively; defaultClapSearchPaths() when empty).
// Loads each library briefly: run it off the UI thread.
std::vector<ClapPluginInfo> scanClapPlugins(const QStringList& paths = {});
// All plugins in one file
std::vector<ClapPluginInfo> readClapFile(const QString& path, QString* error = nullptr);

// Loads and activates `id` from `path`; null + *error on failure.
std::unique_ptr<Effect> loadClapPlugin(const QString& path, const QString& id, double sampleRate, QString* error = nullptr);
// The mixer's entry point: honours params pluginPath / pluginId and restores `state`.
std::unique_ptr<Effect> makeClapEffect(const MixInsert& insert, double sampleRate);

} // namespace sf::audio
