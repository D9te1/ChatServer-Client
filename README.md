# TCP Chat Server

Простой многопоточный чат-сервер на C++ с клиентом.

## Особенности
- TCP сокеты (Windows/Linux)
- JSON сообщения с разрезкой по `\n`
- Rate limiting (5 сообщений/сек)
- Поддержка множества клиентов
- Логирование и отладка

## Технологии
- **C++17** (threads, shared_ptr, chrono)
- **Winsock2** / **POSIX sockets** (кроссплатформенный)
- **std::regex** (парсинг JSON)
- **std::jthread** (авто-джойн потоки)

## Запуск
```bash
# Windows
g++ -std=c++17 -pthread server.cpp -o server.exe -lws2_32
server.exe 8080

# Linux/Mac
g++ -std=c++17 -pthread server.cpp -o server
./server 8080
