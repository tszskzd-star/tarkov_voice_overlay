#include "tvo/net/DirectVoiceTransport.h"
#include "tvo/net/IceConfig.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#ifdef TVO_PLATFORM_WINDOWS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <winhttp.h>

#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

namespace tvo {

namespace {

using namespace std::chrono_literals;

constexpr std::array<std::uint8_t, 4> kPacketMagic{'T', 'V', 'O', 'A'};
constexpr std::uint8_t kPacketVersion = 1;
constexpr std::uint8_t kPacketPing = 1;
constexpr std::uint8_t kPacketVoice = 2;
constexpr std::size_t kMaxUdpPayloadBytes = 1200;
constexpr std::uint16_t kDefaultDirectVoicePort = 40771;

void appendP2PLog(const std::string& line) {
    std::error_code ignored;
    std::filesystem::create_directories("logs", ignored);
    std::ofstream out("logs/p2p-voice.log", std::ios::out | std::ios::app);
    if (out) {
        out << line << "\n";
    }
}

std::string escapeJson(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (unsigned char c : text) {
        switch (c) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            out += c < 0x20 ? ' ' : static_cast<char>(c);
            break;
        }
    }
    return out;
}

std::size_t skipWhitespace(const std::string& text, std::size_t pos) {
    while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos])) != 0) {
        ++pos;
    }
    return pos;
}

std::optional<std::size_t> findJsonValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const std::size_t keyPos = json.find(needle);
    if (keyPos == std::string::npos) {
        return std::nullopt;
    }

    const std::size_t colon = json.find(':', keyPos + needle.size());
    if (colon == std::string::npos) {
        return std::nullopt;
    }

    return skipWhitespace(json, colon + 1);
}

std::optional<std::string> extractJsonString(const std::string& json, const std::string& key) {
    const auto valuePos = findJsonValue(json, key);
    if (!valuePos || *valuePos >= json.size() || json[*valuePos] != '"') {
        return std::nullopt;
    }

    std::string out;
    for (std::size_t i = *valuePos + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') {
            return out;
        }
        if (c == '\\' && i + 1 < json.size()) {
            const char escaped = json[++i];
            switch (escaped) {
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            default:
                out += escaped;
                break;
            }
        } else {
            out += c;
        }
    }

    return std::nullopt;
}

std::optional<double> extractJsonNumber(const std::string& json, const std::string& key) {
    const auto valuePos = findJsonValue(json, key);
    if (!valuePos) {
        return std::nullopt;
    }

    const char* start = json.c_str() + *valuePos;
    char* end = nullptr;
    const double parsed = std::strtod(start, &end);
    if (end == start) {
        return std::nullopt;
    }
    return parsed;
}

std::uint16_t preferredDirectVoicePort() {
    const char* value = std::getenv("TVO_DIRECT_UDP_PORT");
    if (value == nullptr || *value == '\0') {
        return kDefaultDirectVoicePort;
    }

    try {
        const int parsed = std::stoi(value);
        if (parsed > 0 && parsed <= 65535) {
            return static_cast<std::uint16_t>(parsed);
        }
    } catch (...) {
    }
    return kDefaultDirectVoicePort;
}

bool envFlagEnabled(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}

std::vector<std::string> configuredStunServers() {
    std::vector<std::string> servers;
    auto addServer = [&servers](std::string server) {
        const auto first = server.find_first_not_of(" \t\r\n");
        const auto last = server.find_last_not_of(" \t\r\n");
        if (first == std::string::npos || last == std::string::npos) {
            return;
        }
        server = server.substr(first, last - first + 1);
        if (std::find(servers.begin(), servers.end(), server) == servers.end()) {
            servers.push_back(std::move(server));
        }
    };

    if (const char* value = std::getenv("TVO_STUN_SERVERS")) {
        std::string current;
        for (const char c : std::string(value)) {
            if (c == ',' || c == ';') {
                addServer(std::move(current));
                current.clear();
            } else {
                current.push_back(c);
            }
        }
        addServer(std::move(current));
    }

    addServer(std::string(kDefaultGoogleStun));
    addServer("stun:stun1.l.google.com:19302");
    addServer("stun:stun2.l.google.com:19302");
    addServer("stun:stun3.l.google.com:19302");
    addServer("stun:stun4.l.google.com:19302");
    addServer("stun:stun.cloudflare.com:3478");
    addServer("stun:global.stun.twilio.com:3478");
    addServer("stun:stun.relay.metered.ca:80");
    addServer("stun:stun.relay.metered.ca:443");
    return servers;
}

std::optional<std::string> extractBalancedValue(
    const std::string& json,
    const std::string& key,
    char openChar,
    char closeChar) {
    const auto valuePos = findJsonValue(json, key);
    if (!valuePos || *valuePos >= json.size() || json[*valuePos] != openChar) {
        return std::nullopt;
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = *valuePos; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
        } else if (c == openChar) {
            ++depth;
        } else if (c == closeChar) {
            --depth;
            if (depth == 0) {
                return json.substr(*valuePos, i - *valuePos + 1);
            }
        }
    }

    return std::nullopt;
}

std::vector<std::string> splitObjectArray(const std::string& arrayJson) {
    std::vector<std::string> objects;
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    std::size_t objectStart = std::string::npos;

    for (std::size_t i = 0; i < arrayJson.size(); ++i) {
        const char c = arrayJson[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }

        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            if (depth == 0) {
                objectStart = i;
            }
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && objectStart != std::string::npos) {
                objects.push_back(arrayJson.substr(objectStart, i - objectStart + 1));
                objectStart = std::string::npos;
            }
        }
    }

    return objects;
}

void appendU16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void appendU32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xff));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xff));
    out.push_back(static_cast<std::uint8_t>(value & 0xff));
}

void appendU64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xff));
    }
}

bool readU16(const std::vector<std::uint8_t>& data, std::size_t& pos, std::uint16_t& out) {
    if (pos + 2 > data.size()) {
        return false;
    }
    out = static_cast<std::uint16_t>((data[pos] << 8) | data[pos + 1]);
    pos += 2;
    return true;
}

bool readU32(const std::vector<std::uint8_t>& data, std::size_t& pos, std::uint32_t& out) {
    if (pos + 4 > data.size()) {
        return false;
    }
    out = (static_cast<std::uint32_t>(data[pos]) << 24) |
        (static_cast<std::uint32_t>(data[pos + 1]) << 16) |
        (static_cast<std::uint32_t>(data[pos + 2]) << 8) |
        static_cast<std::uint32_t>(data[pos + 3]);
    pos += 4;
    return true;
}

bool readU64(const std::vector<std::uint8_t>& data, std::size_t& pos, std::uint64_t& out) {
    if (pos + 8 > data.size()) {
        return false;
    }
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | data[pos + i];
    }
    pos += 8;
    return true;
}

std::vector<std::uint8_t> encodePacket(
    std::uint8_t type,
    const RoomId& roomId,
    const PeerId& sourcePeerId,
    std::uint32_t sequence,
    std::int64_t captureUnixMs,
    const std::vector<std::uint8_t>& payload) {
    if (payload.size() > kMaxUdpPayloadBytes ||
        roomId.size() > 0xffff ||
        sourcePeerId.size() > 0xffff) {
        return {};
    }

    std::vector<std::uint8_t> out;
    out.reserve(32 + roomId.size() + sourcePeerId.size() + payload.size());
    out.insert(out.end(), kPacketMagic.begin(), kPacketMagic.end());
    out.push_back(kPacketVersion);
    out.push_back(type);
    appendU16(out, 0);
    appendU32(out, sequence);
    appendU64(out, static_cast<std::uint64_t>(std::max<std::int64_t>(0, captureUnixMs)));
    appendU16(out, static_cast<std::uint16_t>(sourcePeerId.size()));
    appendU16(out, static_cast<std::uint16_t>(roomId.size()));
    appendU32(out, static_cast<std::uint32_t>(payload.size()));
    out.insert(out.end(), sourcePeerId.begin(), sourcePeerId.end());
    out.insert(out.end(), roomId.begin(), roomId.end());
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

struct DecodedPacket {
    std::uint8_t type = 0;
    RoomId roomId;
    PeerId sourcePeerId;
    std::uint32_t sequence = 0;
    std::int64_t captureUnixMs = 0;
    std::vector<std::uint8_t> payload;
};

std::optional<DecodedPacket> decodePacket(const std::uint8_t* raw, int size) {
    if (size <= 0) {
        return std::nullopt;
    }

    std::vector<std::uint8_t> data(raw, raw + size);
    std::size_t pos = 0;
    if (data.size() < 24 ||
        !std::equal(kPacketMagic.begin(), kPacketMagic.end(), data.begin())) {
        return std::nullopt;
    }
    pos += kPacketMagic.size();

    if (data[pos++] != kPacketVersion) {
        return std::nullopt;
    }

    DecodedPacket packet;
    packet.type = data[pos++];

    std::uint16_t ignored = 0;
    std::uint16_t sourceLen = 0;
    std::uint16_t roomLen = 0;
    std::uint32_t payloadLen = 0;
    std::uint64_t captureMs = 0;
    if (!readU16(data, pos, ignored) ||
        !readU32(data, pos, packet.sequence) ||
        !readU64(data, pos, captureMs) ||
        !readU16(data, pos, sourceLen) ||
        !readU16(data, pos, roomLen) ||
        !readU32(data, pos, payloadLen)) {
        return std::nullopt;
    }

    if (pos + sourceLen + roomLen + payloadLen > data.size()) {
        return std::nullopt;
    }

    packet.captureUnixMs = static_cast<std::int64_t>(captureMs);
    packet.sourcePeerId.assign(
        reinterpret_cast<const char*>(data.data() + pos),
        reinterpret_cast<const char*>(data.data() + pos + sourceLen));
    pos += sourceLen;
    packet.roomId.assign(
        reinterpret_cast<const char*>(data.data() + pos),
        reinterpret_cast<const char*>(data.data() + pos + roomLen));
    pos += roomLen;
    packet.payload.assign(data.begin() + static_cast<std::ptrdiff_t>(pos),
        data.begin() + static_cast<std::ptrdiff_t>(pos + payloadLen));
    return packet;
}

#ifdef TVO_PLATFORM_WINDOWS
struct SocketEndpoint {
    sockaddr_storage storage{};
    int length = 0;
    std::string address;
    std::uint16_t port = 0;
};

bool sameEndpoint(const SocketEndpoint& left, const SocketEndpoint& right) {
    return left.address == right.address && left.port == right.port;
}

struct WinHttpHandle {
    HINTERNET value = nullptr;

    ~WinHttpHandle() {
        if (value != nullptr) {
            WinHttpCloseHandle(value);
        }
    }

    explicit operator bool() const noexcept {
        return value != nullptr;
    }
};

bool isPublicIPv4Address(const std::string& text) {
    IN_ADDR address{};
    if (InetPtonA(AF_INET, text.c_str(), &address) != 1) {
        return false;
    }

    const std::uint32_t ip = ntohl(address.S_un.S_addr);
    const std::uint8_t a = static_cast<std::uint8_t>((ip >> 24) & 0xff);
    const std::uint8_t b = static_cast<std::uint8_t>((ip >> 16) & 0xff);

    if (a == 0 || a == 10 || a == 127 || a >= 224) {
        return false;
    }
    if (a == 100 && b >= 64 && b <= 127) {
        return false;
    }
    if (a == 169 && b == 254) {
        return false;
    }
    if (a == 172 && b >= 16 && b <= 31) {
        return false;
    }
    if (a == 192 && b == 168) {
        return false;
    }
    return true;
}

std::optional<std::string> extractPublicIPv4Token(const std::string& body) {
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (std::isdigit(static_cast<unsigned char>(body[i])) == 0) {
            continue;
        }

        std::size_t end = i;
        int dots = 0;
        while (end < body.size() &&
            (std::isdigit(static_cast<unsigned char>(body[end])) != 0 || body[end] == '.')) {
            if (body[end] == '.') {
                ++dots;
            }
            ++end;
        }

        std::string token = body.substr(i, end - i);
        if (dots == 3 && isPublicIPv4Address(token)) {
            return token;
        }
        i = end;
    }
    return std::nullopt;
}

std::optional<std::string> queryPublicIPv4Endpoint(
    const wchar_t* host,
    INTERNET_PORT port,
    const wchar_t* path,
    bool secure) {
    WinHttpHandle session{WinHttpOpen(
        L"TarkovVoiceOverlay/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0)};
    if (!session) {
        return std::nullopt;
    }

    WinHttpSetTimeouts(session.value, 1000, 1000, 1000, 1500);

    WinHttpHandle connect{WinHttpConnect(session.value, host, port, 0)};
    if (!connect) {
        return std::nullopt;
    }

    WinHttpHandle request{WinHttpOpenRequest(
        connect.value,
        L"GET",
        path,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        secure ? WINHTTP_FLAG_SECURE : 0)};
    if (!request) {
        return std::nullopt;
    }

    if (!WinHttpSendRequest(
            request.value,
            WINHTTP_NO_ADDITIONAL_HEADERS,
            0,
            WINHTTP_NO_REQUEST_DATA,
            0,
            0,
            0) ||
        !WinHttpReceiveResponse(request.value, nullptr)) {
        return std::nullopt;
    }

    std::string body;
    std::array<char, 256> buffer{};
    for (;;) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(
                request.value,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &bytesRead) ||
            bytesRead == 0) {
            break;
        }
        body.append(buffer.data(), buffer.data() + bytesRead);
        if (body.size() > 1024) {
            break;
        }
    }

    return extractPublicIPv4Token(body);
}

std::optional<std::string> queryPublicIPv4ViaHttps() {
    struct Endpoint {
        const wchar_t* host;
        INTERNET_PORT port;
        const wchar_t* path;
        bool secure;
        const char* name;
    };

    constexpr std::array<Endpoint, 3> endpoints{{
        Endpoint{L"api.ipify.org", INTERNET_DEFAULT_HTTPS_PORT, L"/", true, "api.ipify.org"},
        Endpoint{L"checkip.amazonaws.com", INTERNET_DEFAULT_HTTPS_PORT, L"/", true, "checkip.amazonaws.com"},
        Endpoint{L"ifconfig.me", INTERNET_DEFAULT_HTTPS_PORT, L"/ip", true, "ifconfig.me"},
    }};

    for (const auto& endpoint : endpoints) {
        if (auto address = queryPublicIPv4Endpoint(
                endpoint.host,
                endpoint.port,
                endpoint.path,
                endpoint.secure)) {
            appendP2PLog(std::string("public IPv4 discovered via HTTPS ") +
                endpoint.name + " " + *address);
            return address;
        }
        appendP2PLog(std::string("HTTPS public IPv4 lookup failed via ") + endpoint.name);
    }
    return std::nullopt;
}

std::optional<SocketEndpoint> makeEndpoint(const std::string& address, std::uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    hints.ai_flags = AI_NUMERICHOST;

    const std::string service = std::to_string(port);
    addrinfo* result = nullptr;
    if (getaddrinfo(address.c_str(), service.c_str(), &hints, &result) != 0 || result == nullptr) {
        return std::nullopt;
    }

    SocketEndpoint endpoint;
    std::memcpy(&endpoint.storage, result->ai_addr, result->ai_addrlen);
    endpoint.length = static_cast<int>(result->ai_addrlen);
    endpoint.address = address;
    endpoint.port = port;
    freeaddrinfo(result);
    return endpoint;
}

std::optional<SocketEndpoint> endpointFromSockaddr(const sockaddr_storage& storage, int length) {
    if (storage.ss_family != AF_INET) {
        return std::nullopt;
    }

    char host[INET_ADDRSTRLEN]{};
    const auto* addr = reinterpret_cast<const sockaddr_in*>(&storage);
    if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&addr->sin_addr), host, sizeof(host)) == nullptr) {
        return std::nullopt;
    }

    SocketEndpoint endpoint;
    endpoint.storage = storage;
    endpoint.length = length;
    endpoint.address = host;
    endpoint.port = ntohs(addr->sin_port);
    return endpoint;
}

std::optional<SocketEndpoint> queryStunMappedEndpoint(
    SOCKET socket,
    std::uint16_t localPort,
    std::string stun) {
    if (stun.rfind("stun:", 0) == 0) {
        stun = stun.substr(5);
    }

    std::string host = stun;
    std::string port = "19302";
    const std::size_t colon = stun.rfind(':');
    if (colon != std::string::npos) {
        host = stun.substr(0, colon);
        port = stun.substr(colon + 1);
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &result) != 0 || result == nullptr) {
        return std::nullopt;
    }

    std::array<std::uint8_t, 20> request{};
    request[1] = 0x01;
    request[4] = 0x21;
    request[5] = 0x12;
    request[6] = 0xa4;
    request[7] = 0x42;
    for (std::size_t i = 8; i < request.size(); ++i) {
        request[i] = static_cast<std::uint8_t>(std::rand() & 0xff);
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        sendto(
            socket,
            reinterpret_cast<const char*>(request.data()),
            static_cast<int>(request.size()),
            0,
            result->ai_addr,
            static_cast<int>(result->ai_addrlen));

        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(socket, &reads);
        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 350000;
        const int ready = select(0, &reads, nullptr, nullptr, &timeout);
        if (ready <= 0) {
            continue;
        }

        std::array<std::uint8_t, 512> response{};
        sockaddr_storage from{};
        int fromLen = sizeof(from);
        const int received = recvfrom(
            socket,
            reinterpret_cast<char*>(response.data()),
            static_cast<int>(response.size()),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &fromLen);
        if (received < 20 ||
            response[0] != 0x01 ||
            response[1] != 0x01 ||
            !std::equal(request.begin() + 8, request.end(), response.begin() + 8)) {
            continue;
        }

        const std::uint16_t length = static_cast<std::uint16_t>((response[2] << 8) | response[3]);
        std::size_t pos = 20;
        const std::size_t end = std::min<std::size_t>(response.size(), 20 + length);
        while (pos + 4 <= end) {
            const std::uint16_t attrType = static_cast<std::uint16_t>((response[pos] << 8) | response[pos + 1]);
            const std::uint16_t attrLen = static_cast<std::uint16_t>((response[pos + 2] << 8) | response[pos + 3]);
            pos += 4;
            if (pos + attrLen > end) {
                break;
            }

            if ((attrType == 0x0020 || attrType == 0x0001) && attrLen >= 8 && response[pos + 1] == 0x01) {
                std::uint16_t mappedPort = static_cast<std::uint16_t>((response[pos + 2] << 8) | response[pos + 3]);
                std::uint32_t mappedAddress =
                    (static_cast<std::uint32_t>(response[pos + 4]) << 24) |
                    (static_cast<std::uint32_t>(response[pos + 5]) << 16) |
                    (static_cast<std::uint32_t>(response[pos + 6]) << 8) |
                    static_cast<std::uint32_t>(response[pos + 7]);

                if (attrType == 0x0020) {
                    mappedPort ^= 0x2112;
                    mappedAddress ^= 0x2112a442;
                }

                in_addr address{};
                address.S_un.S_addr = htonl(mappedAddress);
                char text[INET_ADDRSTRLEN]{};
                if (InetNtopA(AF_INET, &address, text, sizeof(text)) != nullptr) {
                    freeaddrinfo(result);
                    return makeEndpoint(text, mappedPort == 0 ? localPort : mappedPort);
                }
            }

            const std::size_t paddedLength = (static_cast<std::size_t>(attrLen) + 3u) &
                ~static_cast<std::size_t>(3u);
            pos += paddedLength;
        }
    }

    freeaddrinfo(result);
    return std::nullopt;
}

std::vector<std::string> localIPv4Addresses() {
    std::vector<std::string> addresses;
    auto addUnique = [&addresses](const std::string& address) {
        if (!address.empty() &&
            std::find(addresses.begin(), addresses.end(), address) == addresses.end()) {
            addresses.push_back(address);
        }
    };

    addUnique("127.0.0.1");

    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname)) != 0) {
        return addresses;
    }

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(hostname, nullptr, &hints, &result) != 0) {
        return addresses;
    }

    for (addrinfo* item = result; item != nullptr; item = item->ai_next) {
        if (item->ai_family != AF_INET || item->ai_addr == nullptr) {
            continue;
        }

        char host[INET_ADDRSTRLEN]{};
        const auto* addr = reinterpret_cast<const sockaddr_in*>(item->ai_addr);
        if (InetNtopA(AF_INET, const_cast<IN_ADDR*>(&addr->sin_addr), host, sizeof(host)) != nullptr) {
            addUnique(host);
        }
    }

    freeaddrinfo(result);
    return addresses;
}
#endif

}  // namespace

struct DirectVoiceTransport::Impl {
    struct Candidate {
        std::string type;
        std::string address;
        std::uint16_t port = 0;
    };

    struct PeerRoute {
        std::vector<Candidate> candidates;
#ifdef TVO_PLATFORM_WINDOWS
        std::vector<SocketEndpoint> endpoints;
        std::optional<SocketEndpoint> learnedEndpoint;
#endif
        bool offerSent = false;
        bool answerSent = false;
        bool noEndpointLogged = false;
        bool learnedEndpointLogged = false;
        bool noConnectivityLogged = false;
        int pingPacketsSent = 0;
        int pingPacketsReceived = 0;
        int voicePacketsSent = 0;
        int voicePacketsReceived = 0;
        int duplicateVoicePacketsDropped = 0;
        int candidateSignalsSent = 0;
        std::uint32_t lastVoiceSequenceReceived = 0;
        Clock::time_point createdAt = Clock::now();
        Clock::time_point lastPingSent = Clock::time_point{};
        Clock::time_point lastHeard = Clock::time_point{};
        Clock::time_point lastCandidateSignalSent = Clock::time_point{};
        bool hasLastVoiceSequenceReceived = false;
    };

    SignalCallback signalCallback;
    FrameCallback frameCallback;
    RoomId roomId;
    PeerId localPeerId;
    std::vector<Candidate> localCandidates;
    std::map<PeerId, PeerRoute> peers;
    bool running = false;
    bool noPeersVoiceLogged = false;

#ifdef TVO_PLATFORM_WINDOWS
    SOCKET socket = INVALID_SOCKET;
    bool wsaStarted = false;
#endif

    bool start(RoomId room, PeerId peer) {
        stop();
        roomId = std::move(room);
        localPeerId = std::move(peer);
        if (roomId.empty() || localPeerId.empty()) {
            return false;
        }
        if (envFlagEnabled("TVO_DISABLE_DIRECT_UDP")) {
            appendP2PLog("direct voice transport disabled by TVO_DISABLE_DIRECT_UDP");
            return false;
        }

#ifdef TVO_PLATFORM_WINDOWS
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            appendP2PLog("WSAStartup failed");
            return false;
        }
        wsaStarted = true;

        socket = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (socket == INVALID_SOCKET) {
            appendP2PLog("UDP socket creation failed");
            stop();
            return false;
        }

        BOOL udpConnReset = FALSE;
        DWORD bytesReturned = 0;
        WSAIoctl(
            socket,
            SIO_UDP_CONNRESET,
            &udpConnReset,
            sizeof(udpConnReset),
            nullptr,
            0,
            &bytesReturned,
            nullptr,
            nullptr);

        auto bindToPort = [this](std::uint16_t port) {
            sockaddr_in bindAddress{};
            bindAddress.sin_family = AF_INET;
            bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);
            bindAddress.sin_port = htons(port);
            return bind(socket, reinterpret_cast<const sockaddr*>(&bindAddress), sizeof(bindAddress)) != SOCKET_ERROR;
        };

        const std::uint16_t preferredPort = preferredDirectVoicePort();
        if (!bindToPort(preferredPort)) {
            appendP2PLog("UDP bind to preferred port " + std::to_string(preferredPort) +
                " failed; falling back to an automatic port");
            if (!bindToPort(0)) {
                appendP2PLog("UDP bind failed");
                stop();
                return false;
            }
        }

        u_long nonBlocking = 1;
        ioctlsocket(socket, FIONBIO, &nonBlocking);

        sockaddr_in localAddress{};
        int localAddressLen = sizeof(localAddress);
        if (getsockname(socket, reinterpret_cast<sockaddr*>(&localAddress), &localAddressLen) == SOCKET_ERROR) {
            appendP2PLog("getsockname failed");
            stop();
            return false;
        }

        const std::uint16_t localPort = ntohs(localAddress.sin_port);
        auto addLocalCandidate = [this](Candidate candidate) {
            const auto exists = std::find_if(
                localCandidates.begin(),
                localCandidates.end(),
                [&](const Candidate& existing) {
                    return existing.type == candidate.type &&
                        existing.address == candidate.address &&
                        existing.port == candidate.port;
                });
            if (exists == localCandidates.end()) {
                localCandidates.push_back(std::move(candidate));
            }
        };

        for (const auto& address : localIPv4Addresses()) {
            addLocalCandidate(Candidate{.type = "host", .address = address, .port = localPort});
        }

        bool mappedCandidateFound = false;
        for (const auto& stun : configuredStunServers()) {
            if (auto mapped = queryStunMappedEndpoint(socket, localPort, stun)) {
                mappedCandidateFound = true;
                addLocalCandidate(Candidate{
                    .type = "srflx",
                    .address = mapped->address,
                    .port = mapped->port});
                appendP2PLog("STUN mapped endpoint via " + stun + " " +
                    mapped->address + ":" + std::to_string(mapped->port));
            } else {
                appendP2PLog("STUN did not return an external endpoint via " + stun);
            }
        }
        bool publicCandidateFound = false;
        if (auto publicAddress = queryPublicIPv4ViaHttps()) {
            publicCandidateFound = true;
            addLocalCandidate(Candidate{
                .type = mappedCandidateFound ? "public" : "srflx",
                .address = *publicAddress,
                .port = localPort});
            appendP2PLog("added HTTPS public UDP candidate " +
                *publicAddress + ":" + std::to_string(localPort));
        }
        if (!mappedCandidateFound && !publicCandidateFound) {
            appendP2PLog("external UDP candidate was not discovered; direct voice can only use LAN/VPN addresses");
        }

        running = true;
        appendP2PLog("direct voice transport started on UDP port " + std::to_string(localPort) +
            " candidates=" + std::to_string(localCandidates.size()));
        for (const auto& candidate : localCandidates) {
            appendP2PLog("local candidate " + candidate.type + " " +
                candidate.address + ":" + std::to_string(candidate.port));
        }
        return true;
#else
        appendP2PLog("direct voice transport is only implemented on Windows");
        return false;
#endif
    }

    void stop() {
#ifdef TVO_PLATFORM_WINDOWS
        if (socket != INVALID_SOCKET) {
            closesocket(socket);
            socket = INVALID_SOCKET;
        }
        if (wsaStarted) {
            WSACleanup();
            wsaStarted = false;
        }
#endif
        localCandidates.clear();
        peers.clear();
        running = false;
        noPeersVoiceLogged = false;
    }

    std::string makeLocalPayloadJson() const {
        std::ostringstream out;
        out << "{\"version\":1,\"transport\":\"udp\",\"candidates\":[";
        for (std::size_t i = 0; i < localCandidates.size(); ++i) {
            if (i != 0) {
                out << ',';
            }
            const auto& candidate = localCandidates[i];
            out << "{\"type\":\"" << escapeJson(candidate.type)
                << "\",\"address\":\"" << escapeJson(candidate.address)
                << "\",\"port\":" << candidate.port << "}";
        }
        out << "]}";
        return out.str();
    }

    std::vector<Candidate> parseCandidates(const std::string& payloadJson) const {
        std::vector<Candidate> candidates;
        const auto candidatesJson = extractBalancedValue(payloadJson, "candidates", '[', ']');
        if (!candidatesJson) {
            return candidates;
        }

        for (const auto& object : splitObjectArray(*candidatesJson)) {
            Candidate candidate;
            candidate.type = extractJsonString(object, "type").value_or("host");
            candidate.address = extractJsonString(object, "address").value_or("");
            candidate.port = static_cast<std::uint16_t>(
                std::clamp(extractJsonNumber(object, "port").value_or(0.0), 0.0, 65535.0));
            if (!candidate.address.empty() && candidate.port != 0) {
                candidates.push_back(std::move(candidate));
            }
        }
        return candidates;
    }

    void ensurePeer(const PeerId& peerId, bool initiateOffer) {
        if (!running || peerId.empty() || peerId == localPeerId) {
            return;
        }

        auto& route = peers[peerId];
        route.createdAt = Clock::now();
        if (initiateOffer && !route.offerSent && signalCallback) {
            route.offerSent = true;
            signalCallback(peerId, "ice_offer", makeLocalPayloadJson());
            appendP2PLog("sent direct voice offer to " + peerId);
        }
    }

    void removePeer(const PeerId& peerId) {
        peers.erase(peerId);
    }

    void handleSignal(
        const PeerId& sourcePeerId,
        const std::string& signalType,
        const std::string& payloadJson) {
        if (!running || sourcePeerId.empty() || sourcePeerId == localPeerId) {
            return;
        }

        auto& route = peers[sourcePeerId];
        if (route.createdAt == Clock::time_point{}) {
            route.createdAt = Clock::now();
        }
        route.candidates = parseCandidates(payloadJson);
        rebuildEndpoints(route);
        route.noEndpointLogged = false;
        route.noConnectivityLogged = false;
        appendP2PLog("received " + signalType + " from " + sourcePeerId +
            " candidates=" + std::to_string(route.candidates.size()));
        for (const auto& candidate : route.candidates) {
            appendP2PLog("remote candidate from " + sourcePeerId + " " +
                candidate.type + " " + candidate.address + ":" + std::to_string(candidate.port));
        }

        if (signalType == "ice_offer" && !route.answerSent && signalCallback) {
            route.answerSent = true;
            signalCallback(sourcePeerId, "ice_answer", makeLocalPayloadJson());
            appendP2PLog("sent direct voice answer to " + sourcePeerId);
        }

        sendPing(route);
    }

    void sendVoiceFrame(const EncodedVoiceFrame& frame) {
        if (!running || frame.payload.empty() || frame.payload.size() > kMaxUdpPayloadBytes) {
            return;
        }

        const auto packet = encodePacket(
            kPacketVoice,
            roomId,
            localPeerId,
            frame.sequence,
            frame.captureUnixMs,
            frame.payload);
        if (packet.empty()) {
            return;
        }

        if (peers.empty()) {
            if (!noPeersVoiceLogged) {
                noPeersVoiceLogged = true;
                appendP2PLog("voice frame captured but no direct peers are known yet");
            }
            return;
        }

        for (auto& [_, route] : peers) {
            route.voicePacketsSent += 1;
            if (route.voicePacketsSent <= 3 || route.voicePacketsSent % 50 == 0) {
                appendP2PLog("sent UDP voice packet to peer route packets=" +
                    std::to_string(route.voicePacketsSent) +
                    " endpoints=" + std::to_string(route.endpoints.size()) +
                    (route.learnedEndpoint ? " learned=1" : " learned=0"));
            }
            sendPacket(route, packet);
        }
    }

    bool hasPeerConnection(const PeerId& peerId) const {
        const auto found = peers.find(peerId);
        return found != peers.end() && found->second.lastHeard != Clock::time_point{};
    }

    bool hasAnyPeerConnection() const {
        return std::any_of(peers.begin(), peers.end(), [](const auto& item) {
            return item.second.lastHeard != Clock::time_point{};
        });
    }

    void pump() {
        if (!running) {
            return;
        }

        drainSocket();
        const auto now = Clock::now();
        for (auto& [peerId, route] : peers) {
            if (now - route.lastPingSent > 750ms) {
                sendPing(route);
            }
            if (signalCallback &&
                route.lastHeard == Clock::time_point{} &&
                route.candidateSignalsSent < 8 &&
                now - route.createdAt > 2s &&
                (route.lastCandidateSignalSent == Clock::time_point{} ||
                    now - route.lastCandidateSignalSent > 3s)) {
                route.lastCandidateSignalSent = now;
                route.candidateSignalsSent += 1;
                signalCallback(peerId, "ice_candidate", makeLocalPayloadJson());
                appendP2PLog("resent direct voice candidates to " + peerId +
                    " attempt=" + std::to_string(route.candidateSignalsSent));
            }
            if (!route.noConnectivityLogged &&
                route.lastHeard == Clock::time_point{} &&
                now - route.createdAt > 8s) {
                route.noConnectivityLogged = true;
                appendP2PLog("no UDP packets received from a peer after 8 seconds; check Windows Firewall/NAT/UDP 40771");
            }
        }
    }

    void rebuildEndpoints(PeerRoute& route) {
#ifdef TVO_PLATFORM_WINDOWS
        route.endpoints.clear();
        for (const auto& candidate : route.candidates) {
            if (auto endpoint = makeEndpoint(candidate.address, candidate.port)) {
                if (std::find_if(route.endpoints.begin(), route.endpoints.end(), [&](const SocketEndpoint& item) {
                        return sameEndpoint(item, *endpoint);
                    }) == route.endpoints.end()) {
                    route.endpoints.push_back(*endpoint);
                }
            }
        }
#endif
    }

    void sendPing(PeerRoute& route) {
        route.lastPingSent = Clock::now();
        route.pingPacketsSent += 1;
        const auto packet = encodePacket(kPacketPing, roomId, localPeerId, 0, 0, {});
        sendPacket(route, packet);
    }

    void sendPacket(PeerRoute& route, const std::vector<std::uint8_t>& packet) {
#ifdef TVO_PLATFORM_WINDOWS
        if (socket == INVALID_SOCKET || packet.empty()) {
            return;
        }

        if (!route.learnedEndpoint && route.endpoints.empty()) {
            if (!route.noEndpointLogged) {
                route.noEndpointLogged = true;
                appendP2PLog("cannot send UDP packet: peer has no usable endpoints");
            }
            return;
        }

        auto sendOne = [this, &packet](const SocketEndpoint& endpoint) {
            sendto(
                socket,
                reinterpret_cast<const char*>(packet.data()),
                static_cast<int>(packet.size()),
                0,
                reinterpret_cast<const sockaddr*>(&endpoint.storage),
                endpoint.length);
        };

        if (route.learnedEndpoint) {
            sendOne(*route.learnedEndpoint);
            return;
        }

        for (const auto& endpoint : route.endpoints) {
            sendOne(endpoint);
        }
#endif
    }

    void drainSocket() {
#ifdef TVO_PLATFORM_WINDOWS
        if (socket == INVALID_SOCKET) {
            return;
        }

        for (;;) {
            std::array<std::uint8_t, 1500> buffer{};
            sockaddr_storage from{};
            int fromLen = sizeof(from);
            const int received = recvfrom(
                socket,
                reinterpret_cast<char*>(buffer.data()),
                static_cast<int>(buffer.size()),
                0,
                reinterpret_cast<sockaddr*>(&from),
                &fromLen);

            if (received == SOCKET_ERROR) {
                const int error = WSAGetLastError();
                if (error != WSAEWOULDBLOCK) {
                    appendP2PLog("UDP receive failed: " + std::to_string(error));
                }
                return;
            }

            auto decoded = decodePacket(buffer.data(), received);
            if (!decoded || decoded->roomId != roomId ||
                decoded->sourcePeerId.empty() ||
                decoded->sourcePeerId == localPeerId) {
                continue;
            }

            auto endpoint = endpointFromSockaddr(from, fromLen);
            auto& route = peers[decoded->sourcePeerId];
            if (endpoint) {
                route.learnedEndpoint = *endpoint;
                if (!route.learnedEndpointLogged) {
                    route.learnedEndpointLogged = true;
                    appendP2PLog("learned peer UDP endpoint " + endpoint->address +
                        ":" + std::to_string(endpoint->port));
                }
            }
            route.lastHeard = Clock::now();

            if (decoded->type == kPacketPing) {
                route.pingPacketsReceived += 1;
                if (route.pingPacketsReceived <= 3 || route.pingPacketsReceived % 50 == 0) {
                    appendP2PLog("received UDP ping from " + decoded->sourcePeerId +
                        " count=" + std::to_string(route.pingPacketsReceived));
                }
            }

            if (decoded->type == kPacketVoice && frameCallback) {
                if (route.hasLastVoiceSequenceReceived &&
                    decoded->sequence <= route.lastVoiceSequenceReceived) {
                    route.duplicateVoicePacketsDropped += 1;
                    if (route.duplicateVoicePacketsDropped <= 3 ||
                        route.duplicateVoicePacketsDropped % 50 == 0) {
                        appendP2PLog("dropped duplicate UDP voice from " +
                            decoded->sourcePeerId +
                            " seq=" + std::to_string(decoded->sequence));
                    }
                    continue;
                }
                route.hasLastVoiceSequenceReceived = true;
                route.lastVoiceSequenceReceived = decoded->sequence;
                route.voicePacketsReceived += 1;
                if (route.voicePacketsReceived <= 3 || route.voicePacketsReceived % 50 == 0) {
                    appendP2PLog("received UDP voice from " + decoded->sourcePeerId +
                        " packets=" + std::to_string(route.voicePacketsReceived));
                }
                EncodedVoiceFrame frame;
                frame.sourcePeerId = decoded->sourcePeerId;
                frame.sequence = decoded->sequence;
                frame.captureUnixMs = decoded->captureUnixMs;
                frame.payload = std::move(decoded->payload);
                frameCallback(frame);
            }
        }
#endif
    }
};

DirectVoiceTransport::DirectVoiceTransport()
    : impl_(std::make_unique<Impl>()) {}

DirectVoiceTransport::~DirectVoiceTransport() {
    stop();
}

void DirectVoiceTransport::setSignalCallback(SignalCallback callback) {
    impl_->signalCallback = std::move(callback);
}

void DirectVoiceTransport::setFrameCallback(FrameCallback callback) {
    impl_->frameCallback = std::move(callback);
}

bool DirectVoiceTransport::start(RoomId roomId, PeerId localPeerId) {
    return impl_->start(std::move(roomId), std::move(localPeerId));
}

void DirectVoiceTransport::stop() {
    impl_->stop();
}

void DirectVoiceTransport::pump() {
    impl_->pump();
}

void DirectVoiceTransport::ensurePeer(const PeerId& peerId, bool initiateOffer) {
    impl_->ensurePeer(peerId, initiateOffer);
}

void DirectVoiceTransport::removePeer(const PeerId& peerId) {
    impl_->removePeer(peerId);
}

void DirectVoiceTransport::handleSignal(
    const PeerId& sourcePeerId,
    const std::string& signalType,
    const std::string& payloadJson) {
    impl_->handleSignal(sourcePeerId, signalType, payloadJson);
}

void DirectVoiceTransport::sendVoiceFrame(const EncodedVoiceFrame& frame) {
    impl_->sendVoiceFrame(frame);
}

bool DirectVoiceTransport::running() const noexcept {
    return impl_->running;
}

bool DirectVoiceTransport::hasPeerConnection(const PeerId& peerId) const {
    return impl_->hasPeerConnection(peerId);
}

bool DirectVoiceTransport::hasAnyPeerConnection() const {
    return impl_->hasAnyPeerConnection();
}

}  // namespace tvo
