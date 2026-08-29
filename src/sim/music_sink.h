#pragma once

// Music request seam (12-audio.md §9). Called on the SIM thread —
// script dispatch (tb.play_music) and framework phase-3 transitions
// (attract/game_over autoplay) — because that is where music decisions
// happen. The app installs one implementation that drains requests to
// AudioSystem (main thread) each frame; a small mutex is fine at the
// human timescale of song changes.

namespace tb::sim {

class MusicSink {
public:
    virtual ~MusicSink() = default;
    // CONTRACT: `song_id` is valid only for the call — implementations
    // that retain it must copy (callers pass string literals and
    // script-owned buffers). Implementations must outlive every caller
    // (the host/machine hold raw pointers; the app clears them — or
    // outlives them — before teardown).
    virtual void play_music(const char* song_id) = 0;
    virtual void stop_music() = 0;
};

} // namespace tb::sim
