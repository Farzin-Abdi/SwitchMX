#pragma once
#include <map>
#include <condition_variable>
#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

enum class MediaKind { Folder, Video, Audio, Subtitle, Playlist, Image, Archive, Text, Document, Other };

struct FileEntry {
    std::string name;
    std::string path;
    MediaKind kind = MediaKind::Other;
    uint64_t size = 0;
    time_t mtime = 0;
    std::string ext;  // lowercase, without dot
    bool isDrive = false;   // an entry of the Storage page (SD card, USB drive, network server)
    int server = -1;        // a network server of the Storage page (index in netfs::servers())
    bool addServer = false; // the "Add network server" tile
    bool removable = false;
    std::string detail;     // overrides the size/date line when set
    bool detailsKnown = true;  // false: size and date not read yet, see Library::fillDetails
    uint32_t uiId = 0;         // cached focus id; hashing every path each frame adds up
};

// Folders first, then by the sort mode (SORT_NAME and so on), names compared naturally.
void sortEntries(std::vector<FileEntry>& entries, int sort);

struct HistoryItem {
    std::string path;
    double position = 0;
    double duration = 0;
    time_t lastPlayed = 0;
    std::string title;  // a stream's name when it had one (its address says nothing); else empty
};

struct Stream {
    std::string name;
    std::string url;
};

struct Playlist {
    std::string name;
    std::vector<std::string> items;
};

enum SortMode { SORT_NAME = 0, SORT_NAME_DESC, SORT_NEWEST, SORT_LARGEST, SORT_COUNT };
const char* sortModeName(int mode);

MediaKind kindForExtension(const std::string& ext);
std::string fileExtension(const std::string& name);
std::string baseName(const std::string& path);
std::string parentDir(const std::string& path);  // keeps trailing slash
std::string stripExtension(const std::string& name);
// A file's name as shown: the last part of the path, decoded when it is stored encoded (names.hpp).
std::string displayName(const std::string& path);
// A video's name as people say it: "@Channel_The_Wild_Robot_2024_720p_x264.mkv" -> "The Wild Robot 2024".
std::string tidyTitle(const std::string& fileName);
// What to call a file on screen: tidied when it's a video and `tidy` is on, else its name without extension.
std::string shownTitle(const std::string& path, bool tidy);
// A song's file name without its track number and underscores: "07_Sirwan-Gharibem.mp3" -> "Sirwan-Gharibem".
std::string tidySong(const std::string& fileName);
// Underscores as spaces (folder names in titles).
std::string tidySpaces(std::string s);
std::string formatSize(uint64_t bytes);
std::string formatDate(time_t t);
std::string formatTime(double secs, bool forceHours = false);
bool isUrl(const std::string& path);
// Replaces the file only once the new content is complete; a crash leaves the old one in place.
bool writeFileAtomic(const std::string& path, const std::string& data);

class Library {
public:
    void load(const std::string& dataDir);
    void save();
    void saveHistoryInBackground();  // for use during playback: SD writes can stall a frame
    void stopBackgroundSaves();

    // Sizes and dates are only read when the sort order needs them; see fillDetails.
    std::vector<FileEntry> list(const std::string& dir, bool showHidden, bool mediaOnly, int sort) const;
    static void fillDetails(FileEntry& e);
    // Playable media in the same folder as `path`, sorted by name; index of path in `out`.
    int siblings(const std::string& path, std::vector<std::string>& out) const;
    // Subtitle files whose names start like the video.
    // matchingOnly: just files named like the video (used for auto-loading).
    std::vector<std::string> subtitlesFor(const std::string& videoPath, bool matchingOnly = false) const;

    // History & resume points
    std::vector<HistoryItem> history;
    const HistoryItem* find(const std::string& path) const;
    // pages: a document's page and page count, kept exactly (no "finished" or near-start rounding).
    void updateHistory(const std::string& path, double pos, double dur, bool pages = false,
                       const std::string& title = "");  // an empty title keeps the one it had
    void removeHistory(const std::string& path);
    void clearHistory();
    double resumePosition(const std::string& path) const;  // 0 if none
    float progress(const std::string& path) const;          // 0..1, -1 if none

    std::vector<std::string> favorites;
    bool isFavorite(const std::string& path) const;
    void toggleFavorite(const std::string& path);

    std::vector<Stream> streams;

    // Likes and playlists. `revision` changes with them, so screens know to refresh.
    std::vector<std::string> liked;  // newest first
    std::vector<Playlist> playlists;
    uint64_t revision = 0;
    bool isLiked(const std::string& path) const { return likedSet_.count(path) != 0; }
    bool toggleLiked(const std::string& path);  // returns whether it is liked now
    int createPlaylist(const std::string& name);  // an existing list of that name is reused
    void deletePlaylist(int index);
    void renamePlaylist(int index, const std::string& name);
    bool addToPlaylist(int index, const std::string& path);  // false if already there
    void removeFromPlaylist(int index, size_t item);
    void movePlaylistItem(int index, size_t item, int delta);
    void forgetFile(const std::string& path);  // a file or folder was deleted
    void pathMoved(const std::string& from, const std::string& to);  // ... was moved or renamed

    // The audio and subtitle track last chosen for a video ("off", "i<track id>" or "e<name>" for a
    // subtitle file); empty when none was chosen. `type` is "audio" or "sub".
    std::string trackPref(const std::string& path, const char* type) const;
    void setTrackPref(const std::string& path, const char* type, const std::string& value);

private:
    void loadPlaylists();
    void savePlaylists();
    std::string historyText() const;
    std::unordered_set<std::string> likedSet_;
    std::map<std::string, std::pair<std::string, std::string>> trackPrefs_;  // path -> {audio, sub}
    void saveTrackPrefs();
    std::string dir_;

    std::mutex fileMutex_;   // one writer at a time for history.tsv
    std::mutex saverMutex_;
    std::condition_variable saverWake_;
    std::thread saver_;
    std::string saverPending_;
    bool saverHasWork_ = false, saverQuit_ = false;
};
