#include "common.hpp"
#include <poll.h>
#include <termios.h>
#include <cerrno>
#include <ctime>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <vector>
#include <iomanip>
#include <cstdlib>

using namespace ExchangeHub;

constexpr int PORT = 8080;

std::string serverIP = "127.0.0.1";
std::string clientId;
std::string username;

const fs::path DOWNLOAD_DIR = "downloads";
const fs::path UPLOAD_META_DIR = ".exchangehub_uploads";
const fs::path DOWNLOAD_HISTORY_FILE =
    ".exchangehub_download_history.tsv";

struct RemoteFile {
    std::string name;
    uint64_t size;
    std::string uploader;
};

struct PausedUpload {
    std::string name;
    uint64_t totalSize;
    uint64_t uploadedSize;
    std::string uploader;
};

int connectToServer() {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock == -1) return -1;

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(PORT);

    if (inet_pton(AF_INET, serverIP.c_str(),
                  &address.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    if (connect(sock, (sockaddr*)&address,
                sizeof(address)) == -1) {
        close(sock);
        return -1;
    }

    return sock;
}

// --------------------------------------------------
// USER IDENTITY
// --------------------------------------------------

std::string generateClientId() {
    std::random_device random;
    std::ostringstream output;

    output << std::hex
           << std::chrono::steady_clock::now()
                  .time_since_epoch().count()
           << random();

    return output.str();
}

bool initializeIdentity() {
    while (true) {
        std::cout << "Enter your username: ";
        std::getline(std::cin, username);

        if (validUsername(username))
            break;

        std::cout << UI::RED
                  << "Use 1-30 letters, numbers, _ or -.\n"
                  << UI::RESET;
    }

    // Store identities in the ExchangeHub project directory.
    fs::path identityDirectory =
        fs::path(".exchangehub") / "identities";

    std::error_code error;
    fs::create_directories(identityDirectory, error);

    if (error) {
        std::cout << "Could not create identity directory: "
                  << error.message() << '\n';
        return false;
    }

    fs::path identityFile =
        identityDirectory / (username + ".identity");

    // Read and validate an existing ExchangeHub identity.
    auto readIdentity = [](const fs::path& path,
                           std::string& result) {
        std::ifstream input(path);
        std::string candidate;

        if (input && std::getline(input, candidate) &&
            validClientId(candidate)) {
            result = candidate;
            return true;
        }

        return false;
    };

    // Reuse the current ExchangeHub identity if it exists.
    if (readIdentity(identityFile, clientId)) {
        std::cout << UI::GREEN
                  << "Existing client identity loaded.\n"
                  << UI::RESET;
        return true;
    }

    // Generate a new identity for this ExchangeHub installation.
    clientId = generateClientId();

    std::ofstream output(identityFile);

    if (!output) {
        std::cout << "Could not save client identity.\n";
        return false;
    }

    output << clientId << '\n';

    if (!output) {
        std::cout << "Failed to write client identity.\n";
        return false;
    }

    std::cout << UI::GREEN
              << "New client identity created and saved.\n"
              << UI::RESET;

    return true;
}

// --------------------------------------------------
// LOCAL UPLOAD METADATA
// --------------------------------------------------

fs::path uploadMetadataPath(const std::string& name) {
    return UPLOAD_META_DIR / (name + ".meta");
}

void saveUploadPath(const std::string& name,
                    const fs::path& originalPath) {
    fs::create_directories(UPLOAD_META_DIR);

    std::ofstream file(uploadMetadataPath(name));

    if (file)
        file << std::quoted(originalPath.string()) << '\n';
}

bool loadUploadPath(const std::string& name,
                    fs::path& originalPath) {
    std::ifstream file(uploadMetadataPath(name));

    if (!file)
        return false;

    std::string path;

    if (!(file >> std::quoted(path)))
        return false;

    originalPath = path;
    return true;
}

void removeUploadPath(const std::string& name) {
    std::error_code error;
    fs::remove(uploadMetadataPath(name), error);
}

// --------------------------------------------------
// SINGLE-LINE PROGRESS BAR
// --------------------------------------------------

void showProgress(const std::string& operation,
                  uint64_t completed,
                  uint64_t total,
                  int& previousPercentage) {

    constexpr int width = 24;

    int percentage = total == 0
        ? 100
        : static_cast<int>((100.0L * completed) / total);

    percentage = std::min(percentage, 100);

    if (percentage == previousPercentage)
        return;

    previousPercentage = percentage;

    // If output is not an interactive terminal, print only once at 100%.
    if (!isatty(STDOUT_FILENO) && percentage < 100)
        return;

    int filled = width * percentage / 100;

    // Return to the beginning and clear the existing line.
    std::cout << '\r' << "\033[2K";

    std::cout << UI::CYAN << operation << " [";

    for (int i = 0; i < width; i++) {
        std::cout << (i < filled ? "█" : "░");
    }

    std::cout << "] " << UI::RESET
              << std::setw(3) << percentage << "%"
              << std::flush;
}

void finishProgress() {
    std::cout << '\n' << std::flush;
}

// --------------------------------------------------
// LOCAL DOWNLOAD HISTORY
// --------------------------------------------------

void addLocalHistory(const std::string& type,
                     const std::string& name,
                     uint64_t size,
                     const std::string& status) {

    std::ofstream file(
        DOWNLOAD_HISTORY_FILE,
        std::ios::app
    );

    if (file) {
        file << type << '\t' << name << '\t'
             << size << '\t' << status << '\n';
    }
}

// --------------------------------------------------
// SERVER LIST REQUESTS
// --------------------------------------------------

std::vector<std::string> requestRows(
    const std::string& command,
    const std::string& expectedHeader) {

    std::vector<std::string> rows;

    int sock = connectToServer();

    if (sock == -1)
        return rows;

    if (!sendLine(sock, command + " " + clientId)) {
        close(sock);
        return rows;
    }

    std::string response;

    if (!receiveLine(sock, response)) {
        close(sock);
        return rows;
    }

    std::istringstream header(response);

    std::string label;
    size_t count = 0;

    header >> label >> count;

    if (label != expectedHeader) {
        std::cout << UI::RED << response
                  << UI::RESET << '\n';
        close(sock);
        return rows;
    }

    for (size_t i = 0; i < count; i++) {
        std::string row;

        if (!receiveLine(sock, row))
            break;

        rows.push_back(row);
    }

    close(sock);
    return rows;
}

std::vector<RemoteFile> getRemoteFiles() {
    std::vector<RemoteFile> files;

    auto rows = requestRows("LIST", "FILES");

    for (const auto& line : rows) {
        std::istringstream input(line);

        RemoteFile file{};
        std::string size;

        if (!std::getline(input, file.name, '\t'))
            continue;

        if (!std::getline(input, size, '\t'))
            continue;

        if (!std::getline(input, file.uploader))
            continue;

        try {
            file.size = std::stoull(size);
            files.push_back(file);
        }
        catch (...) {
            continue;
        }
    }

    return files;
}

std::vector<PausedUpload> getPausedUploads() {
    std::vector<PausedUpload> uploads;

    auto rows = requestRows("PAUSED", "PAUSED");

    for (const auto& line : rows) {
        std::istringstream input(line);

        PausedUpload upload{};
        std::string total, partial;

        if (!std::getline(input, upload.name, '\t') ||
            !std::getline(input, total, '\t') ||
            !std::getline(input, partial, '\t') ||
            !std::getline(input, upload.uploader)) {
            continue;
        }

        try {
            upload.totalSize = std::stoull(total);
            upload.uploadedSize = std::stoull(partial);
            uploads.push_back(upload);
        }
        catch (...) {
            continue;
        }
    }

    return uploads;
}

// --------------------------------------------------
// UPLOAD
// --------------------------------------------------

void uploadFile(const fs::path& localPath) {
    if (!fs::exists(localPath) ||
        !fs::is_regular_file(localPath)) {

        std::cout << UI::RED
                  << "File not found.\n"
                  << UI::RESET;
        return;
    }

    std::string name = localPath.filename().string();

    if (!validFilename(name)) {
        std::cout << UI::RED
                  << "Invalid filename.\n"
                  << UI::RESET;
        return;
    }

    uint64_t totalSize = fs::file_size(localPath);

    saveUploadPath(name, fs::absolute(localPath));

    std::string hash = sha256File(localPath);

    if (hash.empty()) {
        std::cout << "Could not calculate file hash.\n";
        return;
    }

    int sock = connectToServer();

    if (sock == -1) {
        std::cout << UI::RED
                  << "Could not connect to server.\n"
                  << UI::RESET;
        return;
    }

    std::string request =
        "UPLOAD " + clientId + " " + username + " " +
        name + " " + std::to_string(totalSize);

    if (!sendLine(sock, request)) {
        close(sock);
        return;
    }

    std::string response;

    if (!receiveLine(sock, response)) {
        close(sock);
        return;
    }

    if (response.rfind("READY ", 0) != 0) {
        std::cout << UI::RED << response
                  << UI::RESET << '\n';
        close(sock);
        return;
    }

    uint64_t offset = std::stoull(response.substr(6));

    if (offset > totalSize) {
        std::cout << "Invalid resume position.\n";
        close(sock);
        return;
    }

    std::ifstream file(localPath, std::ios::binary);

    if (!file) {
        close(sock);
        return;
    }

    file.seekg(static_cast<std::streamoff>(offset));

    uint64_t remaining = totalSize - offset;
    uint64_t completed = offset;

    char buffer[BUFFER_SIZE];
    int previousPercentage = -1;

    std::cout << "\nUploading: " << name << '\n';

    if (offset > 0) {
        std::cout << UI::YELLOW
                  << "Resuming from " << formatBytes(offset)
                  << UI::RESET << '\n';
    }

    showProgress("UPLOAD", completed,
                 totalSize, previousPercentage);

    while (remaining > 0) {
        size_t amount = static_cast<size_t>(
            std::min<uint64_t>(remaining, BUFFER_SIZE)
        );

        file.read(buffer, static_cast<std::streamsize>(amount));

        if (static_cast<size_t>(file.gcount()) != amount ||
            !sendAll(sock, buffer, amount)) {

            finishProgress();

            std::cout << UI::RED
                      << "Upload interrupted. Your progress is saved.\n"
                      << UI::RESET;

            close(sock);
            return;
        }

        remaining -= amount;
        completed += amount;

        showProgress("UPLOAD", completed,
                     totalSize, previousPercentage);
    }

    finishProgress();

    if (!sendLine(sock, hash)) {
        close(sock);
        return;
    }

    if (receiveLine(sock, response)) {
        if (response.rfind("OK ", 0) == 0) {
            std::cout << UI::GREEN
                      << response << UI::RESET << '\n';

            removeUploadPath(name);
        }
        else {
            std::cout << UI::RED
                      << response << UI::RESET << '\n';
        }
    }
    else {
        std::cout << UI::YELLOW
                  << "No server confirmation received. "
                  << "You can retry the upload.\n"
                  << UI::RESET;
    }

    close(sock);
}

// --------------------------------------------------
// DOWNLOAD
// --------------------------------------------------

void downloadFile(const std::string& name,
                  uint64_t remoteSize) {

    if (!validFilename(name))
        return;

    fs::create_directories(DOWNLOAD_DIR);

    fs::path finalPath = DOWNLOAD_DIR / name;
    fs::path partPath = DOWNLOAD_DIR / (name + ".part");

    // Skip download if the final file already exists.
    if (fs::exists(finalPath)) {
        std::cout << UI::YELLOW
                  << "File already exists in downloads. "
                  << "Skipping download.\n"
                  << UI::RESET;
        return;
    }

    uint64_t localOffset = fs::exists(partPath)
        ? fs::file_size(partPath) : 0;

    if (localOffset > remoteSize) {
        fs::remove(partPath);
        localOffset = 0;
    }

    int sock = connectToServer();

    if (sock == -1) {
        std::cout << UI::RED
                  << "Could not connect to server.\n"
                  << UI::RESET;
        return;
    }

    std::string request =
        "DOWNLOAD " + clientId + " " + name + " " +
        std::to_string(localOffset);

    if (!sendLine(sock, request)) {
        close(sock);
        return;
    }

    std::string response;

    if (!receiveLine(sock, response)) {
        close(sock);
        return;
    }

    if (response.rfind("FILE ", 0) != 0) {
        std::cout << UI::RED << response
                  << UI::RESET << '\n';
        close(sock);
        return;
    }

    uint64_t totalSize = 0;
    uint64_t offset = 0;

    std::istringstream input(response.substr(5));
    input >> totalSize >> offset;

    if (!input || offset > totalSize) {
        std::cout << "Invalid server response.\n";
        close(sock);
        return;
    }

    std::ios::openmode mode =
        std::ios::binary | std::ios::out;

    if (offset == 0)
        mode |= std::ios::trunc;
    else
        mode |= std::ios::app;

    std::ofstream output(partPath, mode);

    if (!output) {
        std::cout << "Cannot create destination file.\n";
        close(sock);
        return;
    }

    uint64_t remaining = totalSize - offset;
    uint64_t completed = offset;

    char buffer[BUFFER_SIZE];
    int previousPercentage = -1;

    std::cout << "\nDownloading: " << name << '\n';

    if (offset > 0) {
        std::cout << UI::YELLOW
                  << "Resuming from " << formatBytes(offset)
                  << UI::RESET << '\n';
    }

    showProgress("DOWNLOAD", completed,
                 totalSize, previousPercentage);

    while (remaining > 0) {
        size_t amount = static_cast<size_t>(
            std::min<uint64_t>(remaining, BUFFER_SIZE)
        );

        if (!receiveExact(sock, buffer, amount)) {
            finishProgress();

            std::cout << UI::RED
                      << "Download interrupted. Progress saved.\n"
                      << UI::RESET;

            output.close();
            close(sock);
            return;
        }

        output.write(buffer, static_cast<std::streamsize>(amount));

        if (!output) {
            finishProgress();

            std::cout << "Writing to disk failed.\n";
            output.close();
            close(sock);
            return;
        }

        remaining -= amount;
        completed += amount;

        showProgress("DOWNLOAD", completed,
                     totalSize, previousPercentage);
    }

    finishProgress();
    output.close();

    std::string serverHash;

    if (!receiveLine(sock, serverHash)) {
        std::cout << "Could not receive verification hash.\n";
        close(sock);
        return;
    }

    close(sock);

    std::string localHash = sha256File(partPath);

    if (localHash.empty() || localHash != serverHash) {
        std::cout << UI::RED
                  << "SHA-256 verification failed. "
                  << "Partial file retained.\n"
                  << UI::RESET;
        return;
    }

    // Never overwrite an existing local file.
    if (fs::exists(finalPath)) {
        std::cout << UI::YELLOW
                  << "A file with this name already exists in downloads.\n"
                  << "Your existing file has been preserved.\n"
                  << UI::RESET;

        return;
    }

    std::error_code error;

    fs::create_hard_link(partPath, finalPath, error);

    if (error) {
        std::cout << UI::RED
                  << "Could not finalize file: "
                  << error.message() << '\n'
                  << UI::RESET;
        return;
    }

    // Remove the partial file after successful finalization.
    fs::remove(partPath, error);

    if (error) {
        std::cout << "Could not finalize file: "
                  << error.message() << '\n';
        return;
    }

    addLocalHistory("DOWNLOAD", name,
                    totalSize, "COMPLETED");

    std::cout << UI::GREEN
              << "Download completed. SHA-256 verification passed.\n"
              << "Saved to: " << finalPath << '\n'
              << UI::RESET;
}

// --------------------------------------------------
// SHARED FILE LIBRARY
// --------------------------------------------------

void showFileLibrary() {
    auto files = getRemoteFiles();

    std::cout << "\n" << UI::CYAN << UI::BOLD;
    std::cout << "====================== EXCHANGEHUB FILE LIBRARY ======================\n";
    std::cout << UI::RESET;

    if (files.empty()) {
        std::cout << "No shared files are available yet.\n";
        return;
    }

    std::cout << std::left
              << std::setw(5) << "NO."
              << std::setw(30) << "FILE NAME"
              << std::setw(15) << "SIZE"
              << std::setw(20) << "UPLOADED BY"
              << "ACTION\n";

    std::cout << std::string(85, '-') << '\n';

    for (size_t i = 0; i < files.size(); i++) {
        std::cout << std::left
                  << std::setw(5) << i + 1
                  << std::setw(30) << files[i].name.substr(0, 28)
                  << std::setw(15) << formatBytes(files[i].size)
                  << std::setw(20) << files[i].uploader.substr(0, 18)
                  << "Download\n";
    }

    std::cout << "\nEnter file number to download, or 0 to go back: ";

    std::string choice;
    std::getline(std::cin, choice);

    try {
        int selected = std::stoi(choice);

        if (selected == 0)
            return;

        if (selected < 1 ||
            selected > static_cast<int>(files.size())) {
            std::cout << "Invalid selection.\n";
            return;
        }

        const auto& file = files[selected - 1];
        downloadFile(file.name, file.size);
    }
    catch (...) {
        std::cout << "Invalid selection.\n";
    }
}

// --------------------------------------------------
// PAUSED TRANSFERS
// --------------------------------------------------

std::vector<fs::path> getPausedDownloads() {
    std::vector<fs::path> result;

    fs::create_directories(DOWNLOAD_DIR);

    for (const auto& entry : fs::directory_iterator(DOWNLOAD_DIR)) {
        if (!entry.is_regular_file())
            continue;

        std::string name = entry.path().filename().string();

        if (name.size() > 5 &&
            name.substr(name.size() - 5) == ".part") {
            result.push_back(entry.path());
        }
    }

    return result;
}

void showTransferHistory() {
    std::cout << "\n" << UI::CYAN << UI::BOLD
              << "====================== MY TRANSFER HISTORY ======================\n"
              << UI::RESET;

    auto uploads = requestRows("HISTORY", "HISTORY");

    std::cout << "\nUPLOAD HISTORY\n";

    if (uploads.empty())
        std::cout << "  No upload history found.\n";

    for (const auto& row : uploads)
        std::cout << "  " << row << '\n';

    std::cout << "\nDOWNLOAD HISTORY\n";

    std::ifstream history(DOWNLOAD_HISTORY_FILE);
    std::string line;
    bool found = false;

    while (std::getline(history, line)) {
        found = true;
        std::replace(line.begin(), line.end(), '\t', ' ');
        std::cout << "  " << line << '\n';
    }

    if (!found)
        std::cout << "  No download history found.\n";
}

void showMyTransfers() {
    auto uploads = getPausedUploads();
    auto downloads = getPausedDownloads();

    std::cout << "\n" << UI::CYAN << UI::BOLD
              << "======================= MY TRANSFERS =======================\n"
              << UI::RESET;

    std::cout << "\nPAUSED UPLOADS\n";

    if (uploads.empty())
        std::cout << "  No paused uploads.\n";

    for (const auto& upload : uploads) {
        std::cout << "  " << upload.name << "  "
                  << formatBytes(upload.uploadedSize)
                  << " / " << formatBytes(upload.totalSize)
                  << '\n';
    }

    std::cout << "\nPAUSED DOWNLOADS\n";

    if (downloads.empty())
        std::cout << "  No paused downloads.\n";

    for (const auto& download : downloads) {
        std::cout << "  " << download.stem().string()
                  << "  " << formatBytes(fs::file_size(download))
                  << " downloaded\n";
    }

    std::cout << "\n";
    std::cout << "  [1] Resume a paused upload\n";
    std::cout << "  [2] Delete a paused upload\n";
    std::cout << "  [3] Resume a paused download\n";
    std::cout << "  [4] Delete a paused download\n";
    std::cout << "  [5] View transfer history\n";
    std::cout << "  [0] Back\n";
    std::cout << "\nSelect: ";

    std::string choice;
    std::getline(std::cin, choice);

    if (choice == "0")
        return;

    if (choice == "5") {
        showTransferHistory();
        return;
    }

    if (choice == "1" || choice == "2") {
        if (uploads.empty()) {
            std::cout << "No paused uploads to manage.\n";
            return;
        }

        std::cout << "\nPaused uploads:\n";

        for (size_t i = 0; i < uploads.size(); i++) {
            std::cout << "  [" << i + 1 << "] "
                      << uploads[i].name << " - "
                      << formatBytes(uploads[i].uploadedSize)
                      << " / " << formatBytes(uploads[i].totalSize)
                      << '\n';
        }

        std::cout << "Select upload number: ";

        std::string selection;
        std::getline(std::cin, selection);

        try {
            int index = std::stoi(selection) - 1;

            if (index < 0 ||
                index >= static_cast<int>(uploads.size()))
                return;

            const auto& upload = uploads[index];

            if (choice == "1") {
                fs::path originalPath;

                bool pathFound =
                    loadUploadPath(upload.name, originalPath);

                if (!pathFound || !fs::exists(originalPath) ||
                    !fs::is_regular_file(originalPath)) {

                    std::cout << UI::YELLOW
                              << "\nOriginal file could not be located.\n"
                              << UI::RESET;

                    std::cout << "Enter the original file path once: ";

                    std::string replacementPath;
                    std::getline(std::cin, replacementPath);

                    originalPath = replacementPath;
                }
                else {
                    std::cout << UI::GREEN
                              << "\nOriginal file found automatically:\n"
                              << UI::RESET
                              << originalPath << '\n';
                }

                if (!fs::exists(originalPath) ||
                    !fs::is_regular_file(originalPath)) {
                    std::cout << UI::RED
                              << "File not found. Upload was not resumed.\n"
                              << UI::RESET;
                    return;
                }

                if (originalPath.filename().string() != upload.name) {
                    std::cout << UI::RED
                              << "Selected file has a different filename.\n"
                              << UI::RESET;
                    return;
                }

                if (fs::file_size(originalPath) != upload.totalSize) {
                    std::cout << UI::RED
                              << "File size differs from the original upload.\n"
                              << "Please select the original unchanged file.\n"
                              << UI::RESET;
                    return;
                }

                saveUploadPath(
                    upload.name,
                    fs::absolute(originalPath)
                );

                uploadFile(originalPath);
            }
            else {
                int sock = connectToServer();

                if (sock == -1) {
                    std::cout << "Could not connect to server.\n";
                    return;
                }

                sendLine(
                    sock,
                    "DELETE_UPLOAD " + clientId + " " + upload.name
                );

                std::string response;

                if (receiveLine(sock, response)) {
                    std::cout << response << '\n';

                    if (response.rfind("OK ", 0) == 0)
                        removeUploadPath(upload.name);
                }

                close(sock);
            }
        }
        catch (...) {
            std::cout << "Invalid selection.\n";
        }

        return;
    }

    if (choice == "3" || choice == "4") {
        if (downloads.empty()) {
            std::cout << "No paused downloads to manage.\n";
            return;
        }

        std::cout << "\nPaused downloads:\n";

        for (size_t i = 0; i < downloads.size(); i++) {
            std::cout << "  [" << i + 1 << "] "
                      << downloads[i].stem().string() << " - "
                      << formatBytes(fs::file_size(downloads[i]))
                      << " downloaded\n";
        }

        std::cout << "Select download number: ";

        std::string selection;
        std::getline(std::cin, selection);

        try {
            int index = std::stoi(selection) - 1;

            if (index < 0 ||
                index >= static_cast<int>(downloads.size()))
                return;

            if (choice == "4") {
                fs::remove(downloads[index]);

                std::cout << UI::GREEN
                          << "Paused download deleted.\n"
                          << UI::RESET;
            }
            else {
                std::string name =
                    downloads[index].filename().string();

                name = name.substr(0, name.size() - 5);

                auto files = getRemoteFiles();

                auto found = std::find_if(
                    files.begin(), files.end(),
                    [&](const RemoteFile& file) {
                        return file.name == name;
                    }
                );

                if (found == files.end()) {
                    std::cout << "File is no longer in the server library.\n";
                    return;
                }

                downloadFile(found->name, found->size);
            }
        }
        catch (...) {
            std::cout << "Invalid selection.\n";
        }

        return;
    }

    std::cout << "Invalid selection.\n";
}

// --------------------------------------------------
// TEAM CHAT
// --------------------------------------------------

void startChat() {
    int sock = connectToServer();

    if (sock == -1) {
        std::cout << UI::RED
                  << "Could not connect to server.\n"
                  << UI::RESET;
        return;
    }

    if (!sendLine(sock, "CHAT " + username)) {
        close(sock);
        return;
    }

    std::string response;

    if (!receiveLine(sock, response) || response != "CHAT_OK") {
        std::cout << "Could not join chat.\n";
        close(sock);
        return;
    }

    // Save terminal settings so they can be restored on exit.
    termios originalTerminal{};

    if (tcgetattr(STDIN_FILENO, &originalTerminal) == -1) {
        std::cout << "Could not read terminal settings.\n";
        close(sock);
        return;
    }

    // Disable automatic terminal echo and line buffering.
    termios chatTerminal = originalTerminal;
    chatTerminal.c_lflag &=
        static_cast<tcflag_t>(~(ICANON | ECHO));

    chatTerminal.c_cc[VMIN] = 1;
    chatTerminal.c_cc[VTIME] = 0;

    if (tcsetattr(STDIN_FILENO, TCSANOW, &chatTerminal) == -1) {
        std::cout << "Could not configure terminal for chat.\n";
        close(sock);
        return;
    }

    // Clear screen.
    std::cout << "\033[2J\033[H";

    std::cout << UI::CYAN << UI::BOLD;
    std::cout << "╔══════════════════════════════════════════════════════╗\n";
    std::cout << "║                 EXCHANGEHUB GROUP CHAT                   ║\n";
    std::cout << "╚══════════════════════════════════════════════════════╝\n";
    std::cout << UI::RESET;

    std::cout << UI::GRAY
              << "  Logged in as " << username
              << " | Press Enter to send, type /exit to leave\n"
              << UI::RESET;

    std::cout << "--------------------------------------------------------\n\n";

    auto displayMessage = [&](const std::string& message) {
        // System messages.
        if (message.rfind("[SYSTEM] ", 0) == 0) {
            std::cout << UI::GRAY
                      << "* " << message.substr(9) << " *"
                      << UI::RESET << '\n';
            return;
        }

        size_t separator = message.find(':');

        if (separator == std::string::npos) {
            std::cout << message << '\n';
            return;
        }

        std::string sender = message.substr(0, separator);
        std::string text = message.substr(separator + 1);

        // Ignore our own server echo because we print locally on Enter.
        if (sender == username)
            return;

        std::cout << UI::CYAN << UI::BOLD
                  << sender
                  << UI::RESET
                  << ":" << text << '\n';
    };

    std::string inputBuffer;

    // Persistent username prompt.
    auto drawPrompt = [&]() {
        std::cout << "\r\033[2K"
                  << UI::GREEN << UI::BOLD
                  << username << ": "
                  << UI::RESET
                  << inputBuffer
                  << std::flush;
    };

    drawPrompt();

    bool running = true;

    while (running) {
        pollfd descriptors[2]{};

        descriptors[0].fd = sock;
        descriptors[0].events = POLLIN;

        descriptors[1].fd = STDIN_FILENO;
        descriptors[1].events = POLLIN;

        int result = poll(descriptors, 2, -1);

        if (result < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        // Incoming messages.
        if (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            std::string incomingMessage;

            if (!receiveLine(sock, incomingMessage)) {
                std::cout << "\r\033[2K"
                          << UI::RED
                          << "Disconnected from group chat.\n"
                          << UI::RESET;
                break;
            }

            // Clear prompt, display message, redraw prompt.
            std::cout << "\r\033[2K";
            displayMessage(incomingMessage);
            drawPrompt();
        }

        // Keyboard input.
        if (descriptors[1].revents & POLLIN) {
            char character = '\0';
            ssize_t bytesRead = read(STDIN_FILENO, &character, 1);

            if (bytesRead <= 0)
                break;

            // Enter sends the message.
            if (character == '\n' || character == '\r') {
                if (inputBuffer == "/exit") {
                    sendLine(sock, "/exit");
                    running = false;
                    break;
                }

                if (!inputBuffer.empty()) {
                    std::string outgoingMessage = inputBuffer;

                    // Display the sent message exactly once.
                    std::cout << "\r\033[2K"
                              << UI::GREEN << UI::BOLD
                              << username
                              << UI::RESET
                              << ": " << outgoingMessage << '\n'
                              << std::flush;

                    // Send only the message text to the server.
                    if (!sendLine(sock, outgoingMessage)) {
                        running = false;
                        break;
                    }

                    inputBuffer.clear();
                }

                drawPrompt();
            }
            // Backspace.
            else if (character == 127 || character == 8) {
                if (!inputBuffer.empty()) {
                    inputBuffer.pop_back();
                    drawPrompt();
                }
            }
            // Ctrl+D exits.
            else if (character == 4) {
                sendLine(sock, "/exit");
                running = false;
                break;
            }
            // Normal printable characters.
            else if (character >= 32 && character != 127) {
                inputBuffer.push_back(character);
                drawPrompt();
            }
        }

        if (descriptors[1].revents & (POLLHUP | POLLERR))
            break;
    }

    // Restore terminal settings.
    tcsetattr(STDIN_FILENO, TCSANOW, &originalTerminal);

    shutdown(sock, SHUT_RDWR);
    close(sock);

    std::cout << "\n" << UI::YELLOW
              << "You left the group chat.\n"
              << UI::RESET;
}

// --------------------------------------------------
// MAIN DASHBOARD
// --------------------------------------------------

void showDashboard() {
    std::cout << "\033[2J\033[H";

    std::cout << UI::CYAN << UI::BOLD;
    std::cout << "============================================================\n";
    std::cout << "                         EXCHANGEHUB                            \n";
    std::cout << "              FILE SHARING & GROUP COMMUNICATION                 \n";
    std::cout << "============================================================\n";
    std::cout << UI::RESET;

    std::cout << "  USER             : " << username << '\n';
    std::cout << "  SERVER           : " << serverIP << ":" << PORT << '\n';
    std::cout << "  PROTOCOL         : TCP / IPv4\n";
    std::cout << "  FILE INTEGRITY   : SHA-256\n";

    std::cout << UI::CYAN;
    std::cout << "\n------------------------------------------------------------\n";
    std::cout << "                         OPERATIONS                         \n";
    std::cout << "------------------------------------------------------------\n";
    std::cout << UI::RESET;

    std::cout << "  [1] Upload a file\n";
    std::cout << "  [2] Browse shared file library\n";
    std::cout << "  [3] My transfers (resume / delete / history)\n";
    std::cout << "  [4] Join team chat\n";
    std::cout << "  [0] Exit\n";

    std::cout << UI::CYAN;
    std::cout << "------------------------------------------------------------\n";
    std::cout << UI::RESET;
}

int main(int argc, char* argv[]) {
    if (argc > 1)
        serverIP = argv[1];

    if (!initializeIdentity())
        return 1;

    fs::create_directories(DOWNLOAD_DIR);
    fs::create_directories(UPLOAD_META_DIR);

    while (true) {
        showDashboard();

        std::cout << "\nSelect operation: ";

        std::string choice;
        std::getline(std::cin, choice);

        if (choice == "1") {
            std::cout << "Enter local file path: ";

            std::string path;
            std::getline(std::cin, path);

            uploadFile(path);
        }
        else if (choice == "2") {
            showFileLibrary();
        }
        else if (choice == "3") {
            showMyTransfers();
        }
        else if (choice == "4") {
            startChat();
        }
        else if (choice == "0") {
            std::cout << UI::GREEN
                      << "\nThank you for using EXCHANGEHUB.\n"
                      << UI::RESET;
            break;
        }
        else {
            std::cout << UI::RED
                      << "Invalid selection.\n"
                      << UI::RESET;
        }

        if (choice != "0") {
            std::cout << "\nPress Enter to return to dashboard...";

            std::string pause;
            std::getline(std::cin, pause);
        }
    }

    return 0;
}