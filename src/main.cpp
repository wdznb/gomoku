#include "server/server.h"
#include <iostream>
#include <csignal>

gomoku::GameServer* g_server = nullptr;

void signal_handler(int signal) {
    if (g_server) {
        std::cout << "\nShutting down..." << std::endl;
        g_server->stop();
    }
}

int main(int argc, char* argv[]) {
    int port = 8080;
    std::string web_dir = "web";
    std::string book_path = "data/opening_book.json";

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

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    try {
        gomoku::GameServer server(port, web_dir, book_path);
        g_server = &server;
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
