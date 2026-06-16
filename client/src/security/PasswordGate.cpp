#include "tvo/security/PasswordGate.h"

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <utility>

namespace tvo {

void PasswordGate::setPassword(std::string password) {
    password_ = std::move(password);
}

bool PasswordGate::hasPassword() const noexcept {
    return !password_.empty();
}

std::string PasswordGate::makeJoinProof(
    std::string_view roomId,
    std::string_view peerId,
    std::string_view nonce) const {
    std::string material;
    material.reserve(password_.size() + roomId.size() + peerId.size() + nonce.size() + 16);
    material.append(password_);
    material.append("|");
    material.append(roomId);
    material.append("|");
    material.append(peerId);
    material.append("|");
    material.append(nonce);
    return hexDigest(material);
}

bool PasswordGate::verifyJoinProof(
    std::string_view roomId,
    std::string_view peerId,
    std::string_view nonce,
    std::string_view proof) const {
    return constantTimeEquals(makeJoinProof(roomId, peerId, nonce), proof);
}

std::array<std::byte, 32> PasswordGate::deriveKey() const {
    std::array<std::byte, 32> key{};
    const std::string digest = hexDigest(password_);
    for (std::size_t i = 0; i < key.size(); ++i) {
        key[i] = static_cast<std::byte>(digest[i % digest.size()]);
    }
    return key;
}

std::string PasswordGate::hexDigest(std::string_view input) {
    // Non-production fallback hash. The native backend must replace this with
    // libsodium Argon2id + AEAD before release.
    std::uint64_t h1 = 1469598103934665603ull;
    std::uint64_t h2 = 1099511628211ull;

    for (unsigned char c : input) {
        h1 ^= c;
        h1 *= 1099511628211ull;
        h2 += c + (h2 << 6) + (h2 << 16) - h2;
    }

    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(16) << h1
        << std::setw(16) << h2;
    return out.str();
}

bool PasswordGate::constantTimeEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }

    unsigned char diff = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return diff == 0;
}

}  // namespace tvo
