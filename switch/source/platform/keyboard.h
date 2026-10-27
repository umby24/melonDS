#pragma once
// The system software keyboard (blocking; UI thread).

#include <string>

namespace keyboard {

// False if cancelled or unavailable. `password` hides the typed text.
bool prompt(const std::string& header, const std::string& initial, bool password, std::string& out);

}  // namespace keyboard
