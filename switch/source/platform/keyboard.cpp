#include "platform/keyboard.h"

#include <switch.h>

namespace keyboard {

bool prompt(const std::string& header, const std::string& initial, bool password, std::string& out) {
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0)))
        return false;
    if (password)
        swkbdConfigMakePresetPassword(&kbd);
    else
        swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetGuideText(&kbd, header.c_str());
    swkbdConfigSetInitialText(&kbd, initial.c_str());
    swkbdConfigSetStringLenMax(&kbd, 128);
    char buf[256] = {};
    Result rc = swkbdShow(&kbd, buf, sizeof(buf));
    swkbdClose(&kbd);
    if (R_FAILED(rc) || buf[0] == '\0')
        return false;
    out = buf;
    return true;
}

}  // namespace keyboard
