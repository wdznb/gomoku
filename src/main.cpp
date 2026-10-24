#include "server/server.h"
#include "training/params.h"
#include <filesystem>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string web_dir = "web";
    std::string book_path = "data/opening_book.json";
    std::string sessions_dir = "data/sessions";
    std::string records_path = "data/training/records.jsonl";
    std::string params_path;
    int ponder_percent = 0;
    int max_sessions = 0;   // #91：0 = 不限（今天的行为）
    bool no_sessions = false;
    // Which of the two persistence paths the caller actually wrote down, as
    // opposed to the defaults above - --no-sessions needs to tell them apart.
    bool sessions_stated = false;
    bool records_stated = false;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc) {
                const std::string text = argv[++i];
                std::size_t used = 0;
                const int parsed = std::stoi(text, &used);
                // stoi stops at the first non-digit, so without this check
                // --port 9090x would silently bind 9090 as if it were asked for.
                if (used != text.size()) {
                    std::cerr << "Invalid --port " << text
                              << " (must be a whole number)" << std::endl;
                    return 1;
                }
                port = parsed;
            } else if (arg == "--web-dir" && i + 1 < argc) {
                web_dir = argv[++i];
                if (web_dir.empty()) {
                    std::cerr << "Invalid --web-dir (must not be empty)" << std::endl;
                    return 1;
                }
            } else if (arg == "--book" && i + 1 < argc) {
                book_path = argv[++i];
                if (book_path.empty()) {
                    std::cerr << "Invalid --book (must not be empty)" << std::endl;
                    return 1;
                }
            } else if (arg == "--no-book") {
                book_path.clear();
            } else if (arg == "--sessions" && i + 1 < argc) {
                sessions_dir = argv[++i];
                sessions_stated = true;
                if (sessions_dir.empty()) {
                    std::cerr << "Invalid --sessions (must not be empty)" << std::endl;
                    return 1;
                }
            } else if (arg == "--no-sessions") {
                // Only the flag is recorded here: clearing the paths in the loop
                // would let a later --records re-open one half of persistence.
                no_sessions = true;
            } else if (arg == "--records" && i + 1 < argc) {
                records_path = argv[++i];
                records_stated = true;
                if (records_path.empty()) {
                    std::cerr << "Invalid --records (must not be empty)" << std::endl;
                    return 1;
                }
            } else if (arg == "--params" && i + 1 < argc) {
                params_path = argv[++i];
                if (params_path.empty()) {
                    std::cerr << "Invalid --params (must not be empty)" << std::endl;
                    return 1;
                }
            } else if (arg == "--ponder" && i + 1 < argc) {
                // Design doc 2.4.3 ②: the share of a finished think that a session may
                // spend pre-searching the reply it predicted. Off unless asked for,
                // because it is one more engine thread per waiting session.
                const std::string text = argv[++i];
                std::size_t used = 0;
                const int parsed = std::stoi(text, &used);
                if (used != text.size() || parsed < 0 || parsed > 100) {
                    std::cerr << "Invalid --ponder " << text
                              << " (must be a whole number 0-100)" << std::endl;
                    return 1;
                }
                ponder_percent = parsed;
            } else if (arg == "--max-sessions" && i + 1 < argc) {
                // #91：detach 下来的会话原本要留满 30 分钟才下架（断线续局要的就是这段时间）。
                // 跑一整场赛具 = 每局两个新会话，实测 +14 MB/局，约 250 个会话/台就开始握手超时。
                // 给了这个名额就改成「最老的先结算并上架」：token 仍然能续，只是从文件重建。
                // 不给（0）就是今天的行为，一点不变。
                const std::string text = argv[++i];
                std::size_t used = 0;
                const int parsed = std::stoi(text, &used);
                if (used != text.size() || parsed < 0) {
                    std::cerr << "Invalid --max-sessions " << text
                              << " (must be a whole number >= 0)" << std::endl;
                    return 1;
                }
                max_sessions = parsed;
            } else {
                std::cerr << "Unknown or valueless option: " << arg << std::endl;
                return 1;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Invalid command-line argument: " << e.what() << std::endl;
        return 1;
    }

    // tcp::endpoint truncates to uint16_t silently, so an out-of-range or
    // negative --port would otherwise bind an unintended port or throw deep
    // inside the server constructor.
    if (port < 1 || port > 65535) {
        std::cerr << "Invalid --port " << port << " (must be 1-65535)" << std::endl;
        return 1;
    }

    // An explicit path alongside --no-sessions is the same ambiguous instruction
    // --sessions "" is refused for: clearing the stated directory silently would
    // re-read what the caller wrote, so the contradiction has to be named here.
    if (no_sessions && (sessions_stated || records_stated)) {
        std::cerr << "Conflicting options: --no-sessions cannot be combined with --sessions/--records"
                  << std::endl;
        return 1;
    }

    if (no_sessions) {
        sessions_dir.clear();
        records_path.clear();
    }

    // Trained coefficients are optional; a missing or malformed file is fatal
    // rather than silently ignored, because playing strength is exactly what the
    // caller believes it is changing.
    gomoku::CoefficientPair coefficients;
    if (!params_path.empty()) {
        std::string error;
        // 丁-3：按色的一对。今天那种扁平文件读进来两侧同值，所以这条路径上的
        // 每一个数与改之前一样。
        if (!gomoku::CoefficientVector::load(params_path, coefficients, error)) {
            std::cerr << "Cannot load --params " << params_path << ": " << error << std::endl;
            return 1;
        }
        std::cout << "Parameters loaded: " << params_path << std::endl;
    }

    // GameServer answers GETs by reading files out of web_dir, so a mistyped path
    // would still print the Open URL and leave the browser looking at dead links.
    // Fail like --params does instead.
    std::error_code web_ec;
    if (!std::filesystem::is_directory(web_dir, web_ec)) {
        std::cerr << "Invalid --web-dir " << web_dir << " (not a directory)" << std::endl;
        return 1;
    }

    // GameServer installs SIGINT/SIGTERM through Asio's signal_set inside
    // start(), so the io thread performs the shutdown. A raw std::signal
    // handler here would conflict with it and is not async-signal-safe.
    try {
        gomoku::GameServer server(port, web_dir, book_path,
                                  params_path.empty() ? nullptr : &coefficients,
                                  sessions_dir, records_path, ponder_percent);
        // 在 start() 之前设：之后就是 io 线程在读它了。
        server.set_max_sessions(max_sessions);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
