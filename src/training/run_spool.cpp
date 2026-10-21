#include "run_spool.h"

#include "json_int.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

using json = nlohmann::json;

namespace gomoku {

namespace {

bool read_required_int(const json& node, const char* key, int& out, std::string& error) {
    if (!node.contains(key)) {
        error = std::string("entry is missing ") + key;
        return false;
    }
    if (!read_json_int(node[key], out)) {
        error = std::string("entry ") + key + " must be an int that fits";
        return false;
    }
    return true;
}

bool read_required_string(const json& node, const char* key, std::string& out, std::string& error) {
    if (!node.contains(key)) {
        error = std::string("entry is missing ") + key;
        return false;
    }
    if (!node[key].is_string()) {
        error = std::string("entry ") + key + " must be a string";
        return false;
    }
    out = node[key].get<std::string>();
    return true;
}

// 与参数文件、checkpoint、开局账本同一个写法：先写临时文件再改名，半写的一份不会顶掉能用的那份。
bool atomic_write_text(const std::string& path, const std::string& text, std::string& error) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream file(tmp, std::ios::trunc);
        if (!file.is_open()) {
            error = "cannot write " + tmp;
            return false;
        }
        file << text;
        file.flush();
        if (!file.good()) {
            error = "failed while writing " + tmp;
            std::error_code remove_ec;
            std::filesystem::remove(tmp, remove_ec);
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::error_code remove_ec;
        std::filesystem::remove(tmp, remove_ec);
        error = "cannot replace " + path + ": " + ec.message();
        return false;
    }
    return true;
}

bool append_text_line(const std::string& path, const std::string& line, std::string& error) {
    std::ofstream file(path, std::ios::app);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    // 一条一开一关，写完就 flush：被 SIGKILL 打断时最多丢正在写的这一条，而留下的那半条一定没有
    // 换行结尾——读的一侧正是按「末尾没有换行的那一段」判截断的。
    file << line << "\n";
    file.flush();
    if (!file.good()) {
        error = "failed while writing " + path;
        return false;
    }
    return true;
}

std::string attempt_cursor_key(const std::string& path, int seq, int attempt) {
    return path + "|" + std::to_string(seq) + "|" + std::to_string(attempt);
}

}  // namespace

bool write_task_file(const std::string& path, const RunTask& task, std::string& error) {
    if (task.run_id.empty()) {
        error = "task has no run_id";
        return false;
    }
    if (task.games <= 0 || task.depth <= 0 || task.attempt < 1 || task.seq < 0) {
        error = "task must carry seq >= 0, attempt >= 1, games > 0 and depth > 0";
        return false;
    }
    if (task.spool_path.empty() || task.done_path.empty()) {
        error = "task needs both a spool_path and a done_path";
        return false;
    }
    json root;
    root["schema"] = "run_task/1";
    root["run_id"] = task.run_id;
    root["seq"] = task.seq;
    root["attempt"] = task.attempt;
    root["games"] = task.games;
    root["first_game_id"] = task.first_game_id;
    root["depth"] = task.depth;
    root["max_nodes"] = task.max_nodes;
    root["max_moves"] = task.max_moves;
    root["pin_cpu"] = task.pin_cpu;
    root["timeout_ms"] = task.timeout_ms;
    root["teacher_depth"] = task.teacher_depth;
    root["teacher_max_nodes"] = task.teacher_max_nodes;
    root["data_opening_plies"] = task.data_opening_plies;
    root["params_path"] = task.params_path;
    root["spool_path"] = task.spool_path;
    root["done_path"] = task.done_path;
    // 与 params_path 一样是「有才写」：任务文件的契约里那 16 个必填项一个都不动，
    // 心跳只是派活的一方主动加的通道。
    if (!task.control_socket.empty()) root["control_socket"] = task.control_socket;
    return atomic_write_text(path, root.dump(2), error);
}

bool read_task_file(const std::string& path, RunTask& out, std::string& error) {
    std::ifstream file(path);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    json root;
    try {
        root = json::parse(buffer.str());
    } catch (const json::exception& e) {
        error = std::string("task file is not valid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        error = "task file is not an object";
        return false;
    }
    RunTask task;
    if (!read_required_string(root, "run_id", task.run_id, error)) return false;
    if (!read_required_int(root, "seq", task.seq, error)) return false;
    if (!read_required_int(root, "attempt", task.attempt, error)) return false;
    if (!read_required_int(root, "games", task.games, error)) return false;
    if (!root.contains("first_game_id") || !read_json_uint(root["first_game_id"], task.first_game_id)) {
        error = "entry first_game_id must be a non-negative integer";
        return false;
    }
    if (!read_required_int(root, "depth", task.depth, error)) return false;
    if (!read_required_int(root, "max_nodes", task.max_nodes, error)) return false;
    if (!read_required_int(root, "max_moves", task.max_moves, error)) return false;
    if (!read_required_int(root, "pin_cpu", task.pin_cpu, error)) return false;
    if (!read_required_int(root, "timeout_ms", task.timeout_ms, error)) return false;
    if (!read_required_int(root, "teacher_depth", task.teacher_depth, error)) return false;
    if (!read_required_int(root, "teacher_max_nodes", task.teacher_max_nodes, error)) return false;
    if (!read_required_int(root, "data_opening_plies", task.data_opening_plies, error)) return false;
    if (!read_required_string(root, "spool_path", task.spool_path, error)) return false;
    if (!read_required_string(root, "done_path", task.done_path, error)) return false;
    if (root.contains("params_path") && root["params_path"].is_string()) {
        task.params_path = root["params_path"].get<std::string>();
    }
    if (root.contains("control_socket") && root["control_socket"].is_string()) {
        task.control_socket = root["control_socket"].get<std::string>();
    }
    if (task.run_id.empty() || task.seq < 0 || task.attempt < 1 || task.games <= 0 ||
        task.depth <= 0) {
        error = "task must carry a run_id, a seq, attempt >= 1, games > 0 and depth > 0";
        return false;
    }
    out = task;
    return true;
}

std::string spool_line_to_string(const SpoolLine& line) {
    json root;
    root["run_id"] = line.run_id;
    root["seq"] = line.seq;
    root["attempt"] = line.attempt;
    root["index"] = line.index;
    root["kind"] = line.kind;
    root["payload"] = line.payload;
    return root.dump();
}

bool parse_spool_line(const std::string& text, SpoolLine& out, std::string& error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::exception& e) {
        error = std::string("line is not valid JSON: ") + e.what();
        return false;
    }
    if (!root.is_object()) {
        error = "spool line is not an object";
        return false;
    }
    SpoolLine line;
    if (!read_required_string(root, "run_id", line.run_id, error)) return false;
    if (!read_required_int(root, "seq", line.seq, error)) return false;
    if (!read_required_int(root, "attempt", line.attempt, error)) return false;
    if (!read_required_int(root, "index", line.index, error)) return false;
    if (!read_required_string(root, "kind", line.kind, error)) return false;
    if (!read_required_string(root, "payload", line.payload, error)) return false;
    if (line.index < 1 || line.attempt < 1) {
        error = "spool line index must start at 1 and attempt at 1";
        return false;
    }
    out = line;
    return true;
}

bool SpoolWriter::open(const RunTask& task, std::string& error) {
    run_id_ = task.run_id;
    seq_ = task.seq;
    attempt_ = task.attempt;
    done_path_ = task.done_path;
    spool_path_ = task.spool_path;
    written_ = 0;
    std::error_code ec;
    const std::filesystem::path parent = std::filesystem::path(task.spool_path).parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    // 不写文件头：这一份是 JSONL，读的一侧按行解析，任何一行前缀都是多余的一条不合法行。
    std::ofstream probe(task.spool_path, std::ios::app);
    if (!probe.is_open()) {
        error = "cannot open spool " + task.spool_path;
        return false;
    }
    return true;
}

bool SpoolWriter::append(const std::string& kind, const std::string& payload, std::string& error) {
    SpoolLine line;
    line.run_id = run_id_;
    line.seq = seq_;
    line.attempt = attempt_;
    line.index = written_ + 1;
    line.kind = kind;
    line.payload = payload;
    if (!append_text_line(spool_path_, spool_line_to_string(line), error)) return false;
    ++written_;
    return true;
}

bool SpoolWriter::finish(std::string& error) {
    json root;
    root["schema"] = "run_done/1";
    root["run_id"] = run_id_;
    root["seq"] = seq_;
    root["attempt"] = attempt_;
    root["lines"] = written_;
    // 完成标记里带上 spool 路径，收账的一方不需要按命名规则猜文件在哪：标记说了才算，
    // 路径也由标记说。这样「有标记但 spool 不见」是一笔能对上的坏账，而不是一条靠约定掩盖的坏账。
    root["spool_path"] = spool_path_;
    std::ostringstream text;
    text << root.dump();
    return atomic_write_text(done_path_, text.str(), error);
}

std::size_t SpoolTail::offset(const std::string& path) const {
    const auto found = offsets_.find(path);
    return found == offsets_.end() ? 0u : found->second;
}

int SpoolTail::expected_index(const std::string& path, int seq, int attempt) const {
    const auto found = next_index_.find(attempt_cursor_key(path, seq, attempt));
    if (found == next_index_.end()) return 1;
    return found->second;
}

bool SpoolTail::read_file(const std::string& path, std::vector<SpoolLine>& lines,
                          TailReport& report, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        error = "cannot open " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string all = buffer.str();
    const std::size_t start = offset(path);
    if (start > all.size()) {
        // 文件比记住的偏移短 = 有人把它截了（或被换成了新的那一份）。不猜：报出来，让调用方处理。
        error = path + " shrank below the recorded offset " + std::to_string(start);
        return false;
    }

    // 只吃带换行的行。最后一个换行之后的字节不是「一行」，它是被杀的写者留下的半条：偏移停在它
    // 之前，等下一轮它成行，或者由重排判定它作废。
    const std::size_t last_newline = all.find_last_of('\n');
    std::size_t stop = start;
    if (last_newline != std::string::npos && last_newline + 1 > start) {
        stop = last_newline + 1;
    }
    report.truncated_tail = all.size() > stop;
    if (report.truncated_tail && report.first_drop.empty()) {
        report.first_drop = "a partial line is waiting at offset " + std::to_string(stop) +
                            " (" + std::to_string(all.size() - stop) + " bytes with no newline)";
    }

    std::size_t cursor = start;
    while (cursor < stop) {
        const std::size_t end = all.find('\n', cursor);
        const std::size_t length = (end == std::string::npos ? stop : end) - cursor;
        const std::string candidate = all.substr(cursor, length);
        cursor += length + 1;
        if (candidate.empty()) continue;

        SpoolLine parsed;
        std::string parse_error;
        if (!parse_spool_line(candidate, parsed, parse_error)) {
            report.dropped_unparsable++;
            if (report.first_drop.empty()) {
                report.first_drop = "dropped an unparsable line at offset " +
                                    std::to_string(cursor - length - 1) + ": " + parse_error;
            }
            continue;
        }
        if (parsed.run_id != run_id_) {
            report.dropped_other_run++;
            if (report.first_drop.empty()) {
                report.first_drop = "dropped a line from run " + parsed.run_id + ", this tail is " +
                                    run_id_;
            }
            continue;
        }
        const std::string key = attempt_cursor_key(path, parsed.seq, parsed.attempt);
        const int expected = expected_index(path, parsed.seq, parsed.attempt);
        if (expected < 0) {
            // 这一 attempt 已经被判废：它后面的行一条都不入账。继续读下去会把「丢了中间一条」
            // 读成「少跑了一条」，那是两种不同的结论。
            report.dropped_sequence++;
            continue;
        }
        if (parsed.index != expected) {
            report.dropped_sequence++;
            if (report.first_drop.empty()) {
                report.first_drop = "dropped seq " + std::to_string(parsed.seq) + " attempt " +
                                    std::to_string(parsed.attempt) + " index " +
                                    std::to_string(parsed.index) + ", expected " +
                                    std::to_string(expected);
            }
            next_index_[key] = -1;
            continue;
        }
        next_index_[key] = expected + 1;
        report.accepted++;
        lines.push_back(std::move(parsed));
    }
    offsets_[path] = stop;
    return true;
}

}  // namespace gomoku
