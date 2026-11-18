#pragma once
#include <string>
#include <vector>

// Subtitle text encodings offered in settings (index = Settings::subEncoding).
const std::vector<std::string>& subtitleEncodingNames();

// Returns a path to a UTF-8 version of the subtitle file: the original when it already is
// UTF-8, otherwise a converted copy in cacheDir. The Switch build of mpv has no iconv, so
// legacy encodings (e.g. Persian/Arabic Windows-1256) must be converted here.
// langHint is the preferred subtitle language index from settings (0 = automatic).
std::string prepareSubtitle(const std::string& path, int encoding, int langHint, const std::string& cacheDir);

// Exposed for tests: converts raw bytes to UTF-8 using the same detection.
std::string subtitleToUtf8(const std::string& data, int encoding, int langHint);
