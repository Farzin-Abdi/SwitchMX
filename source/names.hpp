// File names the Switch can't store as they are. The console's file system creates only plain
// English (ASCII) names, and FAT drives refuse \ / : * ? " < > |. Such a name is kept on disk in a
// lossless ASCII form ("xn--...", Punycode as web addresses use for other alphabets) and shown
// as it really is everywhere in SwitchMX and its web page.
#pragma once
#include <string>

// The name to create on disk: the name itself when it can be stored, otherwise its encoded form.
// The extension stays readable ("xn--...-abc.mp3") so the file is still recognised by type.
std::string toDiskName(const std::string& name);
// The real name of a file on disk: decodes an encoded name, anything else is returned unchanged.
std::string fromDiskName(const std::string& name);
// Every component of a path decoded ("sdmc:/Music/xn--.../a.mp3" -> "sdmc:/Music/موسیقی/a.mp3").
std::string fromDiskPath(const std::string& path);
