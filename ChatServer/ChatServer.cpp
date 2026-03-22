#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iostream>
#include <string>
#include <atomic>
#include <mutex>
#include <list>
#include <memory>
#include <algorithm>
#include <vector>
#include <clocale>
#include <fstream>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <regex>

#pragma comment(lib, "Ws2_32.lib")

// JSON парсер (упрощённый)
struct JsonMessage {
    std::string type;      // "message", "nick", "list", "quit", "pm"
    std::string from;
    std::string text;
    std::string to;        // для PM
    std::string timestamp;

    std::string to_json() const {
        std::string json = "{";
        json += "\"type\":\"" + type + "\",";
        json += "\"from\":\"" + from + "\",";
        if (!to.empty()) json += "\"to\":\"" + to + "\",";
        json += "\"text\":\"" + escape_json(text) + "\",";
        json += "\"timestamp\":\"" + timestamp + "\"";
        json += "}";
        return json + "\n";
    }

private:
    static std::string escape_json(const std::string& s) {
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
};

std::atomic<int> client_counter{ 0 };
std::fstream logfile;

struct Client {
    SOCKET socket = INVALID_SOCKET;
    std::string name;
    std::string inbuf;
    int msg_count = 0;
    time_t last_msg = 0;
    bool banned = false;
    size_t buf_limit = 8192; // лимит буфера

    Client(SOCKET s) : socket(s) {
        int num = ++client_counter;
        name = "Guest" + std::to_string(num);
        last_msg = time(nullptr);
    }
};

std::list<std::shared_ptr<Client>> clients;
std::mutex clients_mutex;

std::string get_timestamp() {
    auto now = std::time(nullptr);
    auto t = *std::localtime(&now);
    std::ostringstream oss;
    oss << std::put_time(&t, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void log(const std::string& msg) {
    std::string logmsg = "[" + get_timestamp() + "] " + msg + "\n";
    std::cout << logmsg;
    if (logfile.is_open()) {
        logfile << logmsg;
        logfile.flush();
    }
}

std::string validate_input(const std::string& input) {
    // Максимум 512 символов
    if (input.size() > 512) return input.substr(0, 512);

    // Запрещённые символы в нике: < > & \ /
    static const std::regex invalid_chars("[<>&\\\\]");
    std::string safe = std::regex_replace(input, invalid_chars, "*");

    // Только printable ASCII
    std::string result;
    for (char c : safe) {
        if (c >= 32 && c <= 126) result += c;
    }
    return result;
}

bool is_rate_limited(const std::shared_ptr<Client>& c) {
    time_t now = time(nullptr);
    if (now - c->last_msg) { // 
        c->msg_count++;
        if (c->msg_count > 5) { // бан при 5+ сообщениях/сек
            log("RATE LIMIT: " + c->name);
            return true;
        }
        return true;
    }

    c->last_msg = now;
    c->msg_count = 1;
    return false;
}

bool setTcpKeepalive(SOCKET socket) {
    struct tcp_keepalive {
        ULONG onoff;
        ULONG keepalivetime;
        ULONG keepaliveinterval;
    } keepalive_vals{ 1, 30000, 5000 };

    DWORD bytes_returned = 0;
    return WSAIoctl(socket, SIO_KEEPALIVE_VALS, &keepalive_vals, sizeof(keepalive_vals),
        NULL, 0, &bytes_returned, NULL, NULL) != SOCKET_ERROR;
}

bool sendAll(SOCKET s, const std::string& msg) {
    return send(s, msg.c_str(), (int)msg.size(), 0) != SOCKET_ERROR;
}

void safeBroadcast(const std::string& message, const std::shared_ptr<Client>& exclude = nullptr) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    log("[BROADCAST] " + message);

    for (auto it = clients.begin(); it != clients.end(); ) {
        auto c = *it;
        if (c->socket != INVALID_SOCKET &&
            (!exclude || c != exclude) &&
            sendAll(c->socket, message)) {
            ++it;
        }
        else {
            closesocket(c->socket);
            c->socket = INVALID_SOCKET;
            it = clients.erase(it);
        }
    }
}

void safeDropClient(const std::shared_ptr<Client>& c) {
    if (!c || c->socket == INVALID_SOCKET) return;

    std::lock_guard<std::mutex> lock(clients_mutex);
    log("DISCONNECT: " + c->name);
    closesocket(c->socket);
    c->socket = INVALID_SOCKET;
    clients.remove(c);
}

void send_to_client(const std::shared_ptr<Client>& c, const JsonMessage& msg) {
    if (c->socket != INVALID_SOCKET) {
        sendAll(c->socket, msg.to_json());
    }
}

void send_private_message(const std::shared_ptr<Client>& from,
    const std::shared_ptr<Client>& to,
    const std::string& text) {
    if (!from || !to) return;

    JsonMessage msg;
    msg.type = "pm";
    msg.from = from->name;
    msg.to = to->name;
    msg.text = text;
    msg.timestamp = get_timestamp();

    send_to_client(to, msg);

    // уведомление отправителю
    msg.type = "pm_sent";
    send_to_client(from, msg);

    log("PM: " + from->name + " -> " + to->name + ": " + text);
}

std::shared_ptr<Client> find_client(const std::string& name) {
    std::lock_guard<std::mutex> lock(clients_mutex);
    for (auto& c : clients) {
        if (c->socket != INVALID_SOCKET && c->name == name) {
            return c;
        }
    }
    return nullptr;
}

void process_message(const std::shared_ptr<Client>& c, const std::string& line) {
    if (line.empty()) return;

    std::string clean_line = validate_input(line);
    if (clean_line.empty()) return;
    // Rate limiting
    if (is_rate_limited(c)) {
        JsonMessage err;
        err.type = "error";
        err.from = "system";
        err.text = "Rate limited. Slow down!";
        err.timestamp = get_timestamp();
        send_to_client(c, err);
        return;
    }

    // Команды (только для себя)
    if (clean_line[0] == '/') {
        JsonMessage resp;
        resp.from = "system";
        resp.timestamp = get_timestamp();

        if (clean_line == "/list") {
            resp.type = "list";
            int count = 0;
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto& cl : clients) {
                if (cl->socket != INVALID_SOCKET) {
                    resp.text += cl->name + " ";
                    count++;
                }
            }
            resp.text += "(" + std::to_string(count) + ")";
        }
        else if (clean_line.rfind("/nick ", 0) == 0) {
            std::string new_name = validate_input(clean_line.substr(5));
            if (new_name.size() < 20 && !find_client(new_name)) {
                std::string old = c->name;
                c->name = new_name;
                resp.type = "nick_changed";
                resp.text = new_name;
                log("NICK: " + old + " -> " + new_name);
            }
            else {
                resp.type = "error";
                resp.text = "Invalid or taken nickname";
            }
        }
        else if (clean_line.rfind("/pm ", 0) == 0) {
            size_t space = clean_line.find(' ', 4);
            if (space != std::string::npos) {
                std::string target = clean_line.substr(4, space - 4);
                std::string msg = clean_line.substr(space + 1);
                auto target_client = find_client(target);
                if (target_client) {
                    send_private_message(c, target_client, msg);
                    return; // не логируем в общий чат
                }
                else {
                    resp.type = "error";
                    resp.text = target + " not found";
                }
            }
        }
        else {
            resp.type = "error";
            resp.text = "Unknown command";
        }

        send_to_client(c, resp);
        return;
    }

    // Обычное сообщение
    JsonMessage msg;
    msg.type = "message";
    msg.from = c->name;
    msg.text = clean_line;
    msg.timestamp = get_timestamp();

    safeBroadcast(msg.to_json(), c);
}

void processMessages(const std::shared_ptr<Client>& c) {
    std::cout << "DEBUG: Received buffer from " << c->name << ": " << c->inbuf << std::endl;

    while (true) {
        size_t eol = c->inbuf.find('\n');
        if (eol == std::string::npos) break;

        std::string json_line = c->inbuf.substr(0, eol);
        c->inbuf.erase(0, eol + 1);

        // Пустая строка — пропускаем
        if (json_line.empty()) continue;

        // Парсим тип сообщения
        size_t type_pos = json_line.find("\"type\":\"") + 8;
        size_t type_end = json_line.find("\"", type_pos);
        if (type_pos != std::string::npos && type_end > type_pos) {
            std::string msg_type = json_line.substr(type_pos, type_end - type_pos);

            JsonMessage msg;
            msg.type = msg_type;
            msg.from = c->name;

            // Извлекаем text
            size_t text_pos = json_line.find("\"text\":\"") + 8;
            size_t text_end = json_line.find("\"", text_pos);
            if (text_pos != std::string::npos && text_end > text_pos) {
                msg.text = json_line.substr(text_pos, text_end - text_pos);
                msg.timestamp = get_timestamp();
                process_message(c, msg.text);
            }
        }
    }
}

int main() {
    setlocale(LC_ALL, "Russian");

    // Логирование
    logfile.open("chat.log", std::ios::app | std::ios::out);
    log("=== SERVER STARTED ===");

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        log("WSAStartup failed");
        return 1;
    }

    SOCKET serverSock = socket(AF_INET, SOCK_STREAM, 0);
    if (serverSock == INVALID_SOCKET) {
        log("socket failed");
        return 1;
    }

    int opt = 1;
    setsockopt(serverSock, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(8080);

    if (bind(serverSock, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR ||
        listen(serverSock, SOMAXCONN) == SOCKET_ERROR) {
        log("bind/listen failed: " + std::to_string(WSAGetLastError()));
        return 1;
    }

    log("Chat server: 127.0.0.1:8080 (JSON protocol)");

    while (true) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(serverSock, &readfds);

        SOCKET maxSock = serverSock;

        std::vector<std::shared_ptr<Client>> active_clients;
        {
            std::lock_guard<std::mutex> lock(clients_mutex);
            for (auto& c : clients) {
                if (c->socket != INVALID_SOCKET) {
                    FD_SET(c->socket, &readfds);
                    if (c->socket > maxSock) maxSock = c->socket;
                    active_clients.push_back(c);
                }
            }
        }

        timeval timeout{ 0, 100000 };
        int sel = select((int)maxSock + 1, &readfds, nullptr, nullptr, &timeout);

        if (sel == SOCKET_ERROR) continue;

        if (FD_ISSET(serverSock, &readfds)) {
            SOCKET clientSock = accept(serverSock, nullptr, nullptr);
            if (clientSock != INVALID_SOCKET) {
                setTcpKeepalive(clientSock);
                u_long mode = 1;
                ioctlsocket(clientSock, FIONBIO, &mode);

                auto new_client = std::make_shared<Client>(clientSock);
                {
                    std::lock_guard<std::mutex> lock(clients_mutex);
                    clients.push_back(new_client);
                }

                log("CONNECT: " + new_client->name);
                safeBroadcast(">>> " + new_client->name + " connected\n");
            }
        }

        std::vector<std::shared_ptr<Client>> to_drop;
        for (auto& c : active_clients) {
            if (FD_ISSET(c->socket, &readfds)) {
                char buf[4096];
                int r = recv(c->socket, buf, sizeof(buf) - 1, 0);

                if (r <= 0) {
                    int err = WSAGetLastError();
                    if (err != WSAEWOULDBLOCK) {
                        to_drop.push_back(c);
                    }
                }
                else {
                    buf[r] = 0;
                    c->inbuf += buf;
                    processMessages(c);
                }
            }
        }

        for (auto& c : to_drop) {
            safeDropClient(c);
        }
    }
    return 0;
}