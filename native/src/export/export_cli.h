#pragma once

// Headless export: `splitframe.exe --export <project.json> --out <file> [options]`. What the agent / MCP
// layer will call. Progress goes to stdout as one line per update, key=value pairs:
//   progress frame=12 total=90 fps=41.3 eta=1.9 stage=rendering
// and the last line is either
//   done path=... frames=90 audio_samples=96000 seconds=2.4 fps=37.5 encoder=libx264 gain_db=0.0
//   error: <message>            (also "cancelled")
// Exit codes: 0 done, 1 export failed, 2 bad arguments / unreadable project, 3 cancelled.
// Options: --out <file> (required) --preset <name> --quality draft|standard|high|master --codec h264|h265|prores|dnxhr
//   --container mp4|mov|mkv|wav --audio-codec aac|pcm16|pcm24|pcm32f|none --no-audio --audio-only --crf <n>
//   --encoder-preset <p> --bitrate <kbps> --width <px> --height <px> --range <inFrame>:<outFrame> --ten-bit
//   --prores-profile proxy|lt|standard|hq|4444 --dnx-profile dnxhr_lb|sq|hq|hqx|444 --lufs <target> --true-peak <dBTP>
//   --overwrite --hw (NVENC when available) --hw-decode --list-presets

#include "export/export_settings.h"
#include "render/media_provider.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace sf::xport {

// Reads a project bundle (or a bare timeline document) and resolves relative asset paths against the file.
// False with *error set when it can't be read.
bool loadProjectForExport(const QString& path, TimelineDoc* doc, render::AssetTable* assets, QString* error);

// The option set shared by --export and the engine's export.start: `o` uses the keys out, overwrite, preset, quality, codec,
// container, audioCodec, noAudio, audioOnly, crf, encoderPreset, bitrate, width, height, range ("in:out" or [in, out]), tenBit,
// proresProfile, dnxProfile, lufs, truePeak, hw, hwDecode. Numbers may be JSON numbers or text. False with *error set (the
// same messages --export prints) when an option is unknown or invalid.
bool exportSettingsFromJson(const QJsonObject& o, const TimelineDoc& doc, ExportSettings* settings, QString* error);

// Parses the command line (args[0] is the program) and runs. Needs a QGuiApplication to exist.
int runExportCli(const QStringList& args);

} // namespace sf::xport
