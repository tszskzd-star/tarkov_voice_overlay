#include "tvo/net/SignalingClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#ifdef TVO_PLATFORM_WINDOWS
#include <windows.h>
#include <winhttp.h>
#endif

namespace tvo {

namespace {

using namespace std::chrono_literals;

std::int64_t nowUnixMs() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void appendSignalingLog(const std::string& line) {
    std::error_code ignored;
    std::filesystem::create_directories("logs", ignored);
    std::ofstream out("logs/signaling.log", std::ios::out | std::ios::app);
    if (out) {
        out << nowUnixMs() << " " << line << "\n";
    }
}

std::string makeStableId(const std::string& prefix, const std::string& seed) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : seed) {
        h ^= c;
        h *= 1099511628211ull;
    }

    std::ostringstream out;
    out << prefix << "_" << std::hex << h;
    return out.str();
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
            if (c < 0x20) {
                out += ' ';
            } else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    return out;
}

std::wstring toWide(const std::string& text) {
    if (text.empty()) {
        return {};
    }

#ifdef TVO_PLATFORM_WINDOWS
    const int required = MultiByteToWideChar(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (required <= 0) {
        return {};
    }

    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
        wide.data(), required);
    return wide;
#else
    return std::wstring(text.begin(), text.end());
#endif
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

std::optional<bool> extractJsonBool(const std::string& json, const std::string& key) {
    const auto valuePos = findJsonValue(json, key);
    if (!valuePos) {
        return std::nullopt;
    }

    if (json.compare(*valuePos, 4, "true") == 0) {
        return true;
    }
    if (json.compare(*valuePos, 5, "false") == 0) {
        return false;
    }
    return std::nullopt;
}

std::string base64Encode(const std::vector<std::uint8_t>& bytes) {
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t b0 = bytes[i];
        const std::uint32_t b1 = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t b2 = i + 2 < bytes.size() ? bytes[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;

        out.push_back(alphabet[(triple >> 18) & 0x3f]);
        out.push_back(alphabet[(triple >> 12) & 0x3f]);
        out.push_back(i + 1 < bytes.size() ? alphabet[(triple >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < bytes.size() ? alphabet[triple & 0x3f] : '=');
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> base64Decode(const std::string& text) {
    auto valueOf = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z') {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9') {
            return c - '0' + 52;
        }
        if (c == '+') {
            return 62;
        }
        if (c == '/') {
            return 63;
        }
        return -1;
    };

    std::vector<std::uint8_t> out;
    out.reserve((text.size() / 4) * 3);
    int value = 0;
    int bits = -8;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            continue;
        }
        if (c == '=') {
            break;
        }
        const int decoded = valueOf(c);
        if (decoded < 0) {
            return std::nullopt;
        }
        value = (value << 6) | decoded;
        bits += 6;
        if (bits >= 0) {
            out.push_back(static_cast<std::uint8_t>((value >> bits) & 0xff));
            bits -= 8;
        }
    }
    return out;
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

std::vector<std::string> parseStringArray(const std::string& json, const std::string& key) {
    std::vector<std::string> values;
    const auto arrayJson = extractBalancedValue(json, key, '[', ']');
    if (!arrayJson) {
        return values;
    }

    bool inString = false;
    bool escaped = false;
    std::string current;
    for (std::size_t i = 1; i + 1 < arrayJson->size(); ++i) {
        const char c = (*arrayJson)[i];
        if (!inString) {
            if (c == '"') {
                inString = true;
                current.clear();
            }
            continue;
        }

        if (escaped) {
            switch (c) {
            case 'n':
                current += '\n';
                break;
            case 'r':
                current += '\r';
                break;
            case 't':
                current += '\t';
                break;
            default:
                current += c;
                break;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            inString = false;
            values.push_back(current);
        } else {
            current += c;
        }
    }

    return values;
}

RoomInfo parseRoom(const std::string& json) {
    RoomInfo room;
    room.id = extractJsonString(json, "id").value_or("");
    room.name = extractJsonString(json, "name").value_or("Squad room");
    room.hostPeerId = extractJsonString(json, "hostPeerId").value_or("");
    room.hostNick = extractJsonString(json, "hostNick").value_or("");
    room.peerCount = static_cast<std::size_t>(
        std::max(0.0, extractJsonNumber(json, "peerCount").value_or(0.0)));
    room.maxPeers = static_cast<std::size_t>(
        std::max(1.0, extractJsonNumber(json, "maxPeers").value_or(
            static_cast<double>(kMaxPeersPerRoom))));
    room.locked = extractJsonBool(json, "locked").value_or(false);
    room.lanOnly = extractJsonBool(json, "lanOnly").value_or(false);
    room.createdAtUnixMs = static_cast<std::int64_t>(
        extractJsonNumber(json, "createdAtUnixMs").value_or(0.0));
    if (room.maxPeers == 0 || room.maxPeers > kMaxPeersPerRoom) {
        room.maxPeers = kMaxPeersPerRoom;
    }
    room.peerIds = parseStringArray(json, "peers");
    return room;
}

std::vector<RoomInfo> parseRooms(const std::string& json) {
    std::vector<RoomInfo> rooms;
    const auto roomsJson = extractBalancedValue(json, "rooms", '[', ']');
    if (!roomsJson) {
        return rooms;
    }

    for (const auto& object : splitObjectArray(*roomsJson)) {
        RoomInfo room = parseRoom(object);
        if (!room.id.empty()) {
            rooms.push_back(std::move(room));
        }
    }
    return rooms;
}

std::array<float, kSpectrumBands> parseSpectrum(const std::string& json) {
    std::array<float, kSpectrumBands> spectrum{};
    const auto value = extractBalancedValue(json, "spectrum", '[', ']');
    if (!value) {
        return spectrum;
    }

    std::size_t pos = 1;
    for (std::size_t i = 0; i < spectrum.size() && pos < value->size(); ++i) {
        pos = skipWhitespace(*value, pos);
        const char* start = value->c_str() + pos;
        char* end = nullptr;
        const double parsed = std::strtod(start, &end);
        if (end == start) {
            break;
        }
        spectrum[i] = std::clamp(static_cast<float>(parsed), 0.0f, 1.0f);
        pos = static_cast<std::size_t>(end - value->c_str());
        const std::size_t comma = value->find(',', pos);
        if (comma == std::string::npos) {
            break;
        }
        pos = comma + 1;
    }
    return spectrum;
}

PeerStatus parsePeerStatus(const std::string& envelope) {
    PeerStatus peer;
    peer.id = extractJsonString(envelope, "peerId").value_or("");
    peer.nick = extractJsonString(envelope, "nick").value_or("Player");
    peer.mediaState = PeerMediaState::Silent;
    peer.lastSeen = Clock::now();

    const auto payload = extractBalancedValue(envelope, "payload", '{', '}');
    const std::string& data = payload ? *payload : envelope;
    peer.iconIndex = std::clamp(
        static_cast<int>(extractJsonNumber(data, "iconIndex").value_or(0.0)), 0, 4);
    peer.muted = extractJsonBool(data, "muted").value_or(false);
    peer.micLevel = std::clamp(
        static_cast<float>(extractJsonNumber(data, "micLevel").value_or(0.0)), 0.0f, 1.0f);
    peer.speaking = extractJsonBool(data, "speaking").value_or(peer.micLevel > 0.04f);
    peer.spectrum = parseSpectrum(data);
    peer.mediaState = peer.muted ? PeerMediaState::Muted :
        (peer.speaking ? PeerMediaState::Speaking : PeerMediaState::Silent);
    return peer;
}

std::string makeRoomPayload(const CreateRoomRequest& request) {
    std::ostringstream out;
    out << "{\"id\":\"\",\"name\":\"" << escapeJson(request.roomName)
        << "\",\"hostPeerId\":\"\",\"hostNick\":\"" << escapeJson(request.hostNick)
        << "\",\"peerCount\":1,\"maxPeers\":" << kMaxPeersPerRoom
        << ",\"locked\":" << (request.locked ? "true" : "false")
        << ",\"lanOnly\":false,\"createdAtUnixMs\":" << nowUnixMs() << "}";
    return out.str();
}

std::string makePeerStatusPayload(const PeerStatus& status) {
    std::ostringstream out;
    out << "{\"iconIndex\":" << std::clamp(status.iconIndex, 0, 4)
        << ",\"muted\":" << (status.muted ? "true" : "false")
        << ",\"speaking\":" << (status.speaking ? "true" : "false")
        << ",\"micLevel\":" << std::clamp(status.micLevel, 0.0f, 1.0f)
        << ",\"spectrum\":[";
    for (std::size_t i = 0; i < status.spectrum.size(); ++i) {
        if (i != 0) {
            out << ',';
        }
        out << std::clamp(status.spectrum[i], 0.0f, 1.0f);
    }
    out << "]}";
    return out.str();
}

struct ParsedUrl {
    bool secure = false;
    std::string host;
    std::string path = "/";
    unsigned short port = 80;
};

std::optional<ParsedUrl> parseWebSocketUrl(const std::string& url) {
    ParsedUrl parsed;
    std::string rest;
    if (url.rfind("wss://", 0) == 0) {
        parsed.secure = true;
        parsed.port = 443;
        rest = url.substr(6);
    } else if (url.rfind("ws://", 0) == 0) {
        parsed.secure = false;
        parsed.port = 80;
        rest = url.substr(5);
    } else if (url.rfind("https://", 0) == 0) {
        parsed.secure = true;
        parsed.port = 443;
        rest = url.substr(8);
    } else if (url.rfind("http://", 0) == 0) {
        parsed.secure = false;
        parsed.port = 80;
        rest = url.substr(7);
    } else {
        return std::nullopt;
    }

    const std::size_t slash = rest.find('/');
    std::string hostPort = slash == std::string::npos ? rest : rest.substr(0, slash);
    parsed.path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (parsed.path.empty()) {
        parsed.path = "/";
    }

    const std::size_t colon = hostPort.rfind(':');
    if (colon != std::string::npos && hostPort.find(']') == std::string::npos) {
        parsed.host = hostPort.substr(0, colon);
        parsed.port = static_cast<unsigned short>(std::stoi(hostPort.substr(colon + 1)));
    } else {
        parsed.host = hostPort;
    }

    return parsed.host.empty() ? std::nullopt : std::optional<ParsedUrl>(parsed);
}

}  // namespace

void SignalingClient::setEventCallback(EventCallback callback) {
    callback_ = std::move(callback);
}

bool SignalingClient::connect(std::string url, std::string nick) {
    disconnect();

    url_ = std::move(url);
    nick_ = std::move(nick);
    peerId_ = makeStableId("peer", nick_ + url_);
    stopRequested_ = false;
    appendSignalingLog("connecting to " + url_);

    {
        std::lock_guard lock(mutex_);
        rooms_.clear();
        events_.clear();
        pendingCreatedRoom_.reset();
        pendingJoinedRoom_.reset();
        pendingError_.reset();
        currentRoomId_.reset();
        receivedHello_ = false;
        receivedRooms_ = false;
    }

#ifdef TVO_PLATFORM_WINDOWS
    const auto parsed = parseWebSocketUrl(url_);
    if (!parsed) {
        queueEvent(SignalingEvent{.type = "error", .error = "bad coordinator url"});
        return false;
    }

    HINTERNET session = WinHttpOpen(
        L"TarkovVoiceOverlay/0.1",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (session == nullptr) {
        queueEvent(SignalingEvent{.type = "error", .error = "WinHTTP session failed"});
        return false;
    }

    const std::wstring host = toWide(parsed->host);
    const std::wstring path = toWide(parsed->path);
    HINTERNET connection = WinHttpConnect(session, host.c_str(), parsed->port, 0);
    if (connection == nullptr) {
        WinHttpCloseHandle(session);
        queueEvent(SignalingEvent{.type = "error", .error = "coordinator connect failed"});
        return false;
    }

    const DWORD flags = parsed->secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = WinHttpOpenRequest(
        connection,
        L"GET",
        path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (request == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        queueEvent(SignalingEvent{.type = "error", .error = "websocket request failed"});
        return false;
    }

    if (!WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) ||
        !WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, nullptr)) {
        appendSignalingLog("websocket upgrade failed before HTTP status");
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        queueEvent(SignalingEvent{.type = "error", .error = "websocket upgrade failed"});
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    if (WinHttpQueryHeaders(
            request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusCodeSize,
            WINHTTP_NO_HEADER_INDEX)) {
        appendSignalingLog("websocket upgrade HTTP status=" + std::to_string(statusCode));
    }

    HINTERNET websocket = WinHttpWebSocketCompleteUpgrade(request, 0);
    WinHttpCloseHandle(request);
    if (websocket == nullptr) {
        WinHttpCloseHandle(connection);
        WinHttpCloseHandle(session);
        const std::string error = statusCode == 0
            ? "websocket completion failed"
            : "websocket completion failed, HTTP status=" + std::to_string(statusCode);
        queueEvent(SignalingEvent{.type = "error", .error = error});
        return false;
    }

    sessionHandle_ = session;
    connectionHandle_ = connection;
    websocketHandle_ = websocket;
    connected_ = true;
    appendSignalingLog("websocket connected");
    receiverThread_ = std::thread([this] { receiveLoop(); });

    std::ostringstream hello;
    hello << "{\"type\":\"hello\",\"nick\":\"" << escapeJson(nick_) << "\"}";
    if (!sendText(hello.str())) {
        disconnect();
        return false;
    }

    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, 5s, [this] {
        return stopRequested_.load() || (receivedHello_ && receivedRooms_);
    });

    if (!receivedHello_) {
        lock.unlock();
        appendSignalingLog("coordinator did not answer hello");
        queueEvent(SignalingEvent{.type = "error", .error = "coordinator did not answer hello"});
        disconnect();
        return false;
    }

    return true;
#else
    connected_ = false;
    queueEvent(SignalingEvent{.type = "error", .error = "websocket is only implemented on Windows"});
    return false;
#endif
}

void SignalingClient::disconnect() {
    stopRequested_ = true;
    connected_ = false;

    closeHandles();

    if (receiverThread_.joinable()) {
        receiverThread_.join();
    }

    {
        std::lock_guard lock(mutex_);
        currentRoomId_.reset();
        pendingCreatedRoom_.reset();
        pendingJoinedRoom_.reset();
        pendingError_.reset();
        receivedHello_ = false;
        receivedRooms_ = false;
    }
}

bool SignalingClient::connected() const noexcept {
    return connected_.load();
}

const PeerId& SignalingClient::peerId() const noexcept {
    return peerId_;
}

std::vector<RoomInfo> SignalingClient::listRooms() const {
    std::lock_guard lock(mutex_);
    return rooms_;
}

std::optional<RoomInfo> SignalingClient::createRoom(const CreateRoomRequest& request) {
    if (!connected_) {
        return std::nullopt;
    }

    {
        std::lock_guard lock(mutex_);
        pendingCreatedRoom_.reset();
        pendingError_.reset();
    }

    std::ostringstream text;
    text << "{\"type\":\"create_room\",\"nick\":\"" << escapeJson(request.hostNick)
         << "\",\"room\":" << makeRoomPayload(request) << "}";
    if (!sendText(text.str())) {
        return std::nullopt;
    }

    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, 5s, [this] {
        return pendingCreatedRoom_.has_value() || pendingError_.has_value() || !connected_.load();
    });
    if (!pendingCreatedRoom_) {
        return std::nullopt;
    }

    RoomInfo room = *pendingCreatedRoom_;
    currentRoomId_ = room.id;
    pendingCreatedRoom_.reset();
    appendSignalingLog("created room " + room.id + " peers=" + std::to_string(room.peerCount));
    return room;
}

bool SignalingClient::joinRoom(const JoinRoomRequest& request) {
    if (!connected_) {
        return false;
    }

    {
        std::lock_guard lock(mutex_);
        pendingJoinedRoom_.reset();
        pendingError_.reset();
    }

    std::ostringstream text;
    text << "{\"type\":\"join_room\",\"roomId\":\"" << escapeJson(request.roomId)
         << "\",\"nick\":\"" << escapeJson(request.nick)
         << "\",\"payload\":{\"passwordProof\":\"" << escapeJson(request.passwordProof)
         << "\",\"publicKey\":\"" << escapeJson(request.publicKey) << "\"}}";
    if (!sendText(text.str())) {
        return false;
    }

    std::unique_lock lock(mutex_);
    cv_.wait_for(lock, 5s, [this] {
        return pendingJoinedRoom_.has_value() || pendingError_.has_value() || !connected_.load();
    });
    if (!pendingJoinedRoom_) {
        return false;
    }

    currentRoomId_ = pendingJoinedRoom_->id;
    appendSignalingLog("joined room " + pendingJoinedRoom_->id +
        " peers=" + std::to_string(pendingJoinedRoom_->peerCount));
    pendingJoinedRoom_.reset();
    return true;
}

void SignalingClient::leaveRoom() {
    if (connected_) {
        sendText("{\"type\":\"leave_room\"}");
    }
    std::lock_guard lock(mutex_);
    currentRoomId_.reset();
}

void SignalingClient::sendIce(const IceMessage& message) {
    if (!connected_) {
        return;
    }

    std::ostringstream text;
    text << "{\"type\":\"" << escapeJson(message.type)
         << "\",\"roomId\":\"" << escapeJson(message.roomId)
         << "\",\"targetPeerId\":\"" << escapeJson(message.targetPeerId)
         << "\",\"payload\":" << (message.payloadJson.empty() ? "{}" : message.payloadJson)
         << "}";
    sendText(text.str());
}

void SignalingClient::sendVoiceFrame(const RoomId& roomId, const EncodedVoiceFrame& frame) {
    if (!connected_ || roomId.empty() || frame.payload.empty()) {
        return;
    }

    std::ostringstream text;
    text << "{\"type\":\"voice_frame\",\"roomId\":\"" << escapeJson(roomId)
         << "\",\"payload\":{\"sequence\":" << frame.sequence
         << ",\"captureUnixMs\":" << frame.captureUnixMs
         << ",\"audio\":\"" << base64Encode(frame.payload) << "\"}}";
    sendText(text.str());
}

void SignalingClient::broadcastStatus(const PeerStatus& status) {
    if (!connected_) {
        return;
    }

    std::ostringstream text;
    text << "{\"type\":\"peer_status\",\"nick\":\"" << escapeJson(status.nick)
         << "\",\"payload\":" << makePeerStatusPayload(status) << "}";
    sendText(text.str());
}

void SignalingClient::pump() {
    std::deque<SignalingEvent> events;
    {
        std::lock_guard lock(mutex_);
        events.swap(events_);
    }

    for (auto& event : events) {
        emit(std::move(event));
    }
}

void SignalingClient::emit(SignalingEvent event) const {
    if (callback_) {
        callback_(std::move(event));
    }
}

bool SignalingClient::sendText(const std::string& text) {
#ifdef TVO_PLATFORM_WINDOWS
    std::lock_guard sendLock(sendMutex_);
    if (!connected_ || websocketHandle_ == nullptr) {
        return false;
    }

    const DWORD result = WinHttpWebSocketSend(
        static_cast<HINTERNET>(websocketHandle_),
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(text.data()),
        static_cast<DWORD>(text.size()));
    if (result != NO_ERROR) {
        queueEvent(SignalingEvent{.type = "error", .error = "websocket send failed"});
        return false;
    }
    return true;
#else
    (void)text;
    return false;
#endif
}

void SignalingClient::receiveLoop() {
#ifdef TVO_PLATFORM_WINDOWS
    std::string message;
    std::array<char, 8192> buffer{};

    while (!stopRequested_) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
        const DWORD result = WinHttpWebSocketReceive(
            static_cast<HINTERNET>(websocketHandle_),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &bytesRead,
            &type);

        if (result != NO_ERROR) {
            if (!stopRequested_) {
                queueEvent(SignalingEvent{.type = "error", .error = "websocket receive failed"});
            }
            break;
        }

        if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            break;
        }

        if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            message.append(buffer.data(), buffer.data() + bytesRead);
            if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                handleServerMessage(message);
                message.clear();
            }
        }
    }

    connected_ = false;
    cv_.notify_all();
#endif
}

void SignalingClient::handleServerMessage(const std::string& text) {
    const std::string type = extractJsonString(text, "type").value_or("");
    if (type.empty()) {
        return;
    }

    SignalingEvent event;
    event.type = type;

    if (type == "hello") {
        const std::string serverPeerId = extractJsonString(text, "peerId").value_or("");
        {
            std::lock_guard lock(mutex_);
            if (!serverPeerId.empty()) {
                peerId_ = serverPeerId;
            }
            receivedHello_ = true;
        }
        queueEvent(SignalingEvent{.type = "connected"});
        cv_.notify_all();
        return;
    }

    if (type == "rooms") {
        auto parsedRooms = parseRooms(text);
        {
            std::lock_guard lock(mutex_);
            rooms_ = std::move(parsedRooms);
            receivedRooms_ = true;
            appendSignalingLog("rooms received count=" + std::to_string(rooms_.size()));
        }
        cv_.notify_all();
        return;
    }

    if (type == "room_created" || type == "joined_room") {
        const auto roomJson = extractBalancedValue(text, "room", '{', '}');
        if (roomJson) {
            event.room = parseRoom(*roomJson);
            {
                std::lock_guard lock(mutex_);
                currentRoomId_ = event.room.id;
                if (type == "room_created") {
                    pendingCreatedRoom_ = event.room;
                } else {
                    pendingJoinedRoom_ = event.room;
                }
            }
            queueEvent(event);
            cv_.notify_all();
        }
        return;
    }

    if (type == "peer_status" || type == "peer_joined" || type == "peer_left") {
        event.peer = parsePeerStatus(text);
        if (event.peer.id.empty()) {
            event.peer.id = extractJsonString(text, "peerId").value_or("");
        }
        queueEvent(event);
        return;
    }

    if (type == "ice_offer" || type == "ice_answer" || type == "ice_candidate") {
        event.ice.roomId = extractJsonString(text, "roomId").value_or("");
        event.ice.sourcePeerId = extractJsonString(text, "peerId").value_or("");
        event.ice.targetPeerId = extractJsonString(text, "targetPeerId").value_or("");
        event.ice.type = type;
        event.ice.payloadJson = extractBalancedValue(text, "payload", '{', '}').value_or("{}");
        queueEvent(event);
        return;
    }

    if (type == "voice_frame") {
        const auto payloadJson = extractBalancedValue(text, "payload", '{', '}').value_or("{}");
        event.voiceFrame.sourcePeerId = extractJsonString(text, "peerId").value_or("");
        event.voiceFrame.sequence = static_cast<std::uint32_t>(
            std::max(0.0, extractJsonNumber(payloadJson, "sequence").value_or(0.0)));
        event.voiceFrame.captureUnixMs = static_cast<std::int64_t>(
            std::max(0.0, extractJsonNumber(payloadJson, "captureUnixMs").value_or(0.0)));
        if (auto audio = extractJsonString(payloadJson, "audio")) {
            if (auto decoded = base64Decode(*audio)) {
                event.voiceFrame.payload = std::move(*decoded);
            }
        }
        if (!event.voiceFrame.sourcePeerId.empty() && !event.voiceFrame.payload.empty()) {
            queueEvent(event);
        }
        return;
    }

    if (type == "room_closed") {
        event.room.id = extractJsonString(text, "roomId").value_or("");
        queueEvent(event);
        return;
    }

    if (type == "error") {
        event.error = extractJsonString(text, "error").value_or("signaling error");
        {
            std::lock_guard lock(mutex_);
            pendingError_ = event.error;
        }
        queueEvent(event);
        cv_.notify_all();
    }
}

void SignalingClient::queueEvent(SignalingEvent event) {
    if (event.type == "error") {
        appendSignalingLog("error: " + event.error);
    }
    {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }
    cv_.notify_all();
}

void SignalingClient::closeHandles() {
#ifdef TVO_PLATFORM_WINDOWS
    std::lock_guard sendLock(sendMutex_);
    if (websocketHandle_ != nullptr) {
        WinHttpCloseHandle(static_cast<HINTERNET>(websocketHandle_));
        websocketHandle_ = nullptr;
    }
    if (connectionHandle_ != nullptr) {
        WinHttpCloseHandle(static_cast<HINTERNET>(connectionHandle_));
        connectionHandle_ = nullptr;
    }
    if (sessionHandle_ != nullptr) {
        WinHttpCloseHandle(static_cast<HINTERNET>(sessionHandle_));
        sessionHandle_ = nullptr;
    }
#endif
}

}  // namespace tvo
