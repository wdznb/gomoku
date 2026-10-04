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
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::stoi(argv[++i]);
        } else if (arg == "--web-dir" && i + 1 < argc) {
            web_dir = argv[++i];
        }
    }
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    try {
        gomoku::GameServer server(port, web_dir);
        g_server = &server;
        server.start();
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
    
    return 0;
}
