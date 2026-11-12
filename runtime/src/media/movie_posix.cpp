// SPDX-License-Identifier: GPL-3.0-or-later
// The host movie backend is Media Foundation (movie_mf.cpp), which only exists on Windows. On Linux and macOS the game's own
// lifted movie:: library runs instead, the same path as [media] movie = off on Windows: it carries the Matroska demuxer (the
// nandroid port in subsdk0) and decodes VP9 through nn::codec, which the host answers with libvpx
// (src/media/video_codec.cpp). The game's two movies are VP9 in WebM, so this path has everything it needs.
//
// Nothing is installed here, and the statistics stay zero.
#include "nxrt/log.h"
#include "nxrt/movie.h"

namespace nxrt::media {

bool install_movie_hle() {
    static bool logged = false;
    if (!logged) {
        logged = true;
        NXLOG_I("media", "movies use the game's own lifted movie:: library and the host nn::codec VP9 decoder (no Media Foundation here)");
    }
    return false;
}

MovieStats movie_stats() { return {}; }

}  // namespace nxrt::media
