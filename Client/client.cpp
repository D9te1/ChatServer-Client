#define _CRT_SECURE_NO_WARNINGS
#include <iostream>
#include <string>
#include <thread>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <vector>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")

std::string get_timestamp() {
    auto now = std::time(nullptr);
    auto t = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&t, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
        case '"': result += "\\\""; break;
        case '\\': result += "\\\\"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        default: result += c; break;
        }
    }
    return result;
}

// Функция для извлечения значения из JSON
std::string extract_json_value(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\":\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";

    pos += search.length();
    size_t end = json.find("\"", pos);
    if (end == std::string::npos) return "";

    return json.substr(pos, end - pos);
}

void receiveMessages(SOCKET sock) {
    char buffer[4096];
    std::string inbuf;

    while (true) {
        int bytesReceived = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) {
            std::cout << "\n[Connection lost]\n";
            closesocket(sock);
            WSACleanup();
            exit(0);
        }
        buffer[bytesReceived] = '\0';
        inbuf += buffer;

        // Парсим по строкам (по \n)
        size_t pos = 0;
        while ((pos = inbuf.find('\n')) != std::string::npos) {
            std::string json_line = inbuf.substr(0, pos);
            inbuf.erase(0, pos + 1);

            if (json_line.empty()) continue;

            std::string msg_type = extract_json_value(json_line, "type");
            std::string msg_text = extract_json_value(json_line, "text");
            std::string msg_from = extract_json_value(json_line, "from");

            if (msg_from.empty()) msg_from = "system";

            if (msg_type == "message") {
                std::cout << "\r[" << msg_from << "]: " << msg_text << "\n> ";
            }
            else if (msg_type == "system") {
                std::cout << "\r[system]: " << msg_text << "\n> ";
            }
            else if (msg_type == "pm") {
                std::cout << "\r[PM from " << msg_from << "]: " << msg_text << "\n> ";
            }
            else if (msg_type == "error") {
                std::cout << "\r[ERROR]: " << msg_text << "\n> ";
            }
            else if (msg_type == "list") {
                std::cout << "\r[Users online]: " << msg_text << "\n> ";
            }
            else if (msg_type == "nick_changed") {
                std::cout << "\r[" << msg_from << " changed name to " << msg_text << "]\n> ";
            }

            std::flush(std::cout);
        }
    }
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Socket creation failed\n";
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(8080);
    inet_pton(AF_INET, "127.0.0.1", &serverAddr.sin_addr);

    if (connect(sock, (struct sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cout << "Couldn't connect to server\n";
        closesocket(sock);
        WSACleanup();
        return 1;
    }

    std::cout << "Connected to chat server!\n";
    std::cout << "Commands:\n";
    std::cout << "  /nick <name> - change nickname\n";
    std::cout << "  /list - show online users\n";
    std::cout << "  /pm <name> <message> - private message\n";
    std::cout << "  /quit - exit\n\n";
    std::cout << "> ";

    // Запускаем поток для получения сообщений
    std::thread(receiveMessages, sock).detach();

    std::string nickname = "Guest";
    std::string message;

    while (true) {
        std::getline(std::cin, message);

        if (message.empty()) {
            std::cout << "> ";
            continue;
        }

        if (message == "/quit") {
            std::cout << "Disconnecting...\n";
            break;
        }

        // Формируем JSON сообщение + \n
        std::string json_msg;

        if (message.rfind("/nick ", 0) == 0) {
            std::string new_nick = message.substr(6);
            json_msg = "{\"type\":\"nick\",\"from\":\"" + nickname +
                "\",\"text\":\"" + escape_json(new_nick) +
                "\",\"timestamp\":\"" + get_timestamp() + "\"}\n";
            nickname = new_nick;
        }
        else if (message.rfind("/pm ", 0) == 0) {
            size_t space1 = message.find(' ', 4);
            if (space1 != std::string::npos) {
                std::string target = message.substr(4, space1 - 4);
                std::string pm_text = message.substr(space1 + 1);
                json_msg = "{\"type\":\"pm\",\"from\":\"" + nickname +
                    "\",\"to\":\"" + escape_json(target) +
                    "\",\"text\":\"" + escape_json(pm_text) +
                    "\",\"timestamp\":\"" + get_timestamp() + "\"}\n";
            }
            else {
                std::cout << "Usage: /pm <name> <message>\n> ";
                continue;
            }
        }
        else if (message == "/list") {
            json_msg = "{\"type\":\"list\",\"from\":\"" + nickname +
                "\",\"text\":\"\",\"timestamp\":\"" + get_timestamp() + "\"}";
        }
        else {
            // Обычное сообщение
            json_msg = "{\"type\":\"message\",\"from\":\"" + nickname +
                "\",\"text\":\"" + escape_json(message) +
                "\",\"timestamp\":\"" + get_timestamp() + "\"}";
        }

        if (!json_msg.empty()) {
            std::string to_send = json_msg + "\n";  // JSON + \n
            int sent = send(sock, to_send.c_str(), (int)to_send.length(), 0);

            if (sent == SOCKET_ERROR) {
                std::cout << "\nSend failed. Connection lost?\n";
                break;
            }
            std::cout << "> ";
        }
    }
    closesocket(sock);
    WSACleanup();
    return 0;
}