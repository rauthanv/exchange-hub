
#include "common.hpp"

#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <atomic>
#include <csignal>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <vector>
#include <sstream>
#include <algorithm>

using namespace ExchangeHub;

constexpr int PORT = 8080;

const fs::path STORAGE = "storage";
const fs::path PARTIALS = STORAGE / ".partials";
const fs::path INDEX_FILE = STORAGE / ".file_index.tsv";
const fs::path HISTORY_FILE = STORAGE / ".history.tsv";

struct ChatClient {
    int socket;
    std::string username;
};

struct SharedFile {
    std::string name;
    uint64_t size;
    std::string uploader;
};

std::vector<ChatClient> chatClients;
std::vector<std::string> activityLog;

std::mutex chatMutex;
std::mutex logMutex;
std::mutex fileMutex;

std::atomic<int> activeTransfers{0};
std::atomic<uint64_t> bytesReceived{0};
std::atomic<uint64_t> bytesSent{0};
std::atomic<uint64_t> completedUploads{0};
std::atomic<uint64_t> completedDownloads{0};
std::atomic<uint64_t> interruptedUploads{0};

// --------------------------------------------------
// TIMESTAMP AND SERVER LOGGING
// --------------------------------------------------

std::string timestamp() {
    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);

    std::tm localTime{};
    localtime_r(&time, &localTime);

    std::ostringstream output;
    output << std::put_time(&localTime, "%H:%M:%S");

    return output.str();
}

void logEvent(const std::string& message) {
    std::lock_guard<std::mutex> lock(logMutex);

    std::string entry =
        "[" + timestamp() + "] " + message;

    activityLog.push_back(entry);

    if (activityLog.size() > 8)
        activityLog.erase(activityLog.begin());

    std::cout << entry << std::endl;
}

// --------------------------------------------------
// FILE PATH HELPERS
// --------------------------------------------------

fs::path userPartialDirectory(const std::string& id) {
    return PARTIALS / id;
}

fs::path partialPath(const std::string& id,
                     const std::string& name) {
    return userPartialDirectory(id) / (name + ".part");
}

fs::path partialInfoPath(const std::string& id,
                         const std::string& name) {
    return userPartialDirectory(id) / (name + ".meta");
}

// --------------------------------------------------
// TRANSFER HISTORY
// --------------------------------------------------

void recordHistory(const std::string& id,
                   const std::string& type,
                   const std::string& name,
                   uint64_t size,
                   const std::string& status) {

    std::lock_guard<std::mutex> lock(fileMutex);

    std::ofstream file(HISTORY_FILE, std::ios::app);

    if (file) {
        file << id << '\t' << type << '\t' << name << '\t'
             << size << '\t' << status << '\t'
             << timestamp() << '\n';
    }
}

// --------------------------------------------------
// SHARED FILE LIBRARY
// --------------------------------------------------

std::vector<SharedFile> loadSharedFiles() {
    std::vector<SharedFile> files;

    std::lock_guard<std::mutex> lock(fileMutex);

    std::ifstream index(INDEX_FILE);
    std::string line;

    while (std::getline(index, line)) {
        std::istringstream row(line);

        SharedFile file{};

        if (std::getline(row, file.name, '\t') &&
            row >> file.size) {

            row.ignore(1, '\t');
            std::getline(row, file.uploader);

            if (fs::exists(STORAGE / file.name))
                files.push_back(file);
        }
    }

    // Include older files that existed before the index was added.
    for (const auto& entry : fs::directory_iterator(STORAGE)) {
        if (!entry.is_regular_file())
            continue;

        std::string name = entry.path().filename().string();

        if (name.empty() || name[0] == '.' ||
            (name.size() >= 5 &&
             name.substr(name.size() - 5) == ".part")) {
            continue;
        }

        auto found = std::find_if(
            files.begin(), files.end(),
            [&](const SharedFile& f) {
                return f.name == name;
            }
        );

        if (found == files.end()) {
            files.push_back({
                name,
                entry.file_size(),
                "Unknown"
            });
        }
    }

    return files;
}

void sendFileLibrary(int client) {
    auto files = loadSharedFiles();

    if (!sendLine(client, "FILES " + std::to_string(files.size())))
        return;

    for (const auto& file : files) {
        if (!sendLine(
                client,
                file.name + "\t" +
                std::to_string(file.size) + "\t" +
                file.uploader)) {
            return;
        }
    }
}

// --------------------------------------------------
// UPLOAD HISTORY REQUEST
// --------------------------------------------------

void sendUploadHistory(int client, const std::string& id) {
    std::vector<std::string> rows;

    {
        std::lock_guard<std::mutex> lock(fileMutex);

        std::ifstream file(HISTORY_FILE);
        std::string line;

        while (std::getline(file, line)) {
            std::istringstream row(line);
            std::string storedId;

            if (!std::getline(row, storedId, '\t'))
                continue;

            if (storedId == id)
                rows.push_back(line.substr(storedId.size() + 1));
        }
    }

    sendLine(client, "HISTORY " + std::to_string(rows.size()));

    for (const auto& row : rows)
        sendLine(client, row);
}

// --------------------------------------------------
// PAUSED UPLOAD REQUEST
// --------------------------------------------------

void sendPausedUploads(int client, const std::string& id) {
    std::vector<std::string> rows;

    fs::path directory = userPartialDirectory(id);

    if (fs::exists(directory)) {
        for (const auto& entry : fs::directory_iterator(directory)) {
            if (!entry.is_regular_file())
                continue;

            std::string filename = entry.path().filename().string();

            if (filename.size() < 5 ||
                filename.substr(filename.size() - 5) != ".part")
                continue;

            std::string name =
                filename.substr(0, filename.size() - 5);

            uint64_t partialSize = entry.file_size();
            uint64_t totalSize = partialSize;
            std::string username = "Unknown";

            std::ifstream meta(partialInfoPath(id, name));

            if (meta) {
                meta >> totalSize;
                meta >> username;
            }

            rows.push_back(
                name + "\t" + std::to_string(totalSize) + "\t" +
                std::to_string(partialSize) + "\t" + username
            );
        }
    }

    sendLine(client, "PAUSED " + std::to_string(rows.size()));

    for (const auto& row : rows)
        sendLine(client, row);
}

// --------------------------------------------------
// UPLOAD
// --------------------------------------------------


void handleUpload(int client,
                  const std::string& id,
                  const std::string& username,
                  const std::string& name,
                  uint64_t totalSize) {

    logEvent("[UPLOAD STARTED] " + username +
             " | " + name +
             " | " + formatBytes(totalSize));

    fs::path finalPath = STORAGE / name;
    fs::path part = partialPath(id, name);
    fs::path meta = partialInfoPath(id, name);

    fs::create_directories(userPartialDirectory(id));

    {
        std::lock_guard<std::mutex> lock(fileMutex);

        if (fs::exists(finalPath)) {
            sendLine(client, "ERROR A file with this name already exists");

            logEvent("[UPLOAD REJECTED] " + username +
                     " | " + name + " | File already exists");
            return;
        }
    }

    if (fs::exists(part) && fs::file_size(part) > totalSize)
        fs::remove(part);

    uint64_t offset = fs::exists(part) ? fs::file_size(part) : 0;

    {
        std::ofstream info(meta);
        info << totalSize << '\n' << username << '\n';
    }

    if (!sendLine(client, "READY " + std::to_string(offset)))
        return;

    if (offset > 0) {
        logEvent("[UPLOAD RESUMED] " + username +
                 " | " + name +
                 " | Starting from " + formatBytes(offset));
    }

    std::ofstream output(
        part,
        std::ios::binary | std::ios::app
    );

    if (!output) {
        sendLine(client, "ERROR Cannot open partial upload");

        logEvent("[UPLOAD FAILED] " + username +
                 " | " + name + " | Cannot open partial file");
        return;
    }

    uint64_t transferred = offset;
    uint64_t remaining = totalSize - offset;

    char buffer[BUFFER_SIZE];

    while (remaining > 0) {

        size_t amount = static_cast<size_t>(
            std::min<uint64_t>(remaining, BUFFER_SIZE)
        );

        size_t receivedBytes = 0;

        bool complete = receiveExact(
            client,
            buffer,
            amount,
            &receivedBytes
        );

        // Save every byte received, even if the connection drops.
        if (receivedBytes > 0) {

            output.write(
                buffer,
                static_cast<std::streamsize>(receivedBytes)
            );

            if (!output) {
                output.close();

                recordHistory(
                    id, "UPLOAD", name, totalSize, "FAILED"
                );

                logEvent("[UPLOAD FAILED] " + username +
                         " | " + name + " | Disk write error");
                return;
            }

            transferred += receivedBytes;
            bytesReceived += receivedBytes;
        }

        // Connection interrupted before the complete chunk arrived.
        if (!complete) {

            output.close();

            interruptedUploads++;

            recordHistory(
                id, "UPLOAD", name, totalSize, "PAUSED"
            );

            logEvent("[UPLOAD PAUSED] " + username +
                     " | " + name +
                     " | " + formatBytes(transferred) +
                     " received");

            return;
        }

        remaining -= amount;
    }

    output.close();

    std::string receivedHash;

    if (!receiveLine(client, receivedHash)) {

        interruptedUploads++;

        recordHistory(
            id, "UPLOAD", name, totalSize, "PAUSED"
        );

        logEvent("[UPLOAD PAUSED] " + username +
                 " | " + name + " | Verification interrupted");
        return;
    }

    std::string actualHash = sha256File(part);

    if (actualHash.empty() || actualHash != receivedHash) {

        sendLine(client, "ERROR SHA-256 verification failed");

        recordHistory(
            id, "UPLOAD", name, totalSize, "FAILED"
        );

        logEvent("[UPLOAD FAILED] " + username +
                 " | " + name + " | SHA-256 verification failed");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(fileMutex);

        if (fs::exists(finalPath)) {

            sendLine(
                client,
                "ERROR Another user uploaded this filename first"
            );

            logEvent("[UPLOAD REJECTED] " + username +
                     " | " + name + " | Filename conflict");
            return;
        }

        std::error_code error;
        fs::rename(part, finalPath, error);

        if (error) {

            sendLine(client, "ERROR Could not finalize uploaded file");

            logEvent("[UPLOAD FAILED] " + username +
                     " | " + name +
                     " | " + error.message());
            return;
        }

        std::ofstream index(INDEX_FILE, std::ios::app);

        if (index) {
            index << name << '\t' << totalSize << '\t'
                  << username << '\n';
        }

        fs::remove(meta);

        std::ofstream history(HISTORY_FILE, std::ios::app);

        if (history) {
            history << id << "\tUPLOAD\t" << name << '\t'
                    << totalSize << "\tCOMPLETED\t"
                    << timestamp() << '\n';
        }
    }

    completedUploads++;

    sendLine(client, "OK Upload completed and SHA-256 verified");

    logEvent("[UPLOAD COMPLETED] " + username +
             " | " + name +
             " | " + formatBytes(totalSize) +
             " | SHA-256 verified");
}

// --------------------------------------------------
// DOWNLOAD
// --------------------------------------------------

void handleDownload(int client,
                    const std::string& id,
                    const std::string& name,
                    uint64_t requestedOffset) {

    fs::path filePath = STORAGE / name;

    if (!fs::exists(filePath) || !fs::is_regular_file(filePath)) {
        sendLine(client, "ERROR File not found");

        logEvent("[DOWNLOAD FAILED] " + name +
                 " | File not found");
        return;
    }

    uint64_t totalSize = fs::file_size(filePath);

    uint64_t offset = requestedOffset <= totalSize
        ? requestedOffset : 0;

    std::string hash = sha256File(filePath);

    if (hash.empty()) {
        sendLine(client, "ERROR Cannot calculate file hash");

        logEvent("[DOWNLOAD FAILED] " + name +
                 " | Could not calculate SHA-256");
        return;
    }

    std::string clientLabel =
        id.substr(0, std::min<size_t>(8, id.size()));

    logEvent("[DOWNLOAD STARTED] " + name +
             " | Client ID: " + clientLabel +
             " | " + formatBytes(totalSize));

    if (!sendLine(
            client,
            "FILE " + std::to_string(totalSize) + " " +
            std::to_string(offset))) {
        logEvent("[DOWNLOAD INTERRUPTED] " + name +
                 " | Could not send file information");
        return;
    }

    if (!sendFileRange(client, filePath, offset)) {
        logEvent("[DOWNLOAD INTERRUPTED] " + name +
                 " | Client ID: " + clientLabel);
        return;
    }

    if (!sendLine(client, hash)) {
        logEvent("[DOWNLOAD INTERRUPTED] " + name +
                 " | Hash transmission failed");
        return;
    }

    bytesSent += totalSize - offset;
    completedDownloads++;

    recordHistory(
        id, "DOWNLOAD", name, totalSize, "SERVED"
    );

    logEvent("[DOWNLOAD COMPLETED] " + name +
             " | Client ID: " + clientLabel +
             " | " + formatBytes(totalSize));
}

// --------------------------------------------------
// DELETE PAUSED UPLOAD
// --------------------------------------------------

void handleDeleteUpload(int client,
                        const std::string& id,
                        const std::string& name) {

    fs::path part = partialPath(id, name);
    fs::path meta = partialInfoPath(id, name);

    if (!fs::exists(part)) {
        sendLine(client, "ERROR Paused upload not found");
        return;
    }

    std::error_code error;

    fs::remove(part, error);
    fs::remove(meta);

    if (error) {
        sendLine(client, "ERROR Could not delete partial upload");

        logEvent("[UPLOAD DELETE FAILED] " + name +
                 " | " + error.message());
        return;
    }

    sendLine(client, "OK Paused upload deleted");

    logEvent("[PAUSED UPLOAD DELETED] " + name +
             " | Client ID: " +
             id.substr(0, std::min<size_t>(8, id.size())));
}

// --------------------------------------------------
// GROUP CHAT
// --------------------------------------------------

void broadcastChat(const std::string& message) {
    std::lock_guard<std::mutex> lock(chatMutex);

    for (auto& client : chatClients)
        sendLine(client.socket, message);

    logEvent("[CHAT MESSAGE] " + message);
}

void removeChatClient(int socket, const std::string& username) {
    {
        std::lock_guard<std::mutex> lock(chatMutex);

        chatClients.erase(
            std::remove_if(
                chatClients.begin(), chatClients.end(),
                [socket](const ChatClient& client) {
                    return client.socket == socket;
                }
            ),
            chatClients.end()
        );
    }

    close(socket);

    broadcastChat("[SYSTEM] " + username + " left the chat.");
}

void handleChat(int client, const std::string& username) {
    if (!validUsername(username)) {
        sendLine(client, "ERROR Invalid username");
        close(client);
        return;
    }

    {
        std::lock_guard<std::mutex> lock(chatMutex);
        chatClients.push_back({client, username});
    }

    sendLine(client, "CHAT_OK");

    logEvent("[CHAT USER JOINED] " + username);

    broadcastChat("[SYSTEM] " + username + " joined the chat.");

    std::string message;

    while (receiveLine(client, message)) {
        if (message == "/exit")
            break;

        if (message.empty())
            continue;

        if (message.size() > 1000) {
            sendLine(client, "[SYSTEM] Message too long.");
            continue;
        }

        broadcastChat(username + ": " + message);
    }

    removeChatClient(client, username);

    logEvent("[CHAT USER DISCONNECTED] " + username);
}

// --------------------------------------------------
// CLIENT REQUEST HANDLER
// --------------------------------------------------

void handleClient(int client) {
    std::string request;

    if (!receiveLine(client, request)) {
        close(client);
        return;
    }

    std::istringstream input(request);

    std::string command, id, username, name;

    uint64_t sizeOrOffset = 0;

    input >> command;

    if (command == "CHAT") {
        input >> username;

        handleChat(client, username);
        return;
    }

    if (command == "LIST" || command == "HISTORY" ||
        command == "PAUSED") {

        input >> id;

        if (!validClientId(id)) {
            sendLine(client, "ERROR Invalid client ID");
        }
        else if (command == "LIST") {
            sendFileLibrary(client);
        }
        else if (command == "HISTORY") {
            sendUploadHistory(client, id);
        }
        else {
            sendPausedUploads(client, id);
        }

        close(client);
        return;
    }

    if (command == "DELETE_UPLOAD") {
        input >> id >> name;

        if (!validClientId(id) || !validFilename(name))
            sendLine(client, "ERROR Invalid request");
        else
            handleDeleteUpload(client, id, name);

        close(client);
        return;
    }

    if (command == "UPLOAD") {
        input >> id >> username >> name >> sizeOrOffset;

        if (!input || !validClientId(id) ||
            !validUsername(username) || !validFilename(name)) {

            sendLine(client, "ERROR Invalid upload request");

            close(client);
            return;
        }

        activeTransfers++;

        handleUpload(
            client, id, username, name, sizeOrOffset
        );

        activeTransfers--;

        close(client);
        return;
    }

    if (command == "DOWNLOAD") {
        input >> id >> name >> sizeOrOffset;

        if (!input || !validClientId(id) ||
            !validFilename(name)) {

            sendLine(client, "ERROR Invalid download request");

            close(client);
            return;
        }

        activeTransfers++;

        handleDownload(
            client, id, name, sizeOrOffset
        );

        activeTransfers--;

        close(client);
        return;
    }

    sendLine(client, "ERROR Unknown command");

    close(client);
}

// --------------------------------------------------
// MAIN SERVER
// --------------------------------------------------

int main() {
    signal(SIGPIPE, SIG_IGN);

    fs::create_directories(STORAGE);
    fs::create_directories(PARTIALS);

    int serverSocket = socket(AF_INET, SOCK_STREAM, 0);

    if (serverSocket == -1) {
        std::cerr << "Socket creation failed\n";
        return 1;
    }

    int option = 1;

    setsockopt(
        serverSocket,
        SOL_SOCKET,
        SO_REUSEADDR,
        &option,
        sizeof(option)
    );

    sockaddr_in address{};

    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);
    address.sin_addr.s_addr = INADDR_ANY;

    if (bind(serverSocket, (sockaddr*)&address,
             sizeof(address)) == -1) {

        std::cerr << "Bind failed. Is port 8080 already in use?\n";

        close(serverSocket);
        return 1;
    }

    if (listen(serverSocket, 20) == -1) {
        std::cerr << "Listen failed\n";

        close(serverSocket);
        return 1;
    }

    std::cout << "\n";
    std::cout << "============================================\n";
    std::cout << "             EXCHANGEHUB SERVER                 \n";
    std::cout << "============================================\n";

    std::cout << "  Status       : RUNNING\n";
    std::cout << "  Port         : " << PORT << '\n';
    std::cout << "  Protocol     : TCP / IPv4\n";
    std::cout << "  Storage      : " << STORAGE << '\n';
    std::cout << "  Integrity    : SHA-256\n";

    std::cout << "============================================\n";
    std::cout << "  SERVER ACTIVITY LOG\n";
    std::cout << "============================================\n\n";

    logEvent("[SERVER STARTED] Listening on port " +
             std::to_string(PORT));

    while (true) {
        int client = accept(serverSocket, nullptr, nullptr);

        if (client == -1)
            continue;

        // Socket numbers are intentionally hidden from normal logs.
        std::thread(handleClient, client).detach();
    }

    close(serverSocket);

    return 0;
}
