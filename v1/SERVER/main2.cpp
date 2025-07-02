#define _WIN32_WINNT 0x0601  // Windows 7 и выше

#include <boost/asio.hpp>
#include <boost/uuid/uuid.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <boost/lexical_cast.hpp>
#include <thread>
#include <mutex>
#include <queue>
#include <atomic>
#include <memory>
#include <iostream>
#include <unordered_map>
#include <functional>
#include <condition_variable>
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <optional>
#include <random>

using json = nlohmann::json;
namespace asio = boost::asio;
namespace fs = std::filesystem;
using asio::ip::tcp;
using namespace std::chrono_literals;

// Класс для безопасных очередей пакетов
template <typename T>
class ProtectedQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(mutex_);
        queue_.push(std::move(value));
        cond_.notify_one();
    }

    bool try_pop(T& value) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cond_.wait_for(lock, 100ms, [this] { return !queue_.empty(); })) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop();
        return true;
    }

    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.empty();
    }

private:
    std::queue<T> queue_;
    mutable std::mutex mutex_;
    std::condition_variable cond_;
};

// Класс для потоковой передачи файлов
class FileTransfer {
public:
    struct FileInfo {
        std::string uuid;
        fs::path path;
        std::fstream file;
        size_t total_size = 0;
        size_t received = 0;
    };

    void start_reception(const std::string& uuid, const fs::path& path, size_t total_size) {
        std::lock_guard<std::mutex> lock(mutex_);
        active_transfers_[uuid] = FileInfo{
            uuid,
            path,
            std::fstream(),
            total_size,
            0
        };
        active_transfers_[uuid].file.open(path, std::ios::binary | std::ios::out);
    }

    bool write_chunk(const std::string& uuid, const char* data, size_t size) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = active_transfers_.find(uuid);
        if (it == active_transfers_.end()) return false;

        auto& transfer = it->second;
        transfer.file.write(data, size);
        transfer.received += size;

        if (transfer.received >= transfer.total_size) {
            transfer.file.close();
            completed_transfers_.push(transfer.path);
            active_transfers_.erase(it);
        }
        return true;
    }

    std::optional<fs::path> next_completed() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (completed_transfers_.empty()) return std::nullopt;
        auto path = completed_transfers_.front();
        completed_transfers_.pop();
        return path;
    }

    void cancel(const std::string& uuid) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (auto it = active_transfers_.find(uuid); it != active_transfers_.end()) {
            it->second.file.close();
            fs::remove(it->second.path);
            active_transfers_.erase(it);
        }
    }

private:
    std::unordered_map<std::string, FileInfo> active_transfers_;
    std::queue<fs::path> completed_transfers_;
    mutable std::mutex mutex_;
};

// Класс клиента
class Client : public std::enable_shared_from_this<Client> {
public:
    using Pointer = std::shared_ptr<Client>;
    using Handler = std::function<void(Pointer, const std::string&)>;

    Client(tcp::socket socket, asio::io_context& io_context, Handler handler, FileTransfer& file_transfer)
        : socket_(std::move(socket)),
        io_context_(io_context),
        handler_(std::move(handler)),
        file_transfer_(file_transfer),
        read_strand_(io_context),
        write_strand_(io_context),
        active_(true) {}

    ~Client() {
        stop();
    }

    asio::ip::tcp::socket& get_socket() { return socket_; }
    asio::ip::address get_ip() const {
        if (socket_.is_open()) {
            return socket_.remote_endpoint().address();
        }
        return asio::ip::make_address("0.0.0.0");
    }
    uint16_t get_port() const {
        if (socket_.is_open()) {
            return socket_.remote_endpoint().port();
        }
        return 0;
    }

    void start() {
        receive_thread_ = std::thread([this] { process_receives(); });
        send_thread_ = std::thread([this] { process_sends(); });
        service_thread_ = std::thread([this] { process_service(); });
        async_read();
    }

    void stop() {
        if (!active_.exchange(false)) return;

        asio::post(io_context_, [this] {
            if (socket_.is_open()) {
                boost::system::error_code ec;
                socket_.close(ec);
            }
            });

        // Сигнализируем потокам о завершении
        input_queue_.push("");
        output_queue_.push("");
        service_queue_.push("");

        if (receive_thread_.joinable()) receive_thread_.join();
        if (send_thread_.joinable()) send_thread_.join();
        if (service_thread_.joinable()) service_thread_.join();
    }

    void send(const std::string& data) {
        output_queue_.push(data);
    }

    bool is_active() const { return active_; }

    uint64_t id() const { return id_; }
    void set_id(uint64_t id) { id_ = id; }

private:
    void async_read() {
        auto self(shared_from_this());
        socket_.async_read_some(asio::buffer(buffer_),
            asio::bind_executor(read_strand_,
                [this, self](boost::system::error_code ec, size_t length) {
                    if (!ec) {
                        input_queue_.push(std::string(buffer_.data(), length));
                        async_read();
                    }
                    else if (ec != asio::error::operation_aborted) {
                        stop();
                    }
                }));
    }

    void process_receives() {
        std::string accumulated;
        while (active_) {
            std::string packet;
            if (!input_queue_.try_pop(packet)) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            if (packet.empty()) break;

            accumulated += packet;
            size_t pos = 0;

            // Обработка полных JSON сообщений
            while ((pos = accumulated.find("\n\n")) != std::string::npos) {
                std::string message = accumulated.substr(0, pos);
                accumulated.erase(0, pos + 2);

                if (!message.empty()) {
                    service_queue_.push(std::move(message));
                }
            }
        }
    }

    void process_sends() {
        while (active_) {
            std::string packet;
            if (!output_queue_.try_pop(packet) || packet.empty()) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            asio::steady_timer timer(io_context_);
            timer.expires_after(5s);
            std::atomic<bool> write_complete{ false };

            auto self(shared_from_this());
            asio::async_write(socket_, asio::buffer(packet),
                asio::bind_executor(write_strand_,
                    [&](boost::system::error_code ec, size_t) {
                        if (ec && ec != asio::error::operation_aborted) {
                            stop();
                        }
                        timer.cancel();
                        write_complete = true;
                    }));

            // Ожидание завершения или таймаута
            while (!write_complete) {
                io_context_.run_one_for(100ms);
                if (timer.expiry() <= asio::steady_timer::clock_type::now()) {
                    socket_.cancel();
                    break;
                }
            }
        }
    }

    void process_service() {
        while (active_) {
            std::string packet;
            if (!service_queue_.try_pop(packet)) {
                std::this_thread::sleep_for(10ms);
                continue;
            }

            if (packet.empty()) break;
            if (handler_) handler_(shared_from_this(), packet);
        }
    }

    tcp::socket socket_;
    asio::io_context& io_context_;
    Handler handler_;
    FileTransfer& file_transfer_;
    asio::io_context::strand read_strand_;
    asio::io_context::strand write_strand_;
    std::array<char, 65536> buffer_;  // 64KB буфер
    uint64_t id_ = 0;
    std::atomic<bool> active_;

    ProtectedQueue<std::string> input_queue_;
    ProtectedQueue<std::string> output_queue_;
    ProtectedQueue<std::string> service_queue_;

    std::thread receive_thread_;
    std::thread send_thread_;
    std::thread service_thread_;
};

// Класс сервера
class Server {
public:
    using ClientHandler = typename Client::Handler;

    Server(short port, ClientHandler handler, FileTransfer& file_transfer)
        : io_context_(),
        acceptor_(io_context_, tcp::endpoint(tcp::v4(), port)),
        handler_(std::move(handler)),
        file_transfer_(file_transfer),
        active_(false),
        work_guard_(asio::make_work_guard(io_context_)) {}

    ~Server() {
        stop();
    }

    void run() {
        if (active_.exchange(true)) return;

        accept_thread_ = std::thread([this] { accept_connections(); });

        const size_t num_threads = std::thread::hardware_concurrency();
        for (size_t i = 0; i < num_threads; ++i) {
            io_threads_.emplace_back([this] { io_context_.run(); });
        }

        std::cout << "Server started on port " << port() << "\n";
    }

    void stop() {
        if (!active_.exchange(false)) return;

        asio::post(io_context_, [this] {
            boost::system::error_code ec;
            acceptor_.close(ec);
            });

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            for (auto& client : clients_) {
                client.second->stop();
            }
            clients_.clear();
        }

        work_guard_.reset();
        io_context_.stop();

        if (accept_thread_.joinable()) accept_thread_.join();
        for (auto& thread : io_threads_) {
            if (thread.joinable()) thread.join();
        }
        io_threads_.clear();
    }

    uint16_t port() const {
        return acceptor_.local_endpoint().port();
    }

    std::vector<std::shared_ptr<Client>> get_clients() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        std::vector<std::shared_ptr<Client>> result;
        for (const auto& [id, client] : clients_) {
            result.push_back(client);
        }
        return result;
    }

    std::shared_ptr<Client> get_client(uint64_t id) const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = clients_.find(id);
        return (it != clients_.end()) ? it->second : nullptr;
    }

private:
    void accept_connections() {
        while (active_) {
            tcp::socket socket(io_context_);
            boost::system::error_code ec;

            acceptor_.accept(socket, ec);

            if (ec) {
                if (ec == asio::error::operation_aborted) break;
                std::cerr << "Accept error: " << ec.message() << "\n";
                continue;
            }

            if (!active_) break;

            auto id = next_id_++;
            auto client = std::make_shared<Client>(
                std::move(socket), io_context_, handler_, file_transfer_);
            client->set_id(id);

            {
                std::lock_guard<std::mutex> lock(clients_mutex_);
                clients_.emplace(id, client);
            }

            client->start();
            std::cout << "Client connected. ID: " << id
                << " IP: " << client->get_ip().to_string()
                << ":" << client->get_port() << "\n";
        }
    }

    asio::io_context io_context_;
    tcp::acceptor acceptor_;
    ClientHandler handler_;
    FileTransfer& file_transfer_;
    std::atomic<bool> active_;

    asio::executor_work_guard<asio::io_context::executor_type> work_guard_;

    std::thread accept_thread_;
    std::vector<std::thread> io_threads_;

    std::atomic<uint64_t> next_id_{ 1 };
    std::unordered_map<uint64_t, std::shared_ptr<Client>> clients_;
    mutable std::mutex clients_mutex_;
};

ProtectedQueue<std::string> messages;
FileTransfer global_file_transfer;

// Генератор UUID с использованием Boost
std::string generate_uuid() {
    static boost::uuids::random_generator generator;
    return boost::lexical_cast<std::string>(generator());
}

// Обработчик клиента
void client_handler(Client::Pointer client, const std::string& packet) {
    try {
        json msg = json::parse(packet);

        if (!msg.contains("action")) return;
        const std::string action = msg["action"];

        if (action == "command log") {
            if (!msg.contains("message")) return;
            messages.push("Client " + std::to_string(client->id()) +
                " command log:\n" +
                msg["message"].get<std::string>());
        }
        else if (action == "file_start") {
            if (!msg.contains("uuid") || !msg.contains("size") || !msg.contains("path")) {
                return;
            }

            const std::string uuid = msg["uuid"];
            const size_t size = msg["size"];
            const std::string server_path = msg["path"];

            global_file_transfer.start_reception(uuid, server_path, size);
            messages.push("Starting file transfer: " + uuid + " -> " + server_path);
        }
        else if (action == "file_chunk") {
            if (!msg.contains("uuid") || !msg.contains("chunk")) {
                return;
            }

            const std::string uuid = msg["uuid"];
            const std::string chunk_data = msg["chunk"];

            global_file_transfer.write_chunk(uuid, chunk_data.data(), chunk_data.size());
        }
        else if (action == "file_cancel") {
            if (!msg.contains("uuid")) return;
            global_file_transfer.cancel(msg["uuid"]);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Error processing client message: " << e.what() << "\n";
    }
}

// Потоковая отправка файла
void stream_file_send(Client::Pointer client, const fs::path& server_path, const fs::path& client_path) {
    const size_t chunk_size = 64 * 1024; // 64KB
    const std::string uuid = generate_uuid();

    try {
        // Отправка метаданных файла
        json start_msg = {
            {"action", "file_start"},
            {"uuid", uuid},
            {"path", client_path.string()},
            {"size", fs::file_size(server_path)}
        };
        client->send(start_msg.dump() + "\n\n");

        // Чтение и отправка файла по частям
        std::ifstream file(server_path, std::ios::binary);
        if (!file) {
            throw std::runtime_error("Cannot open file: " + server_path.string());
        }

        std::vector<char> buffer(chunk_size);
        while (file) {
            file.read(buffer.data(), buffer.size());
            const size_t bytes_read = file.gcount();

            json chunk_msg = {
                {"action", "file_chunk"},
                {"uuid", uuid},
                {"chunk", std::string(buffer.data(), bytes_read)}
            };
            client->send(chunk_msg.dump() + "\n\n");
        }

        // Уведомление о завершении
        json complete_msg = {
            {"action", "file_complete"},
            {"uuid", uuid}
        };
        client->send(complete_msg.dump() + "\n\n");
    }
    catch (const std::exception& e) {
        json cancel_msg = { {"action", "file_cancel"}, {"uuid", uuid} };
        client->send(cancel_msg.dump() + "\n\n");
        throw;
    }
}

// Консоль управления сервером
void console_loop(Server& server) {
    std::cout << "Server management console\n";
    std::cout << "Commands: clients, select <id>, recv, send, execute, exit\n";

    uint64_t selected_id = 0;
    bool active = true;

    // Поток для обработки уведомлений
    std::atomic<bool> notification_active{ true };
    std::thread notification_thread([&] {
        while (notification_active) {
            // Обработка завершенных передач файлов
            if (auto path = global_file_transfer.next_completed()) {
                messages.push("File transfer completed: " + path->string());
            }

            // Обработка обычных уведомлений
            std::string msg;
            if (messages.try_pop(msg)) {
                std::cout << "\n[NOTIFICATION] " << msg << "\n> " << std::flush;
            }
            std::this_thread::sleep_for(100ms);
        }
        });

    while (active) {
        std::cout << "> ";
        std::string input;
        std::getline(std::cin, input);

        if (input.empty()) continue;

        if (input == "clients") {
            auto clients = server.get_clients();
            if (clients.empty()) {
                std::cout << "No connected clients\n";
            }
            else {
                for (const auto& client : clients) {
                    std::cout << "  [" << client->id() << "] "
                        << client->get_ip().to_string()
                        << ":" << client->get_port();

                    if (client->id() == selected_id) {
                        std::cout << " (selected)";
                    }
                    std::cout << "\n";
                }
            }
        }
        else if (input.rfind("select ", 0) == 0) {
            try {
                uint64_t id = std::stoull(input.substr(7));
                if (server.get_client(id)) {
                    selected_id = id;
                    std::cout << "Selected client: " << id << "\n";
                }
                else {
                    std::cerr << "Client not found\n";
                }
            }
            catch (...) {
                std::cerr << "Invalid client ID\n";
            }
        }
        else if (input == "execute") {
            if (!selected_id) {
                std::cerr << "No client selected\n";
                continue;
            }

            auto client = server.get_client(selected_id);
            if (!client) {
                std::cerr << "Client disconnected\n";
                continue;
            }

            std::cout << "Command to execute: ";
            std::string command;
            std::getline(std::cin, command);

            json cmd = {
                {"action", "command"},
                {"message", command}
            };
            client->send(cmd.dump() + "\n\n");
            std::cout << "Command sent\n";
        }
        else if (input == "exit") {
            active = false;
            std::cout << "Shutting down server...\n";
        }
        else {
            std::cerr << "Unknown command: " << input << "\n";
        }
    }

    notification_active = false;
    notification_thread.join();
}

int main() {
    try {
        Server server(8080, client_handler, global_file_transfer);
        server.run();

        console_loop(server);

        server.stop();
    }
    catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}