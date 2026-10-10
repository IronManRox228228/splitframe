#pragma once

// Proxies: lighter stand-ins for big video files, so preview scrubs and plays smoothly. The reference is
// the Electron app's makeProxy (apps/desktop/src/main/ffmpeg.ts: 540p H.264, software when no encoder
// is at hand); the native one differs where the compositor profits:
//   - H.264 (libx264) ALL-INTRA (keyint 1, CRF 23, veryfast), 960 px wide, yuv420p: every frame is a
//     keyframe, so a seek or a backward step decodes exactly one picture instead of a whole GOP. That is
//     what makes scrubbing fast; the files are a few times larger than inter-coded ones, which is the trade.
//   - frame timestamps are copied, not regenerated: frame N of the proxy is frame N of the source (the
//     decoder's index <-> time mapping and the container start offset stay valid), variable frame rate
//     included. Rotation (display matrix) and the colour tags are carried over.
//   - no audio: preview sound comes from the original (the audio engine reads `path`, never the proxy).
//
// Cache: <project folder>/<project file name>.proxies/ next to a saved project, otherwise the user's
// cache location (QStandardPaths::CacheLocation/proxies). A proxy file is named after a hash of the source
// path + size + modification time, so editing or replacing the source simply yields a new name (the old
// file is deleted by removeStaleProxies). Nothing is ever written under %APPDATA%\Cutboard.
//
// Export never reads proxies: they only reach preview through render::AssetRef::proxyPath.

#include <QString>

#include <atomic>
#include <functional>

namespace sf::xport {

struct ProxyOptions {
  int width = 960;  // proxy width in pixels (coded orientation); sources not wider than this need none
  int crf = 23;
  QString preset = QStringLiteral("veryfast");
};

// "<pathHash>_<keyHash>.mp4": pathHash groups every version of one source, keyHash changes with size/mtime.
QString proxyFileName(const QString& sourcePath);
// Folder for a project's proxies: "<dir>/<name>.proxies" for a saved project file, else the cache location.
QString proxyCacheDir(const QString& projectFilePath);
// Where the proxy of `sourcePath` lives (whether or not it exists yet).
QString proxyPathFor(const QString& sourcePath, const QString& cacheDir);
// The proxy file if it exists and is complete, else an empty string.
QString existingProxy(const QString& sourcePath, const QString& cacheDir);
// Deletes proxies of the same source made for an older size/mtime. Returns how many.
int removeStaleProxies(const QString& sourcePath, const QString& cacheDir);

struct ProxyResult {
  bool ok = false;
  bool cancelled = false;
  bool unneeded = false; // the source is already small enough
  QString error;
  QString path;
  double seconds = 0;
};

// Transcodes on the calling thread (software decode, software encode). `progress` gets 0..1. Writes
// "<proxy>.part" and renames when done; a cancelled or failed run leaves nothing behind.
ProxyResult makeProxy(const QString& sourcePath, const QString& cacheDir, const ProxyOptions& options = {},
                      const std::function<void(double)>& progress = {}, const std::atomic<bool>* cancel = nullptr);

} // namespace sf::xport
