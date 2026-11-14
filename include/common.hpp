#ifndef EXCHANGEHUB_COMMON_HPP
#define EXCHANGEHUB_COMMON_HPP
#include <vector>
#include <iostream>
#include <fstream>
#include <string>
#include <filesystem>
#include <sstream>
#include <cctype>
#include <cstdint>
#include <algorithm>
#include <iomanip>
#include <sys/socket.h>
#include <openssl/evp.h>
#include <cerrno>

namespace fs = std::filesystem;

namespace ExchangeHub {

constexpr size_t BUFFER_SIZE = 65536;

namespace UI {
    constexpr const char* RESET  = "\033[0m";
    constexpr const char* BOLD   = "\033[1m";
    constexpr const char* CYAN   = "\033[36m";
    constexpr const char* GREEN  = "\033[32m";
    constexpr const char* YELLOW = "\033[33m";
    constexpr const char* RED    = "\033[31m";
    constexpr const char* BLUE   = "\033[34m";
    constexpr const char* GRAY   = "\033[90m";
}

inline bool sendAll(int sock, const char* data, size_t length) {
    size_t total = 0;

    while (total < length) {
        ssize_t sent = send(sock, data + total,
                            length - total, MSG_NOSIGNAL);
        if (sent <= 0) return false;
        total += static_cast<size_t>(sent);
    }

    return true;
}


inline bool receiveExact(
    int sock,
    char* data,
    size_t length,
    size_t* receivedBytes = nullptr
) {
    size_t total = 0;

    while (total < length) {
        ssize_t received = recv(
            sock,
            data + total,
            length - total,
            0
        );

        if (received < 0) {
            if (errno == EINTR)
                continue;

            if (receivedBytes)
                *receivedBytes = total;

            return false;
        }

        if (received == 0) {
            if (receivedBytes)
                *receivedBytes = total;

            return false;
        }

        total += static_cast<size_t>(received);
    }

    if (receivedBytes)
        *receivedBytes = total;

    return true;
}


inline bool sendLine(int sock, const std::string& message) {
    std::string data = message + "\n";
    return sendAll(sock, data.c_str(), data.size());
}

inline bool receiveLine(int sock, std::string& message) {
    message.clear();
    char ch;

    while (message.size() < 4096) {
        ssize_t received = recv(sock, &ch, 1, 0);

        if (received <= 0) return false;
        if (ch == '\n') return true;

        if (ch != '\r')
            message += ch;
    }

    return false;
}


inline bool validFilename(const std::string& name) {
    if (name.empty() || name == "." || name == "..")
        return false;

    // Protect ExchangeHub internal storage files and directories.
    static const std::vector<std::string> reservedNames = {
        ".file_index.tsv",
        ".history.tsv",
        ".partials"
    };

    if (std::find(
            reservedNames.begin(),
            reservedNames.end(),
            name
        ) != reservedNames.end()) {
        return false;
    }

    for (unsigned char ch : name) {
        if (!std::isalnum(ch) && ch != '.' &&
            ch != '_' && ch != '-') {
            return false;
        }
    }

    return true;
}

inline bool validClientId(const std::string& id) {
    if (id.empty() || id.size() > 64)
        return false;

    for (unsigned char ch : id) {
        if (!std::isalnum(ch) && ch != '-')
            return false;
    }

    return true;
}

inline bool validUsername(const std::string& username) {
    if (username.empty() || username.size() > 30)
        return false;

    for (unsigned char ch : username) {
        if (!std::isalnum(ch) && ch != '_' && ch != '-')
            return false;
    }

    return true;
}

inline std::string sha256File(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) return "";

    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        return "";
    }

    char buffer[BUFFER_SIZE];

    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        if (EVP_DigestUpdate(
                context, buffer,
                static_cast<size_t>(file.gcount())) != 1) {
            EVP_MD_CTX_free(context);
            return "";
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLength = 0;

    bool success = EVP_DigestFinal_ex(
        context, hash, &hashLength
    ) == 1;

    EVP_MD_CTX_free(context);

    if (!success) return "";

    const char hex[] = "0123456789abcdef";
    std::string result;

    for (unsigned int i = 0; i < hashLength; i++) {
        result += hex[hash[i] >> 4];
        result += hex[hash[i] & 0x0F];
    }

    return result;
}

inline bool sendFileRange(int sock, const fs::path& path,
                          uint64_t offset) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;

    file.seekg(static_cast<std::streamoff>(offset));

    char buffer[BUFFER_SIZE];

    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        if (!sendAll(sock, buffer,
                     static_cast<size_t>(file.gcount()))) {
            return false;
        }
    }

    return file.eof();
}

inline std::string formatBytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;

    while (size >= 1024 && unit < 4) {
        size /= 1024;
        unit++;
    }

    std::ostringstream output;
    output << std::fixed << std::setprecision(2)
           << size << " " << units[unit];

    return output.str();
}

} // namespace ExchangeHub

#endif