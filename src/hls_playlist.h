#pragma once

#include <string>
#include <vector>

// Keep one H.264 rendition and its default audio in a master playlist.
// Media playlists and masters without a compatible rendition are returned unchanged.
std::string select_hls_rendition(const std::string &playlist, int height = 0,
                                std::vector<int> *available_heights = nullptr);
