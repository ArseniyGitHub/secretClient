#include <SFML/Network.hpp>
#include <SFML/System.hpp>
#include <string>
#include <vector>
#include <thread>
#include <map>
#include <unordered_map>
#include <mutex>
#include <queue>
#include <variant>
#include <atomic>
#include <iostream>
#include <format> // Добавлен заголовочный файл для std::format
#include <stdio.h>
#include <stdlib.h>
#include <functional>
#include <nlohmann/json.hpp>
#include <Windows.h>
#include <fstream>
#include <math.h>

#define FILE_CHUNK_SIZE 1024 * 1024

size_t min(size_t n1, size_t n2) { return n1 < n2 ? n1 : n2; }
size_t max(size_t n1, size_t n2) { return n1 > n2 ? n1 : n2; }

using json = nlohmann::json;

std::vector<std::string> chunkedFile(std::string fileDir) {
    std::vector<std::string> ret;
    std::string buffer;
    std::ifstream file(fileDir.c_str(), std::ios::binary | std::ios::ate);
    if (!file) return ret;
    size_t fileSize = file.tellg();
    file.seekg(0);

    size_t realSize = 0;
    for (size_t offset = 0; offset < fileSize; offset += FILE_CHUNK_SIZE) {
        realSize = min(FILE_CHUNK_SIZE, fileSize - offset);
        buffer.resize(realSize);
        file.read(buffer.data(), realSize);
        ret.push_back(buffer);
    }
    file.close();
    return ret;
}

template <typename ret_type, typename... args_type> using fn = ret_type(*)(args_type...);

class Server;
class Client;

template <typename T = std::string>
class ClientsQueue {
private:
    std::queue<T> queue;
    std::mutex queueMutex;

public:
    void push(const T& msg) {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.push(msg);
    }

    void push(std::queue<T>& msg) {
        std::lock_guard<std::mutex> lock(queueMutex);
        while (!msg.empty()) {
            queue.push(msg.front());
            msg.pop();
        }
    }

    void push(const std::vector<T>& msg) {
        std::lock_guard<std::mutex> lock(queueMutex);
        for (size_t i = 0; i < msg.size(); i++)
            queue.push(msg[i]);
    }

    T& front() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return queue.front();
    }

    void pop() {
        std::lock_guard<std::mutex> lock(queueMutex);
        queue.pop();
    }

    bool empty() {
        std::lock_guard<std::mutex> lock(queueMutex);
        return queue.empty();
    }

    auto& getMutex() { return queueMutex; }
    auto& getQueue() { return queue; }
};

class Client {
public:
    using processFunctionType = fn<void, const std::string&, Client*>;

    sf::TcpSocket* socket;
    Server* server = nullptr;

    ClientsQueue<> output;
    ClientsQueue<>  input;
    std::thread outputThread;
    std::thread  inputThread;
    std::atomic<bool> active;

    std::vector<std::string> buffer;
    std::mutex bufferMutex;
    std::thread processThread;
    processFunctionType processFunction;
    std::vector<std::string> receivingFiles;

    void start();
    void stop();

    Client(Server* s, sf::TcpSocket* sock, processFunctionType f);
    Client();
    ~Client();
};

class Server {
public:
    std::thread mainThread;
    std::thread stopThread;
    std::vector<Client*> clients;
    std::mutex clientsMutex;
    std::atomic<bool> active;
    sf::TcpListener listener;
    unsigned short port;
    sf::IpAddress ip;
    Client::processFunctionType hClientFunction;
    ClientsQueue<Client*> stopQueue;

    struct ServerCantListenPort : std::exception {
        const char* what() {
            return "Server cant start listener for accepting the clients! Try use any port.";
        }
    };

    void deleteClient(Client* ptr);
    void start();
    void stop();

    ~Server();
    Server(unsigned short port, Client::processFunctionType hClient, sf::IpAddress ip = sf::IpAddress::Any);
    Server() : ip(sf::IpAddress::Any) {}
};

// Реализация методов Client после определения Server
void Client::start() {
    active = true;
    outputThread = std::thread([&, this]() {
        sf::Socket::Status st;
        while (active.load()) {
            while (output.empty() && active.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (!active.load()) break;

            sf::Packet p;  p << output.front();  output.pop();

        sending:
            st = socket->send(p);
            if (st != sf::Socket::Status::Done) {
                if (st == sf::Socket::Status::Disconnected) {
                    server->stopQueue.push(this);
                    break;
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    goto sending;
                }
            }

        }
        });

    inputThread = std::thread([&, this]() {
        sf::Socket::Status st;
        std::string str;
        while (active.load()) {
            sf::Packet p;

        receiving:
            if (!active.load()) break;

            st = socket->receive(p);
            if (st != sf::Socket::Status::Done) {
                if (st == sf::Socket::Status::Disconnected) {
                    server->stopQueue.push(this);
                    break;
                }
                else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    goto receiving;
                }
            }

            p >> str;  input.push(str);
        }
        });

    processThread = std::thread([&, this]() {
        while (active.load()) {
            while (active.load() && input.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!active.load()) break;

            std::string msg = input.front();  input.pop();
            processFunction(msg, this);
        }
        });
}

void Client::stop() {
    if (active.load()) {
        active = false;
        socket->disconnect();
        outputThread.join();
        inputThread.join();
        processThread.join();
    }
}

Client::Client(Server* s, sf::TcpSocket* sock, processFunctionType f) : server(s), socket(sock), processFunction(f) {
    start();
}

Client::Client() : active(false) {}

Client::~Client() {
    stop();
    delete socket;
}

// Реализация методов Server
void Server::deleteClient(Client* ptr) {
    std::lock_guard<std::mutex> lock(clientsMutex);
    for (size_t i = 0; i < clients.size(); i++)
        if (clients[i] == ptr) {
            clients.erase(clients.begin() + i);
            break;
        }
    delete ptr;
}

void Server::start() {
    active = true;
    mainThread = std::thread([&]() {
        if (listener.listen(port, ip) != sf::Socket::Status::Done)
            throw ServerCantListenPort();
        while (active.load()) {
            sf::TcpSocket* client = new sf::TcpSocket();
        accepting:
            if (!active.load()) {
                delete client;
                break;
            }
            if (listener.accept(*client) == sf::Socket::Status::Done) {
                std::lock_guard<std::mutex> lock(clientsMutex);
                clients.push_back(new Client(this, client, hClientFunction));
            }
            else goto accepting;
        }
        });

    stopThread = std::thread([&]() {
        while (active.load()) {
            while (active.load() && stopQueue.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
            if (!active.load()) break;

            Client* cl = stopQueue.front();  stopQueue.pop();
            deleteClient(cl);
        }
        // Очистка оставшихся клиентов
        std::lock_guard<std::mutex> lock(clientsMutex);
        for (auto* client : clients) {
            client->stop();
            delete client;
        }
        clients.clear();
        });
}

void Server::stop() {
    if (active.load()) {
        active = false;
        listener.close(); // Закрываем listener для выхода из accept
        mainThread.join();
        stopThread.join();
    }
}

Server::~Server() {
    stop();
}

Server::Server(unsigned short port, Client::processFunctionType hClient, sf::IpAddress ip)
    : port(port), hClientFunction(hClient), ip(ip) {
    start();
}

void clientHandler(const std::string& str, Client* cl) {
    try {
        json answer;
        if (str.size() >= 3 &&
            static_cast<unsigned char>(str[0]) == 0xEF &&
            static_cast<unsigned char>(str[1]) == 0xBB &&
            static_cast<unsigned char>(str[2]) == 0xBF) {
            answer = json::parse(str.substr(3));
        }
        else {
            answer = json::parse(str);
        }
        if (!answer.contains("action") || !answer.contains("message")) {
            std::cerr << std::format("from client {}:{} - no module action/message in json. Packet:\n{}\n", cl->socket->getRemoteAddress().value().toString(), cl->socket->getRemotePort(), str);
            return;
        }
        std::string action = answer["action"];
        if (action == "cmdlog") {
            std::cout << std::format("from client {}:{} - Command executing result (output):\n{}\n", cl->socket->getRemoteAddress().value().toString(), cl->socket->getRemotePort(), answer["message"].get<std::string>());
        }
        else if (action == "recvfile") {
            std::string fileChunk = answer["message"];
            if (!answer.contains("isfinal") || !answer.contains("path")) {
                std::cerr << std::format("from client {}:{} - no module isfinal/path in json. Packet:\n{}\n", cl->socket->getRemoteAddress().value().toString(), cl->socket->getRemotePort(), str);
                return;
            }
            bool isFinal = answer["isfinal"];

            bool found = false;
            std::string filename = answer["path"];
            for (size_t i = 0; i < cl->receivingFiles.size(); i++) {
                if (cl->receivingFiles[i] == filename) {
                    found = true;
                    if (isFinal) cl->receivingFiles.erase(cl->receivingFiles.begin() + i);
                    break;
                }
            }

            if (!found) {
                std::cerr << std::format("from client {}:{} - file {} not found in waiting list in json. Packet:\n{}\n", cl->socket->getRemoteAddress().value().toString(), cl->socket->getRemotePort(), filename, str);
            }
            cl->buffer.push_back(fileChunk);
            if (isFinal) {
                std::ofstream file(filename);
                for (auto& e : cl->buffer) {
                    file << e;
                }
                cl->buffer.clear();
                file.close();
                std::cout << std::format("from client {}:{} - file receiving is succesful in {}\n", cl->socket->getRemoteAddress().value().toString(), cl->socket->getRemotePort(), filename);
            }
            
        }
    }
    catch (const std::exception& e) {
        std::cerr << std::format("from client {}:{} - cant parse as json. Error:\n{}\nPacket:\n{}\n", cl->socket->getRemoteAddress().value().toString(), cl->socket->getRemotePort(), e.what(), str);
        return;
    }
}

struct _base_machine {
    const char* name;
    virtual const std::string& cmd(const std::string& str) = 0;
};


template <typename T>
struct command {
    const char* cmd;
    T function;
};

constexpr size_t strsize(const char* str) { for (size_t i = 0; true; i++) if (str[i] == 0) return i; }



template <typename T>
void cmd(const std::string& request, const std::vector<command<T>>& cmds) {
    for (auto& [name, func] : cmds) {
        if (request.find(name) == 0) {
            std::string req = request.substr(strsize(name) + 1);
            func(req);
        }
    }
}

class ServerTerminal {
public:

    static void select(const std::string& str, ServerTerminal* terminal) {
        try {
            size_t _ = std::stoull(str);
            if (_ == 0 || terminal->server->clients.size() < _) std::cerr << std::format("invalid client id {}\n", _);
            else {
                terminal->selected = _;
                std::cout << std::format("selected client: {}\n", _);
            }
        }
        catch (const std::exception& e) {
            std::cerr << std::format("something went wrong: {}\n", e.what());
        }
    }

    static void exit(const std::string& str, ServerTerminal* terminal) {
        *terminal->active = false;
        terminal->server->stop();
        std::cout << "shutdowning the server...\n";
    }

    static void message(const std::string& str, ServerTerminal* terminal) {
        if (terminal->selected == 0) {
            std::cerr << "no client selected!\n";
            return;
        }
        
        json packet = {
            {"action", "message"},
            {"message", str}
        };
        std::string file = packet.dump();

        terminal->server->clients[terminal->selected - 1]->output.push(file);
        std::cout << "succesful!\n";
    }

    static void cmd(const std::string& str, ServerTerminal* terminal) {
        if (terminal->selected == 0) {
            std::cerr << "no client selected!\n";
            return;
        }

        json packet = {
            {"action", "cmd"},
            {"message", str}
        };
        std::string file = packet.dump();
        terminal->server->clients[terminal->selected - 1]->output.push(file);
        std::cout << "sent! please, wait result of executing.\n";
    }

    static void clients(const std::string& str, ServerTerminal* terminal) {
        std::cout << "connected clients: \n";
        auto& v = terminal->server->clients;
        for (size_t i = 0; i < v.size(); i++) {
            std::cout << std::format(" [{}] - {}:{}\n", i + 1, v[i]->socket->getRemoteAddress().value().toString(), v[i]->socket->getRemotePort());
        }
    }

    static void recvfile(const std::string& str, ServerTerminal* terminal) {
        if (terminal->selected == 0) {
            std::cerr << "no client selected!\n";
            return;
        }

        std::string homepath, endpath;
        std::cout << "enter homepath (on client device): ";
        std::getline(std::cin, homepath);
        std::cout << "enter end (on server device): ";
        std::getline(std::cin, endpath);


        json req = {
            {"action", "recvfile"},
            {"message", homepath},
            {"path", endpath}
        };

        terminal->server->clients[terminal->selected - 1]->receivingFiles.push_back(endpath);
        terminal->server->clients[terminal->selected - 1]->output.push(req.dump());
    }

    static void sendfile(const std::string& str, ServerTerminal* terminal) {
        if (terminal->selected == 0) {
            std::cerr << "no client selected!\n";
            return;
        }

        std::string homepath, endpath;
        std::cout << "enter homepath (on server device): ";
        std::getline(std::cin, homepath);
        std::cout << "enter end (on client device): ";
        std::getline(std::cin, endpath);

        auto& client = *terminal->server->clients[terminal->selected - 1];
        auto chunked = chunkedFile(homepath);
        
        {
            std::lock_guard<std::mutex> lock(client.output.getMutex());
            for (size_t i = 0; i < chunked.size(); i++) {
                json req = {
                    {"action", "sendfile"},
                    {"message", chunked[i]},
                    {"path", endpath},
                    {"isfinal", (i + 1 == chunked.size())}
                };
                client.output.getQueue().push(req.dump());
            }
        }
        
        std::cout << "file sent!\n";
    }

    Server* server;
    size_t selected;
    bool* active;

    using func = fn<void, const std::string&, ServerTerminal*>;
    std::vector<command<func>> commands = {
        {"select",   select},
        {"message",  message},
        {"exit",     exit},
        {"cmd",      cmd},
        {"clients",  clients},
        {"recv",     recvfile},
        {"send",     sendfile}
    };

    void processCommand(const std::string& cmd) {
        bool processed = false;
        for (auto& [name, func] : commands) {
            if (cmd.find(name) == 0) {
                std::string req = cmd.substr(strsize(name));
                while (req[0] == ' ') req = req.substr(1);
                processed = true;
                func(req, this);
            }
        }
        if (!processed) std::cerr << "Incorrect command!\n";
    }
};

int main() {

    //   запускаем сервер
    Server* server = new Server(8080, clientHandler);
    size_t selected = 0;
    bool active = true;
    ServerTerminal terminal;
    terminal.server = server;
    terminal.active = &active;
    system("chcp 65001");

    //   основной цикл работы консоли
    std::string input;
    while (active) {
        std::cout << "> ";
        std::getline(std::cin, input);
        terminal.processCommand(input);
    }
}