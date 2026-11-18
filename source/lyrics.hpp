// Song lyrics for the music player: timed (.lrc, "[01:23.45] line") or plain text.
#pragma once
#include <map>
#include <string>
#include <vector>

struct Lyrics {
    struct Line {
        double time = 0;  // seconds; unused for plain lyrics
        std::string text;
    };
    std::vector<Line> lines;
    bool synced = false;
    bool empty() const { return lines.empty(); }
    // The line being sung at `t` (-1 before the first one). Synced lyrics only.
    int lineAt(double t) const;
};

// Parses LRC or plain text (already UTF-8).
Lyrics parseLyrics(const std::string& text);

// Lyrics for a song: "<song>.lrc" beside it first, else lyrics stored in its tags. `langHint` is the
// subtitle language setting, used to read old non-UTF-8 .lrc files correctly.
Lyrics findLyrics(const std::string& songPath, const std::map<std::string, std::string>& tags, int langHint);
