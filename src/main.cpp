#include "server/server.h"
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string web_dir = "web";
    std::string book_path = "data/opening_book.json";

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

    // GameServer installs SIGINT/SIGTERM through Asio's signal_set inside
    // start(), so the io thread performs the shutdown. A raw std::signal
    // handler here would conflict with it and is not async-signal-safe.
    try {
        gomoku::GameServer server(port, web_dir, book_path);
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
