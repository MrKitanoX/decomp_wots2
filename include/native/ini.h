#pragma once
// Small ini helpers shared by the mods' mod.ini code, the F5 panel and the
// PC settings menu (settings.ini).
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace ini {

// "[section]" and "key = value" lines; ';' or '#' starts a comment.
// → "section.key" → value (section and key in lower case).  False if the
// file cannot be read.
bool read(const std::string& path, std::map<std::string, std::string>& out);

// Writes `values` into `section`, keeping the file's comments and order; keys
// it lacks go at the end of that section, a missing section at the end of the
// file.  With `create`, a missing file is made.
bool write(const std::string& path, const std::vector<std::pair<std::string, std::string>>& values,
           const char* section, bool create = false);

}  // namespace ini
