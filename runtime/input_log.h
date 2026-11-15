// Input logging and replay.
//
// A run leaves behind a record of every controller state the game polled, keyed by the
// poll's sequence number, so that the same route can be played again without a human at
// the controls -- "get me to the boss" rather than scripting a handful of presses. With
// the virtual clock (runtime.h) the guest's timeline is a function of its own execution,
// so the Nth poll of a replay is the same instant in the game as the Nth poll of the
// recording, and the replay is exact. The log also records the run's start time, which
// the replay feeds to the RTC, so even the calendar looks the same to the game.
//
// Version 1 logs (keyed by presented frame, recorded under the host clock) still load;
// they replay approximately, as they always did.
//
// A log is a directory:
//   inputs.txt       one line per change of pad state (see the file's own header)
//   memcard_a.raw    the memory card as it was when the run started, if there was one
// A replay restores that card to a scratch copy so that the run sees the same saves and
// the real card is untouched.
#pragma once
#include "hw/pad.h"
#include <cstdint>
#include <string>

// Start recording into `dir` (created). `memcard_path` is copied in as the snapshot when
// it exists. Returns false, and records nothing, if the directory cannot be made.
bool input_log_start(const std::string& dir, const std::string& memcard_path);


// Load `dir` for replay. The card snapshot, if any, is copied to `dir`/memcard_replay.raw
// and `memcard_out` receives that path; the frontend points the EXI card at it before
// boot. Returns false if there is no inputs.txt.
bool input_replay_load(const std::string& dir, std::string& memcard_out);

// Marks where the run ended: the log's last line is otherwise the last *change*, and a
// replay would hand control back to the live pad there, which can be well before the end.
// Called on a normal exit and from the crash and check handlers, so that a replay holds
// the last state up to the instant the run stopped.
void input_log_end();

// A marker: the player saw something worth a second look and pressed the mark key (F9 or a
// controller's right stick click, main.cpp). Logged as a comment,
// `# mark <n> <poll> <frame> <present> <tick>`: the marker's number from 1, the pad polls
// so far, the submitted frame, `present` (the presented frame on screen, which frame dumps
// are numbered by) and the virtual time. Being a comment, a replay passes it by, and so
// does any older replayer: a log with markers replays exactly as it would without them.
// Any thread; the poll and the time are read as they stand. Returns the marker's number,
// or 0 when no log is being written.
int input_log_mark(uint32_t present);

// Called by the SI layer each time the guest polls a pad, with the live state in `s`.
// While a replay is loaded and not yet past its end, overwrites `s` with the logged state
// for `chan` and returns true; past the end the live controller takes over again and this
// returns false. Either way the state the guest will see is recorded, when logging.
bool input_pad_poll(int chan, PadState& s);

// Whether a replay is in progress (loaded and not yet past its last poll).
bool input_replay_active();

// The pad polls so far, all channels: the key a replay by poll goes by. Guest thread.
uint32_t input_poll_seq();

// A poll is any read the guest makes of the SI's pad registers, so the count of them
// depends on how often the game reads: a game made to run at another frame rate reads
// more or less often a second, and a replay keyed by poll hands it the inputs per game
// frame instead of per unit of time. Since version 6 each pad line also carries the
// poll's virtual time; replaying by time (this, or GCN_REPLAY_BY_TIME=1) applies each
// change at the first poll at or after that time instead, so a route recorded at one
// frame rate drives the game at the same instants at another. Exact only at the
// recording's own rate and work, where polls sharing a time can still split differently;
// the recorded clock jumps, keyed by back-edge count, are left out. A log older than v6
// replays by poll whatever is asked.
void input_replay_by_time(bool on);

// Poses the game's hooks read on the guest thread (vr::hand_pose and vr::eye_views in
// vr_game.h) are input too: a game that aims from a controller or culls to the viewer's
// head steers its simulation by them, and a replay without them parts from the recording
// as soon as they matter. Each read is counted per kind, in the order the guest makes
// them, and a change in what a read returned is logged against that count
// (`pose <kind> <read> <valid> <values...>`); a replay answers the Nth read of a kind
// with the state its log had at N, so a hook sees exactly what it saw. Only reads from
// the guest thread are logged: a frontend's own reads do not reach the game. A log
// without pose lines, or one that has run out, leaves the live poses in force.
// `valid` and the `n` floats are the live result on entry and the result to use on
// return; returns true when a replay supplied it.
bool input_pose(const char* kind, bool* valid, float* v, int n);

// The clock's catch-up jumps (clock_pace in cpu.cpp): a jump of `edges` back-edges made
// when the guest's back-edge count was `at`. Recorded so that a replay makes the same
// jump at the same count, which keeps it exact: the guest then reads the same time base
// at every instruction. A log without any (recorded before version 3, or on a host that
// kept up) replays without any.
void input_log_jump(uint64_t at, uint64_t edges);
// The count the next recorded jump is due at, UINT64_MAX when none: the clock polls there.
uint64_t input_replay_next_jump_at();
// If a recorded jump is due at `count` (or was passed), takes it and returns its size.
uint64_t input_replay_take_jump(uint64_t count);

// Settings the guest reads (settings.h, AffectsGuest) are input too: a game that changes
// what it does by one parts from its recording as soon as the value differs. Each value
// the guest latches is logged against the submitted frame it took effect at
// (`setting <frame> <key> <value>`), the values it started with at frame 0, and a replay
// latches the logged values at the same frames instead of the live ones while it lasts.
void input_log_setting(uint32_t frame, const char* key, const char* value);
// In a replay: calls `fn` for every recorded setting due by `frame` and not yet applied, in
// the order logged. A log without setting lines calls nothing.
void input_replay_settings(uint32_t frame, void (*fn)(const char* key, const char* value));
