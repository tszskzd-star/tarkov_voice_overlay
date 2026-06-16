#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace tvo {

class PasswordGate {
public:
    void setPassword(std::string password);
    [[nodiscard]] bool hasPassword() const noexcept;
    [[nodiscard]] std::string makeJoinProof(
        std::string_view roomId,
        std::string_view peerId,
        std::string_view nonce) const;
    [[nodiscard]] bool verifyJoinProof(
        std::string_view roomId,
        std::string_view peerId,
        std::string_view nonce,
        std::string_view proof) const;

private:
    [[nodiscard]] std::array<std::byte, 32> deriveKey() const;
    [[nodiscard]] static std::string hexDigest(std::string_view input);
    [[nodiscard]] static bool constantTimeEquals(std::string_view a, std::string_view b);

    std::string password_;
};

}  // namespace tvo

