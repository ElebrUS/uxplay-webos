#pragma once

#include <string>

// Keep one H.264 rendition and its default audio in a master playlist.
// Media playlists and masters without a compatible rendition are returned unchanged.
std::string select_hls_rendition(const std::string &playlist);
