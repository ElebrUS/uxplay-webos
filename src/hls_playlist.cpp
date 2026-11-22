#include "hls_playlist.h"
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace {
const std::string stream_tag = "#EXT-X-STREAM-INF:";
const std::string media_tag = "#EXT-X-MEDIA:";

std::vector<std::string> attributes(const std::string &line, size_t start) {
    std::vector<std::string> fields;
    bool quoted = false;
    for (size_t i = start; i < line.size(); ++i) {
        if (line[i] == '"') quoted = !quoted;
        if (line[i] == ',' && !quoted) {
            fields.emplace_back(line.substr(start, i - start));
            start = i + 1;
        }
    }
    fields.emplace_back(line.substr(start));
    return fields;
}

std::string attribute(const std::vector<std::string> &fields, const std::string &key) {
    for (const auto &field : fields) {
        if (field.compare(0, key.size() + 1, key + "=") != 0) continue;
        auto value = field.substr(key.size() + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
            value = value.substr(1, value.size() - 2);
        return value;
    }
    return {};
}
}

std::string select_hls_rendition(const std::string &playlist) {
    if (playlist.find(stream_tag) == std::string::npos) return playlist;
    std::vector<std::string> lines;
    std::istringstream input(playlist);
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }

    int best = -1, best_score = -100000000;
    std::string resource;
    std::string best_audio;
    std::vector<std::string> selected;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i].compare(0, stream_tag.size(), stream_tag) != 0) continue;
        const auto fields = attributes(lines[i], stream_tag.size());
        const auto codecs = attribute(fields, "CODECS");
        if (!codecs.empty() && codecs.find("avc1") == std::string::npos &&
            codecs.find("avc3") == std::string::npos) continue;
        unsigned width = 0, height = 0;
        const auto resolution = attribute(fields, "RESOLUTION");
        sscanf(resolution.c_str(), "%ux%u", &width, &height);
        if (width > 1920 || height > 1088) continue;
        constexpr int preferred = 640 * 360;
        const int pixels = int(width * height);
        const int score = pixels <= preferred ? pixels : preferred - (pixels - preferred);
        size_t j = i + 1;
        while (j < lines.size() && lines[j].empty()) ++j;
        if (j >= lines.size() || lines[j].front() == '#') continue;
        if (best >= 0 && score <= best_score) continue;
        best = int(i);
        best_score = score;
        resource = lines[j];
        best_audio = attribute(fields, "AUDIO");
        selected = fields;
    }
    if (best < 0) return playlist;

    std::string audio;
    for (const auto &line : lines) {
        if (line.compare(0, media_tag.size(), media_tag) != 0 || best_audio.empty()) continue;
        const auto fields = attributes(line, media_tag.size());
        if (attribute(fields, "TYPE") != "AUDIO" || attribute(fields, "GROUP-ID") != best_audio) continue;
        if (audio.empty()) audio = line;
        if (attribute(fields, "DEFAULT") == "YES") { audio = line; break; }
    }

    std::string result = "#EXTM3U\n";
    for (const auto &line : lines) {
        if (line.compare(0, 15, "#EXT-X-VERSION:") == 0 ||
            line == "#EXT-X-INDEPENDENT-SEGMENTS" ||
            line.compare(0, 14, "#EXT-X-DEFINE:") == 0 ||
            line.compare(0, 19, "#EXT-X-SESSION-KEY:") == 0) result += line + "\n";
    }
    if (!audio.empty()) result += audio + "\n";
    result += stream_tag;
    bool first = true;
    for (const auto &field : selected) {
        if (field.compare(0, 10, "SUBTITLES=") == 0 ||
            field.compare(0, 16, "CLOSED-CAPTIONS=") == 0 ||
            field.compare(0, 6, "VIDEO=") == 0 ||
            (audio.empty() && field.compare(0, 6, "AUDIO=") == 0)) continue;
        if (!first) result += ',';
        result += field;
        first = false;
    }
    return result + "\n" + resource + "\n";
}
