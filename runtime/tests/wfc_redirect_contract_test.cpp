#include "hle/wfc_redirect_contract.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

int main() {
    using namespace WfcRedirectContract;

    assert(IsMeteorWiimmfiTitle(0x52445350u, 0x4146u));
    assert(!IsMeteorWiimmfiTitle(0x524D4350u, 0x3031u));

    assert(RewriteNintendoHostname("naswii.nintendowifi.net") == "nas.wiimmfi.de");
    assert(RewriteNintendoHostname("gpcm.gs.nintendowifi.net") == "gpcm.gs.wiimmfi.de");
    assert(RewriteNintendoHostname("natneg1.gs.nintendowifi.net") == "natneg1.gs.wiimmfi.de");
    assert(RewriteNintendoHostname("sake.gamespy.com") == "sake.wiimmfi.de");
    assert(RewriteNintendoHostname("sdkdev.gamespy.com") == "sdkdev.gamespy.com");

    const std::string original =
        "POST /ac HTTP/1.1\r\n"
        "Host: naswii.nintendowifi.net\r\n"
        "Content-Length: 4\r\n\r\n"
        "test";
    std::vector<uint8_t> request(original.begin(), original.end());
    assert(RewriteHttpHostHeader(request));
    const std::string rewritten(request.begin(), request.end());
    assert(rewritten.find("Host: nas.wiimmfi.de\r\n") != std::string::npos);
    assert(rewritten.find("Content-Length: 4\r\n\r\ntest") != std::string::npos);

    const std::string unrelated =
        "GET / HTTP/1.1\r\n"
        "Host: example.com\r\n\r\n";
    request.assign(unrelated.begin(), unrelated.end());
    assert(!RewriteHttpHostHeader(request));
    assert(std::string(request.begin(), request.end()) == unrelated);

    std::cout << "WFC redirect contract: RDSP Wiimmfi DNS and HTTP host rewrite validated\n";
    return 0;
}
