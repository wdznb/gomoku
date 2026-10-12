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
    bool no_sessions = false;

    try {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--port" && i + 1 < argc) {
                port = std::stoi(argv[++i]);
            } else if (arg == "--web-dir" && i + 1 < argc) {
                web_dir = argv[++i];
            } else if (arg == "--book" && i + 1 < argc) {
                book_path = argv[++i];
            } else if (arg == "--no-book") {
                book_path.clear();
            } else if (arg == "--sessions" && i + 1 < argc) {
                sessions_dir = argv[++i];
            } else if (arg == "--no-sessions") {
                // Only the flag is recorded here: clearing the paths in the loop
                // would let a later --records re-open one half of persistence.
                no_sessions = true;
            } else if (arg == "--records" && i + 1 < argc) {
                records_path = argv[++i];
            } else if (arg == "--params" && i + 1 < argc) {
                params_path = argv[++i];
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

    if (no_sessions) {
        sessions_dir.clear();
        records_path.clear();
    }

    // Trained coefficients are optional; a missing or malformed file is fatal
    // rather than silently ignored, because playing strength is exactly what the
    // caller believes it is changing.
    gomoku::ThreatCoefficients coefficients;
    if (!params_path.empty()) {
        std::string error;
        if (!gomoku::CoefficientVector::load(params_path, coefficients, error)) {
            std::cerr << "Cannot load --params " << params_path << ": " << error << std::endl;
            return 1;
        }
        std::cout << "Parameters loaded: " << params_path << std::endl;
    }

    // GameServer only stores web_dir and answers every GET with "Not found", so a
    // mistyped path would still print the Open URL and leave the browser looking at
    // dead links. Fail like --params does instead.
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
                                  sessions_dir, records_path);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
