#include "hls_playlist.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
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

std::string select_hls_rendition(const std::string &playlist, int requested_height,
                                HlsSelection *selection, bool prefer_hardware) {
    if (selection) *selection = HlsSelection{};
    if (playlist.find(stream_tag) == std::string::npos) return playlist;
    std::vector<std::string> lines;
    std::istringstream input(playlist);
    for (std::string line; std::getline(input, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }

    int best = -1;
    double best_score = -std::numeric_limits<double>::infinity();
    unsigned long long best_bandwidth = 0;
    bool best_matches = false;
    bool best_hardware = false;
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
        double fps = 30;
        const auto rate = attribute(fields, "FRAME-RATE");
        if (sscanf(rate.c_str(), "%lf", &fps) != 1 || !std::isfinite(fps) || fps <= 0 || fps > 240)
            fps = 30;
        const int pixels = int(width * height);
        const double work = pixels * fps;
        constexpr double budget = 854 * 480 * 24;
        // Prefer the sharpest variant inside the decode budget. If every known
        // variant exceeds it, pick the cheapest one.
        unsigned long long bandwidth = 0;
        const auto bandwidth_field = attribute(fields, "BANDWIDTH");
        sscanf(bandwidth_field.c_str(), "%llu", &bandwidth);
        const bool hardware = prefer_hardware && requested_height == 0 && width > 0 &&
            height >= 720 && fps <= 30 && !codecs.empty() && bandwidth > 0 && bandwidth <= 6000000;
        const double score = hardware ? 2 * budget + pixels :
            pixels == 0 ? 0 : work <= budget ? budget + pixels : -work;
        size_t j = i + 1;
        while (j < lines.size() && lines[j].empty()) ++j;
        if (j >= lines.size() || lines[j].front() == '#') continue;
        if (selection && pixels > 0) selection->heights.push_back(int(height));
        const bool matches = requested_height > 0 && int(height) == requested_height && pixels > 0;
        if ((best_matches && !matches) || (best_matches == matches &&
            (score < best_score || (score == best_score &&
                (bandwidth == 0 || (best_bandwidth != 0 && bandwidth >= best_bandwidth)))))) continue;
        best = int(i);
        best_score = score;
        best_bandwidth = bandwidth;
        best_matches = matches;
        best_hardware = hardware;
        resource = lines[j];
        best_audio = attribute(fields, "AUDIO");
        selected = fields;
    }
    if (selection) {
        selection->promoted_for_hardware = best_hardware;
        auto &heights = selection->heights;
        std::sort(heights.begin(), heights.end());
        heights.erase(std::unique(heights.begin(), heights.end()), heights.end());
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
