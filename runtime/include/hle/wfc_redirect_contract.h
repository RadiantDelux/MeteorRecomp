#pragma once

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace WfcRedirectContract {

inline constexpr uint32_t kMeteorGameCode = 0x52445350u; // RDSP
inline constexpr uint16_t kMeteorMakerCode = 0x4146u;    // AF

inline bool IsMeteorWiimmfiTitle(uint32_t gameCode, uint16_t makerCode) {
    return gameCode == kMeteorGameCode && makerCode == kMeteorMakerCode;
}

inline char LowerAscii(char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
}

inline std::string Lower(std::string_view text) {
    std::string lowered(text);
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), LowerAscii);
    return lowered;
}

inline bool EndsWith(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

// Equivalent to the domain/http portion of Wiimm's generic --wiimmfi patch,
// but applied at the host-network boundary so the retail DOL remains intact.
inline std::string RewriteNintendoHostname(std::string_view hostname) {
    std::string rewritten = Lower(hostname);
    constexpr std::string_view oldDomain = "nintendowifi.net";
    constexpr std::string_view newDomain = "wiimmfi.de";

    if (rewritten == oldDomain) {
        rewritten.assign(newDomain);
    } else if (EndsWith(rewritten, oldDomain) &&
               rewritten.size() > oldDomain.size() &&
               rewritten[rewritten.size() - oldDomain.size() - 1] == '.') {
        rewritten.replace(rewritten.size() - oldDomain.size(), oldDomain.size(), newDomain);
    } else if (rewritten == "sake.gamespy.com") {
        rewritten = "sake.wiimmfi.de";
    }

    if (rewritten.rfind("naswii.", 0) == 0) {
        rewritten.replace(0, 7, "nas.");
    }
    return rewritten;
}

inline bool IsPlaintextReplacementWfcHost(std::string_view hostname) {
    const std::string lowered = Lower(hostname);
    return lowered.rfind("nas.", 0) == 0 ||
           lowered.rfind("naswii.", 0) == 0 ||
           lowered.rfind("sake.gs.", 0) == 0 ||
           lowered.find(".sake.gs.") != std::string::npos ||
           lowered.rfind("gamestats.gs.", 0) == 0 ||
           lowered.find(".gamestats.gs.") != std::string::npos ||
           lowered.rfind("gamestats2.gs.", 0) == 0 ||
           lowered.find(".gamestats2.gs.") != std::string::npos ||
           lowered.rfind("race.gs.", 0) == 0 ||
           lowered.find(".race.gs.") != std::string::npos;
}

inline bool StartsHttpRequest(const uint8_t* data, size_t size) {
    if (!data || size < 4) {
        return false;
    }
    const std::string_view text(reinterpret_cast<const char*>(data), size);
    return text.rfind("GET ", 0) == 0 ||
           text.rfind("POST ", 0) == 0 ||
           text.rfind("HEAD ", 0) == 0 ||
           text.rfind("PUT ", 0) == 0 ||
           text.rfind("DELETE ", 0) == 0 ||
           text.rfind("OPTIONS ", 0) == 0;
}

// Rewrites only the HTTP Host header. Request path/body bytes stay retail.
inline bool RewriteHttpHostHeader(std::vector<uint8_t>& request) {
    if (request.empty()) {
        return false;
    }
    std::string text(reinterpret_cast<const char*>(request.data()), request.size());
    const size_t headerEnd = text.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        return false;
    }

    size_t lineStart = 0;
    while (lineStart < headerEnd) {
        const size_t lineEnd = text.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) {
            break;
        }
        const size_t colon = text.find(':', lineStart);
        if (colon != std::string::npos && colon < lineEnd) {
            const std::string name = Lower(std::string_view(text).substr(lineStart, colon - lineStart));
            if (name == "host") {
                size_t valueStart = colon + 1;
                while (valueStart < lineEnd && (text[valueStart] == ' ' || text[valueStart] == '\t')) {
                    ++valueStart;
                }
                const std::string oldHost = text.substr(valueStart, lineEnd - valueStart);
                const std::string newHost = RewriteNintendoHostname(oldHost);
                if (newHost != Lower(oldHost)) {
                    text.replace(valueStart, lineEnd - valueStart, newHost);
                    request.assign(text.begin(), text.end());
                    return true;
                }
                return false;
            }
        }
        lineStart = lineEnd + 2;
    }
    return false;
}

} // namespace WfcRedirectContract
