#pragma once
// Frontend option store. The host declares each option with its allowed values (the
// first is the default), or as free-form text (e.g. a path) with just a default. Values the user changed are persisted to config.ini as
// `key=value` lines; config.ini may be loaded before or after the options are declared.

#include <string>
#include <vector>

namespace options {

struct Option {
    std::string key;
    std::string description;
    std::vector<std::string> values;  // values[0] is the default
    std::vector<std::string> labels;  // display names, same order as values
    std::string current;
    bool free_form = false;           // any non-empty value allowed; values = {default}
};

// Declares (or redeclares) an option. Keeps a value already loaded from config.ini if it
// is in the allowed list. `labels` defaults to the values themselves.
void define(const std::string& key, const std::string& description,
            const std::vector<std::string>& values,
            const std::vector<std::string>& labels = {});

// Declares a free-form option (any non-empty value). Generic selector UIs skip these.
void define_text(const std::string& key, const std::string& description,
                 const std::string& default_value);

// Current value; "" if the key was never defined.
const std::string& get(const std::string& key);
bool is(const std::string& key, const std::string& value);  // get(key) == value

bool set(const std::string& key, const std::string& value);  // marks dirty
bool consume_dirty();  // true once after any change; host re-applies settings

const std::vector<Option>& all();

bool load(const std::string& path);
bool save(const std::string& path);

}  // namespace options
