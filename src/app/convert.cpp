#include "convert.h"

#include "config.h"
#include "cuefile.h"
#include "rpp.h"
#include "rpp_export.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace cue {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool readText(const std::string& path, std::string& text) {
    std::ifstream f(path, std::ios::binary);
    if (!f)
        return false;
    std::stringstream ss;
    ss << f.rdbuf();
    text = ss.str();
    return true;
}

// Writes beside the old file and replaces it once complete, keeping the old one as .bak.
bool writeFile(const std::string& path, const std::string& text, std::string* error) {
    const std::string tmp = path + ".part";
    {
        std::ofstream o(tmp, std::ios::binary);
        o << text;
        if (!o) {
            *error = "cannot write " + path;
            return false;
        }
    }
    std::error_code ec;
    if (fs::exists(path, ec))
        fs::copy_file(path, path + ".bak", fs::copy_options::overwrite_existing, ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        *error = "cannot write " + path + ": " + ec.message();
        return false;
    }
    return true;
}

// A folder of its own under the system's temporary folder, removed with the object.
struct TempDir {
    std::string path;
    TempDir() {
        std::string tmpl = (fs::temp_directory_path() / "cue-convert-XXXXXX").string();
        if (mkdtemp(tmpl.data()))
            path = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        if (!path.empty())
            fs::remove_all(path, ec);
    }
};

bool rppToCue(const std::string& in, const std::string& out, const ConvertOptions& options, ConvertResult& result, std::string* error) {
    std::string text;
    if (!readText(in, text)) {
        *error = "cannot open " + in;
        return false;
    }
    const std::string project = fs::path(in).stem().string();
    SystemLocator locator(options.search, options.reaperDir, options.scanClap);
    Import imp;
    std::string mapping;
    if (!options.mapping.empty()) {
        std::error_code ec;
        mapping = fs::absolute(expandPath(options.mapping), ec).string();
        if (!fs::exists(mapping, ec)) {
            *error = "pedal file " + options.mapping + " not found";
            return false;
        }
    }
    if (!convertRpp(text, project, locator, mapping, imp, error))
        return false;
    // The import is a config with its states beside it: read it back from a
    // temporary folder and save it as one file.
    TempDir dir;
    if (dir.path.empty()) {
        *error = "cannot make a temporary folder";
        return false;
    }
    std::error_code ec;
    fs::create_directories(fs::path(dir.path) / "states", ec);
    for (const auto& f : imp.files) {
        std::ofstream o(fs::path(dir.path) / f.path, std::ios::binary);
        o.write(reinterpret_cast<const char*>(f.data.data()), static_cast<std::streamsize>(f.data.size()));
    }
    Config config;
    if (!parseConfigText(imp.config, (fs::path(dir.path) / (project + ".ini")).string(), config, error))
        return false;
    config.performance = project;
    std::vector<std::string> warnings;
    if (!saveCue(config, out, error, &warnings))
        return false;
    result.notes = imp.notes;
    if (!mapping.empty())
        result.notes.insert(result.notes.begin(), "pedals: from " + fs::path(mapping).filename().string() + ", not from ReaLearn");
    result.notes.insert(result.notes.end(), warnings.begin(), warnings.end());
    result.chains = imp.chains;
    result.plugins = imp.plugins;
    result.skipped = imp.skipped;
    result.missing = imp.missing;
    return true;
}

bool cueToRpp(const std::string& in, const std::string& out, const ConvertOptions& options, ConvertResult& result, std::string* error) {
    Config config;
    if (!loadPerformance(in, options.rig, config, error))
        return false;
    if (!presetsToChains(config, error))
        return false;
    RppExport exp;
    if (!exportRpp(config, exp, error, options.reaperDir))
        return false;
    if (!writeFile(out, exp.text, error))
        return false;
    result.notes = config.warnings;
    result.notes.insert(result.notes.end(), exp.notes.begin(), exp.notes.end());
    for (const auto& s : config.scenes)
        result.chains += static_cast<int>(s.chains.size());
    result.plugins = exp.plugins;
    result.skipped = exp.skipped;
    return true;
}

} // namespace

bool isRppFile(const std::string& path) { return lower(fs::path(path).extension().string()) == ".rpp"; }

std::string convertedName(const std::string& in) {
    fs::path p(in);
    p.replace_extension(isRppFile(in) ? ".cue" : ".RPP");
    return p.string();
}

bool convertFile(const std::string& in, const std::string& out, const ConvertOptions& options, ConvertResult& result, std::string* error) {
    result = ConvertResult{};
    result.out = out.empty() ? convertedName(in) : out;
    // Without a name given, never replace a file: Song.RPP made from Song.cue
    // would otherwise overwrite the project Song.cue came from.
    std::error_code ec;
    for (int n = 2; out.empty() && fs::exists(result.out, ec); ++n) {
        const fs::path p = convertedName(in);
        result.out = (p.parent_path() / (p.stem().string() + " (" + std::to_string(n) + ")" + p.extension().string())).string();
    }
    if (fs::absolute(result.out) == fs::absolute(in)) {
        *error = "the converted file would replace " + in;
        return false;
    }
    if (isRppFile(in))
        return rppToCue(in, result.out, options, result, error);
    return cueToRpp(in, result.out, options, result, error);
}

} // namespace cue
