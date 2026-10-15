// Session store: the envelope that lets a game outlive its socket, and the files
// it lives in. Tested against a real temporary directory because the properties
// that matter (atomic overwrite, listing, refusing a corrupt file) are file
// properties.
#include "server/session_store.h"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace gomoku;
using nlohmann::json;

namespace {

std::filesystem::path scratch_dir(const char* name) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    auto path = std::filesystem::temp_directory_path() /
                ("gomoku_session_" + std::string(name) + "_" + std::to_string(stamp));
    std::filesystem::remove_all(path);
    return path;
}

MoveRecord ply(int row, int col, int color, int score, const char* method) {
    MoveRecord record;
    record.move = Move(row, col, color);
    record.score = score;
    record.depth = 4;
    record.nodes = 1234;
    record.elapsed_ms = 56;
    record.method = method;
    return record;
}

SessionSnapshot sample(const std::string& token) {
    SessionSnapshot snap;
    snap.token = token;
    snap.created_ms = 1000;
    snap.updated_ms = 2000;
    snap.player_color = "black";
    snap.status = "playing";
    snap.position_pending = false;
    snap.timeout_ms = 1000;
    snap.max_depth = 4;
    snap.record.moves = {ply(7, 7, BLACK, 0, "book"), ply(8, 8, WHITE, 42, "alphabeta")};
    snap.record.winner = EMPTY;
    snap.record.reason = GameEndReason::INVALID;
    snap.record.total_nodes = 1234;
    snap.record.max_depth = 4;
    return snap;
}

void test_token_and_path() {
    assert(valid_token("0123456789abcdef"));
    assert(valid_token(std::string(64, 'a')));
    assert(!valid_token(""));
    assert(!valid_token(std::string(65, 'a')));
    assert(!valid_token("ABCDEF"));          // upper case is not a hex token
    assert(!valid_token("../../etc/passwd"));
    assert(!valid_token("a/../../b"));
    assert(!valid_token("a\\b"));
    assert(!valid_token("a b"));
    assert(!valid_token("a.json"));
    assert(session_path("data/sessions", "beef") == "data/sessions/beef.json");
    assert(session_path("data/sessions", "../x").empty());
    std::cout << "test_token_and_path passed" << std::endl;
}

void test_envelope_round_trip() {
    const SessionSnapshot snap = sample("deadbeef");
    const std::string text = session_to_json(snap);
    assert(text.find("\"gomoku_session\"") != std::string::npos);
    // The record is embedded in the shape the trainer already reads, so a saved
    // session is also a training record with no conversion.
    assert(text.find("\"elapsed_ms\"") != std::string::npos);

    SessionSnapshot back;
    std::string error;
    assert(session_from_json(text, back, error) && error.empty());
    assert(back.token == "deadbeef");
    assert(back.created_ms == 1000 && back.updated_ms == 2000);
    assert(back.player_color == "black" && back.status == "playing");
    assert(back.timeout_ms == 1000 && back.max_depth == 4);
    assert(!back.position_pending);
    assert(back.record.moves.size() == 2);
    assert(back.record.moves[0].move == Move(7, 7, BLACK));
    assert(back.record.moves[1].score == 42);
    assert(back.record.moves[1].method == "alphabeta");
    assert(back.record.moves[1].nodes == 1234);
    assert(back.record.total_nodes == 1234);
    // A game that started from an empty board has no base stones, and that is
    // what makes its record replayable on its own.
    assert(back.base.empty());

    SessionSnapshot setup = sample("cafe0000");
    setup.position_pending = true;
    setup.status = "waiting";
    setup.player_color.clear();
    setup.base = {Move(7, 7, BLACK), Move(6, 6, WHITE), Move(14, 0, BLACK)};
    SessionSnapshot setup_back;
    assert(session_from_json(session_to_json(setup), setup_back, error) && error.empty());
    assert(setup_back.position_pending && setup_back.player_color.empty());
    assert(setup_back.base.size() == 3);
    assert(setup_back.base[0] == Move(7, 7, BLACK));
    assert(setup_back.base[1] == Move(6, 6, WHITE));
    assert(setup_back.base[2] == Move(14, 0, BLACK));
    std::cout << "test_envelope_round_trip passed" << std::endl;
}

void test_rejects_damaged_files() {
    SessionSnapshot out;
    std::string error;

    assert(!session_from_json("", out, error));
    assert(!session_from_json("[1,2,3]", out, error));
    assert(!session_from_json("   ", out, error));
    assert(!session_from_json(R"({"version":2,"token":"ab","record":{}})", out, error));
    assert(!session_from_json(R"({"version":1,"token":"AB","record":{}})", out, error));
    assert(!session_from_json(R"({"version":1,"token":"ab"})", out, error));
    assert(!session_from_json(R"({"version":1,"token":"ab","record":{}})", out, error));
    assert(error.find("record") != std::string::npos);
    // Round 78: the record owns winner and reason, so a record without them is
    // the record case; the status check is only reached by a readable record.
    assert(!session_from_json(
               R"({"version":1,"token":"ab","status":"paused","record":{"moves":[]}})", out, error));
    assert(error.find("对局记录") != std::string::npos);
    assert(!session_from_json(
               R"({"version":1,"token":"ab","status":"paused","record":{"moves":[],"winner":"draw","reason":"invalid"}})",
               out, error));
    assert(error.find("状态") != std::string::npos);

    // Base stones are what makes a hand-set position resumable, so a file whose
    // base cannot be read exactly is refused instead of resuming the player into
    // a position they never left.
    const char* skeleton = R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},)";
    const std::string ok_base = std::string(skeleton) + R"("base":[{"row":0,"col":14,"color":"black"}]})";
    assert(session_from_json(ok_base, out, error));
    assert(out.base.size() == 1 && out.base[0] == Move(0, 14, BLACK));
    for (const char* bad : {R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":{}})",
                            R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":"7,7"})",
                            R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":[{"row":15,"col":0,"color":"black"}]})",
                            R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":[{"row":0,"color":"black"}]})",
                            R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":[{"row":0,"col":0,"color":"purple"}]})",
                            R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":[{"row":0,"col":0}]})",
                            R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":[null]})"}) {
        assert(!session_from_json(bad, out, error));
    }
    // Absent is not damaged: a game from an empty board simply has none.
    assert(session_from_json(
               R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"}})", out, error));
    assert(out.base.empty());

    // A finished session keeps its ending; the record owns winner and reason.
    SessionSnapshot done = sample("ab");
    done.status = "game_over";
    done.winner = "white";
    done.reason = "five";
    done.record.winner = WHITE;
    done.record.reason = GameEndReason::FIVE;
    assert(session_from_json(session_to_json(done), out, error));
    assert(out.status == "game_over" && out.winner == "white");
    assert(out.record.is_decisive() && out.record.outcome_for(WHITE) > 0.0f);
    std::cout << "test_rejects_damaged_files passed" << std::endl;
}

// Round 79, A4: the envelope reader now refuses a file whose position_pending is
// not a boolean (a 1 would quietly resume a setup board as a live game) and one
// whose player_color or winner names a side the game does not have. Keys that
// are simply absent keep the old defaults, and the writer's own payload must
// still read back whole.
void test_envelope_rejects_damaged_flags() {
    SessionSnapshot out;
    std::string error;

    const std::string head =
        R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},)";

    // position_pending present but not a boolean: both a number and a string
    // refuse, because either would read as the flag.
    error.clear();
    assert(!session_from_json(head + R"("position_pending":1})", out, error));
    assert(error.find("摆棋待定") != std::string::npos);
    assert(!session_from_json(head + R"("position_pending":"true"})", out, error));
    assert(error.find("摆棋待定") != std::string::npos);

    // A player colour the game does not have.
    error.clear();
    assert(!session_from_json(head + R"("player_color":"red"})", out, error));
    assert(error.find("玩家颜色") != std::string::npos);
    assert(error.find("red") != std::string::npos);

    // A winner outside ""/black/white/draw.
    error.clear();
    assert(!session_from_json(head + R"("winner":"none"})", out, error));
    assert(error.find("胜者") != std::string::npos);
    assert(error.find("none") != std::string::npos);

    // Absent is not damaged: the three keys keep their old defaults.
    error.clear();
    assert(session_from_json(
        R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"}})",
        out, error));
    assert(!out.position_pending && out.player_color.empty() && out.winner.empty());
    // The listed values of each key are still accepted.
    assert(session_from_json(head + R"("position_pending":false,"player_color":"black","winner":"draw"})",
                             out, error));

    // Positive: a payload session_to_json wrote reads back with the guarded
    // fields intact.
    SessionSnapshot live = sample("ab");
    live.position_pending = true;
    live.player_color = "white";
    SessionSnapshot live_back;
    error.clear();
    assert(session_from_json(session_to_json(live), live_back, error) && error.empty());
    assert(live_back.position_pending);
    assert(live_back.player_color == "white");

    SessionSnapshot done = sample("ab");
    done.status = "game_over";
    done.winner = "white";
    done.reason = "five";
    done.record.winner = WHITE;
    done.record.reason = GameEndReason::FIVE;
    SessionSnapshot done_back;
    error.clear();
    assert(session_from_json(session_to_json(done), done_back, error) && error.empty());
    assert(done_back.winner == "white" && done_back.status == "game_over");

    std::cout << "test_envelope_rejects_damaged_flags passed" << std::endl;
}

// R86 P1: the envelope's four textual fields (player_color / status / winner /
// reason) are read with read_string, which skips a key whose value is not a
// string - so a damaged "winner":null used to resume as "no result" instead of
// refusing the file. A present key of the wrong type now refuses it whole; a
// key that is simply absent still means the writer's "not set yet". The two
// timestamps are deliberately outside this: they are rewritten on every save,
// so refusing on their type would refuse files the writer itself will not keep.
void test_rejects_non_string_envelope_fields() {
    SessionSnapshot out;
    std::string error;

    SessionSnapshot done = sample("ab");
    done.status = "game_over";
    done.winner = "black";
    done.reason = "five";
    done.record.winner = BLACK;
    done.record.reason = GameEndReason::FIVE;
    const json base = json::parse(session_to_json(done));

    // winner in a shape no string door can read: null, a number, an array.
    for (const json& bad : {json(nullptr), json(1), json::array({7})}) {
        json doc = base;
        doc["winner"] = bad;
        error.clear();
        assert(!session_from_json(doc.dump(), out, error));
        assert(!error.empty());
    }
    // A winner string outside ""/black/white/draw is the legal-set refusal.
    {
        json doc = base;
        doc["winner"] = "BLACK";
        error.clear();
        assert(!session_from_json(doc.dump(), out, error));
        assert(!error.empty());
    }

    // player_color: null refuses; absent is "side not chosen yet" and loads on.
    {
        json doc = base;
        doc["player_color"] = nullptr;
        error.clear();
        assert(!session_from_json(doc.dump(), out, error));
        assert(!error.empty());
    }
    {
        json doc = base;
        doc.erase("player_color");
        error.clear();
        assert(session_from_json(doc.dump(), out, error) && error.empty());
        assert(out.player_color.empty());
    }

    // status: the listed strings still load, a number refuses.
    {
        json doc = base;
        doc["status"] = "playing";
        error.clear();
        assert(session_from_json(doc.dump(), out, error) && error.empty());
        assert(out.status == "playing");
    }
    {
        json doc = base;
        doc["status"] = 1;
        error.clear();
        assert(!session_from_json(doc.dump(), out, error));
        assert(!error.empty());
    }

    // reason: null refuses - the old reader cleared it and resumed anyway.
    {
        json doc = base;
        doc["reason"] = nullptr;
        error.clear();
        assert(!session_from_json(doc.dump(), out, error));
        assert(!error.empty());
    }

    // The writer's own game_over payload reads back whole.
    SessionSnapshot back;
    error.clear();
    assert(session_from_json(session_to_json(done), back, error) && error.empty());
    assert(back.token == "ab" && back.status == "game_over");
    assert(back.winner == "black" && back.reason == "five");
    assert(back.player_color == done.player_color);
    assert(back.timeout_ms == done.timeout_ms && back.max_depth == done.max_depth);
    assert(back.created_ms == done.created_ms && back.updated_ms == done.updated_ms);
    std::cout << "test_rejects_non_string_envelope_fields passed" << std::endl;
}

// R86 P2: the config fields' bounds as seen from the envelope. timeout_ms keeps
// 0 as the "not set" sentinel; any other value must sit inside [100, 120000],
// the range the set_config clamp uses. max_depth is [0, 20]. A legal value must
// read back exactly as written - clamping here would resume on a value the file
// never stated.
void test_session_config_range_edges() {
    struct Case {
        long long timeout;
        long long depth;
        bool ok;
    };
    const Case cases[] = {
        {0, 4, true},       // the "not set" sentinel stays legal
        {50, 4, false},     // below the clamp's floor
        {99, 4, false},
        {100, 4, true},     // the floor itself
        {120000, 4, true},  // the ceiling itself
        {120001, 4, false},
        {-1000, 4, false},
        {1000, 0, true},    // the "not set" depth sentinel
        {1000, 1, true},
        {1000, 20, true},
        {1000, 21, false},
        {1000, -1, false},
    };
    const json base = json::parse(session_to_json(sample("ab")));
    SessionSnapshot out;
    std::string error;
    for (const Case& c : cases) {
        json doc = base;
        doc["timeout_ms"] = c.timeout;
        doc["max_depth"] = c.depth;
        error.clear();
        const bool ok = session_from_json(doc.dump(), out, error);
        assert(ok == c.ok);
        if (ok) {
            assert(out.timeout_ms == c.timeout);
            assert(out.max_depth == c.depth);
            assert(error.empty());
        }
    }
    std::cout << "test_session_config_range_edges passed" << std::endl;
}

void test_files_are_written_atomically() {
    const auto dir = scratch_dir("atomic");
    std::string error;

    // The directory does not exist yet: saving creates it.
    SessionSnapshot snap = sample("cafe1234");
    assert(save_session(dir.string(), snap, error));
    assert(error.empty());
    assert(std::filesystem::exists(dir / "cafe1234.json"));
    // No temporary file survives a successful write.
    assert(!std::filesystem::exists(dir / "cafe1234.json.tmp"));

    snap.updated_ms = 3000;
    snap.status = "game_over";
    assert(save_session(dir.string(), snap, error));
    SessionSnapshot back;
    assert(load_session(dir.string(), "cafe1234", back, error));
    assert(back.updated_ms == 3000 && back.status == "game_over");

    // An illegal token never reaches the filesystem.
    assert(!save_session(dir.string(), sample("../escape"), error));
    assert(!std::filesystem::exists(std::filesystem::temp_directory_path() / "escape.json"));

    std::error_code ec;
    assert(list_sessions(dir.string()).size() == 1);
    assert(delete_session(dir.string(), "cafe1234", error));
    assert(!std::filesystem::exists(dir / "cafe1234.json"));
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_files_are_written_atomically passed" << std::endl;
}

// R86 P5: save_session writes through <token>.json.tmp and renames it over the
// session file. A failure early in that dance used to return with a partial tmp
// still on disk - only the rename failure swept it - and the next process-level
// retry would find a stale half-file next to the old one. The true mid-write
// failure needs the filesystem to fail inside the write (ENOSPC), which a test
// cannot stage, so what is pinned here is the reachable half of the invariant:
// the normal save leaves exactly the session file and no tmp, and the refusal
// path never creates the directory or a file at all.
void test_save_session_leaves_no_tmp() {
    const auto dir = scratch_dir("save_tmp");
    std::string error;

    assert(save_session(dir.string(), sample("cafe1234"), error));
    assert(error.empty());
    // After a normal save the directory holds exactly the session file.
    int files = 0;
    std::string only;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        ++files;
        only = entry.path().filename().string();
    }
    assert(files == 1);
    assert(only == "cafe1234.json");
    assert(!std::filesystem::exists(dir / "cafe1234.json.tmp"));

    // Overwriting keeps the invariant: the rename consumed the tmp.
    SessionSnapshot snap = sample("cafe1234");
    snap.updated_ms = 42;
    assert(save_session(dir.string(), snap, error));
    files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        (void)entry;
        ++files;
    }
    assert(files == 1);
    assert(!std::filesystem::exists(dir / "cafe1234.json.tmp"));

    // An illegal token is refused before the directory or any file exists.
    const auto untouchable = scratch_dir("save_tmp_illegal");
    error.clear();
    assert(!save_session(untouchable.string(), sample("../escape"), error));
    assert(!error.empty());
    assert(!std::filesystem::exists(untouchable));
    assert(list_sessions(untouchable.string()).empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_save_session_leaves_no_tmp passed" << std::endl;
}

void test_listing_and_corrupt_files() {
    const auto dir = scratch_dir("listing");
    std::string error;

    assert(save_session(dir.string(), sample("aaaa"), error));
    assert(save_session(dir.string(), sample("bbbb"), error));
    {
        std::ofstream(dir / "notes.txt") << "not a session";
        std::ofstream(dir / "cccc.json.tmp") << "half written";
    }

    const auto found = list_sessions(dir.string());
    assert(found.size() == 2);
    assert(found[0].find("aaaa.json") != std::string::npos);
    assert(found[1].find("bbbb.json") != std::string::npos);

    // A session file that was truncated by a crash is refused, not half-read.
    { std::ofstream(dir / "dddd.json") << R"({"version":1,"token":"dd)"; }
    SessionSnapshot out;
    assert(!load_session(dir.string(), "dddd", out, error));
    assert(!load_session(dir.string(), "nope", out, error));
    assert(!load_session(dir.string(), "../outside", out, error));

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_listing_and_corrupt_files passed" << std::endl;
}

// Round 72, flush_entry's empty-board branch calls delete_session on every tick
// of a zero-stone session. A missing file is the normal state and must come back
// as success with no error, or every tick would log a failure and never commit
// its dedup stamp.
void test_delete_session_is_idempotent() {
    const auto dir = scratch_dir("delete");
    std::string error;

    // Neither the directory nor the file exists: nothing to delete is success.
    assert(delete_session(dir.string(), "beef", error));
    assert(error.empty());

    SessionSnapshot snap = sample("beef");
    assert(save_session(dir.string(), snap, error));
    // A stale temporary from an interrupted write goes with the file.
    { std::ofstream(dir / "beef.json.tmp") << "half written"; }
    assert(delete_session(dir.string(), "beef", error));
    assert(error.empty());
    assert(!std::filesystem::exists(dir / "beef.json"));
    assert(!std::filesystem::exists(dir / "beef.json.tmp"));

    // Deleting again is the state after the first tick, not an error.
    assert(delete_session(dir.string(), "beef", error));
    assert(error.empty());
    assert(!delete_session(dir.string(), "../escape", error));
    assert(!error.empty());

    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    std::cout << "test_delete_session_is_idempotent passed" << std::endl;
}

// Defect 1: Unvalidated timeout_ms/max_depth in session restore (session_store.cpp:121-122)
// A corrupted session file with negative or extreme values should be rejected
// rather than loaded without validation.
void test_session_config_validation() {
    SessionSnapshot out;
    std::string error;

    // Negative timeout_ms should be rejected
    const char* neg_timeout = R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid","total_nodes":0,"max_depth":0},"timeout_ms":-1000,"max_depth":4})";
    assert(!session_from_json(neg_timeout, out, error));

    // Excessively large timeout_ms (>120s) should be rejected
    const char* huge_timeout = R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid","total_nodes":0,"max_depth":0},"timeout_ms":999999999,"max_depth":4})";
    assert(!session_from_json(huge_timeout, out, error));

    // Negative max_depth should be rejected
    const char* neg_depth = R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid","total_nodes":0,"max_depth":0},"timeout_ms":5000,"max_depth":-5})";
    assert(!session_from_json(neg_depth, out, error));

    // Excessively large max_depth (>20) should be rejected
    const char* huge_depth = R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid","total_nodes":0,"max_depth":0},"timeout_ms":5000,"max_depth":999})";
    assert(!session_from_json(huge_depth, out, error));

    // Valid values within bounds should still work
    const char* valid_config = R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid","total_nodes":0,"max_depth":0},"timeout_ms":5000,"max_depth":8})";
    assert(session_from_json(valid_config, out, error));
    assert(out.timeout_ms == 5000 && out.max_depth == 8);

    std::cout << "test_session_config_validation passed" << std::endl;
}

// Round 75, E2: 64-bit integers that only a truncating cast could love. JSON
// holds integers in 64 bits and get<int> casts, so version 2^32+1 read back as 1
// and the file was taken for this format, base row 2^32+3 became 3, and the two
// config fields bypassed their bounds outright. Every one of those must now
// refuse the whole file instead of resuming on a value the writer never meant.
void test_rejects_wide_integer_fields() {
    SessionSnapshot out;
    std::string error;

    // 2^32 + 1 truncates to 1: the old reader accepted this as version 1.
    error.clear();
    assert(!session_from_json(
        R"({"version":4294967297,"token":"ab","status":"playing","record":{"moves":[]}})",
        out, error));
    assert(error.find("版本") != std::string::npos);

    // A base stone row of 2^32 + 3 truncates to 3, moving the stone. The file is
    // refused wholesale rather than read with a wrong square. (The record is
    // valid so the refusal really is the base check, not the record parse.)
    const char* wide_row =
        R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":[{"row":4294967299,"col":7,"color":"black"}]})";
    error.clear();
    assert(!session_from_json(wide_row, out, error));
    assert(error.find("摆棋") != std::string::npos);
    // The column door as well.
    assert(!session_from_json(
        R"({"version":1,"token":"ab","status":"playing","record":{"moves":[],"winner":"draw","reason":"invalid"},"base":[{"row":7,"col":4294967304,"color":"black"}]})",
        out, error));

    // Same for the two config fields, whose bounds the truncated value would
    // otherwise sail past.
    error.clear();
    assert(!session_from_json(
        R"({"version":1,"token":"ab","status":"playing","record":{"moves":[]},"timeout_ms":4294967297,"max_depth":4})",
        out, error));
    assert(error.find("超时") != std::string::npos);
    error.clear();
    assert(!session_from_json(
        R"({"version":1,"token":"ab","status":"playing","record":{"moves":[]},"timeout_ms":5000,"max_depth":4294967300})",
        out, error));
    assert(error.find("深度") != std::string::npos);

    // Through the file door as well: load_session refuses the whole file, and
    // nothing of it reaches the caller.
    const auto dir = scratch_dir("wide");
    std::filesystem::create_directories(dir);
    const std::string token = "abcdef0123456789";
    { std::ofstream(dir / (token + ".json")) << wide_row; }
    SessionSnapshot loaded;
    error.clear();
    assert(!load_session(dir.string(), token, loaded, error));
    assert(!error.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // The 64-bit timestamps are not meant to become ints and still round-trip
    // whole: a cast would fold 2^32 + 5 down to 5.
    SessionSnapshot snap = sample("ab");
    snap.created_ms = 4294967301LL;
    snap.updated_ms = 4294967302LL;
    error.clear();
    assert(session_from_json(session_to_json(snap), out, error) && error.empty());
    assert(out.created_ms == 4294967301LL && out.updated_ms == 4294967302LL);

    std::cout << "test_rejects_wide_integer_fields passed" << std::endl;
}

// R97 BB4: a created_ms / updated_ms that is written but is not an integer is a
// claim about the file's birth, not an absent field. read_number used to skip
// silently and leave the 0 default, so the next open_session wrote its own
// now_ms back as the session's birth time - a value the file never stated,
// replacing the one it did. Absent keys keep the 0 sentinel, and a real 64-bit
// integer (the case above) still loads whole.
void test_rejects_non_integer_timestamp_fields() {
    SessionSnapshot out;
    std::string error;

    const std::string good = session_to_json(sample("ab"));
    error.clear();
    assert(session_from_json(good, out, error) && error.empty());

    const auto with = [](const char* key, const json& value) {
        json root = json::parse(session_to_json(sample("ab")));
        root[key] = value;
        return root.dump();
    };

    // null, a string, and a float past int range: present, not an integer.
    const json bad_values[] = {json(), json("1720000000000"), json(1.72e12)};
    for (const json& value : bad_values) {
        error.clear();
        assert(!session_from_json(with("created_ms", value), out, error));
        assert(!error.empty());  // it says why, without the test pinning the wording
        error.clear();
        assert(!session_from_json(with("updated_ms", value), out, error));
        assert(!error.empty());
    }

    // A whole-numbered float is still a float: 2^32 + 5 written as .0 is not the
    // integer the wide case above proved the reader can carry.
    error.clear();
    assert(!session_from_json(with("created_ms", json(4294967301.0)), out, error));

    // Through the file door too: nothing of the refused file reaches the caller.
    const auto dir = scratch_dir("bad_stamp");
    std::filesystem::create_directories(dir);
    const std::string token = "abcdef0123456789";
    { std::ofstream(dir / (token + ".json")) << with("created_ms", json(1.72e12)); }
    SessionSnapshot loaded;
    error.clear();
    assert(!load_session(dir.string(), token, loaded, error));
    assert(!error.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);

    // Both keys absent is the other half of the rule: legal, and a freshly read
    // snapshot keeps the 0 sentinel for each of them. (A fresh object: the
    // reader only writes what the file states.)
    json stripped = json::parse(good);
    stripped.erase("created_ms");
    stripped.erase("updated_ms");
    SessionSnapshot untouched;
    error.clear();
    assert(session_from_json(stripped.dump(), untouched, error) && error.empty());
    assert(untouched.created_ms == 0 && untouched.updated_ms == 0);

    // One key absent, the other a real integer: the present one still lands.
    json half = json::parse(good);
    half.erase("updated_ms");
    half["created_ms"] = 4294967301LL;
    SessionSnapshot only_created;
    error.clear();
    assert(session_from_json(half.dump(), only_created, error) && error.empty());
    assert(only_created.created_ms == 4294967301LL && only_created.updated_ms == 0);

    std::cout << "test_rejects_non_integer_timestamp_fields passed" << std::endl;
}

// Round 75, F5: a session file is addressable by its own name, and load_session
// compares the token inside against the name it read. A file renamed under a
// different token would resume as a game the URL never promised, and every
// later write would go to a name this file can never be - so the load is
// refused whole. The file itself is the user's data and stays on disk.
void test_load_refuses_token_mismatch() {
    const auto dir = scratch_dir("token_mismatch");
    std::string error;
    std::error_code ec;

    assert(save_session(dir.string(), sample("bbbb"), error));
    std::filesystem::rename(dir / "bbbb.json", dir / "aaaa.json", ec);
    assert(!ec);

    SessionSnapshot out;
    error.clear();
    assert(!load_session(dir.string(), "aaaa", out, error));
    assert(error.find("token") != std::string::npos);
    assert(std::filesystem::exists(dir / "aaaa.json"));

    // The name the file used to answer to is gone: nothing loads under it, and
    // the mismatch was not resolved by inventing a second file.
    error.clear();
    assert(!load_session(dir.string(), "bbbb", out, error));
    assert(!std::filesystem::exists(dir / "bbbb.json"));

    // Control: the same envelope under its own name loads exactly as before.
    assert(save_session(dir.string(), sample("cccc"), error));
    error.clear();
    assert(load_session(dir.string(), "cccc", out, error));
    assert(out.token == "cccc");

    // A half-written temporary never counts as a session in the listing, and
    // the refused file is still listed under the name it does not match - the
    // listing reports files, the loader is what validates them.
    { std::ofstream(dir / "dddd.json.tmp") << "half written"; }
    const auto found = list_sessions(dir.string());
    assert(found.size() == 2);
    bool saw_renamed = false;
    for (const std::string& path : found) {
        assert(path.find(".json.tmp") == std::string::npos);
        if (path.find("aaaa.json") != std::string::npos) saw_renamed = true;
    }
    assert(saw_renamed);

    std::filesystem::remove_all(dir, ec);
    std::cout << "test_load_refuses_token_mismatch passed" << std::endl;
}

// Round 73, delete_session: a real failure on the main file must come back as
// false with a message. The old code shared one error_code between the main
// remove and the .tmp sweep, so an absent .tmp cleared the error and a session
// that was still on disk was reported deleted.
void test_delete_session_reports_main_failure() {
    const auto dir = scratch_dir("delete_failure");
    std::string error;

    SessionSnapshot snap = sample("beef");
    assert(save_session(dir.string(), snap, error));

    // The session file is a non-empty directory: the main remove() genuinely
    // fails, and the sweep of the .tmp (which does not exist) must not mask it.
    std::error_code ec;
    std::filesystem::remove(dir / "beef.json", ec);
    assert(std::filesystem::create_directory(dir / "beef.json", ec));
    { std::ofstream(dir / "beef.json" / "keeper.txt") << "inside"; }

    error.clear();
    assert(!delete_session(dir.string(), "beef", error));
    assert(!error.empty());
    assert(std::filesystem::exists(dir / "beef.json"));

    // A retry reports the same failure instead of succeeding by absence of the
    // temporary.
    error.clear();
    assert(!delete_session(dir.string(), "beef", error));
    assert(!error.empty());
    assert(std::filesystem::exists(dir / "beef.json"));

    // With the obstruction gone the old semantics are intact: deleting what is
    // there succeeds, and deleting what is not there is still success.
    std::filesystem::remove_all(dir / "beef.json", ec);
    error.clear();
    assert(delete_session(dir.string(), "beef", error));
    assert(error.empty());
    assert(!std::filesystem::exists(dir / "beef.json"));
    assert(delete_session(dir.string(), "beef", error));
    assert(error.empty());

    std::filesystem::remove_all(dir, ec);
    std::cout << "test_delete_session_reports_main_failure passed" << std::endl;
}

}  // namespace

int main() {
    test_token_and_path();
    test_envelope_round_trip();
    test_rejects_damaged_files();
    test_envelope_rejects_damaged_flags();
    test_rejects_non_string_envelope_fields();
    test_session_config_range_edges();
    test_files_are_written_atomically();
    test_save_session_leaves_no_tmp();
    test_listing_and_corrupt_files();
    test_delete_session_is_idempotent();
    test_delete_session_reports_main_failure();
    test_session_config_validation();
    test_rejects_wide_integer_fields();
    test_rejects_non_integer_timestamp_fields();
    test_load_refuses_token_mismatch();
    std::cout << "All session store tests passed!" << std::endl;
    return 0;
}
