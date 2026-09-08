/*
 * font_locator.cc
 *
 * See font_locator.h for the overall design.
 *
 * fc-match is invoked as a subprocess (same approach as the production
 * WebFontExpander) instead of linking fontconfig directly: zero extra
 * link dependencies and exactly the same matching behavior as the rest
 * of the container tooling.
 */

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

#include <sys/stat.h>

#include "util/font_locator.h"

namespace pdf2htmlEX {

namespace {

// normalize: lowercase, remove [\s-_], keep everything else (incl. CJK)
std::string normalize_font_name(const std::string & name)
{
    std::string out;
    out.reserve(name.size());
    for(char c : name)
    {
        if(c == ' ' || c == '-' || c == '_' || c == '\t')
            continue;
        out.push_back((char)std::tolower((unsigned char)c));
    }
    return out;
}

// strip subset prefix like "ABCDEF+"
std::string strip_subset_prefix(const std::string & name)
{
    if(name.size() > 7 && name[6] == '+')
    {
        bool ok = true;
        for(int i = 0; i < 6; ++i)
            if(!(name[i] >= 'A' && name[i] <= 'Z')) { ok = false; break; }
        if(ok)
            return name.substr(7);
    }
    return name;
}

// strip common style suffixes that are part of the name, not the family
std::string strip_style_suffixes(const std::string & name)
{
    static const char * suffixes[] = {
        "-BoldItalic", "-Bold", "-Italic", "-Regular",
        ",Bold", ",Italic",
        "PSMT", // TimesNewRomanPSMT
    };
    for(auto * s : suffixes)
    {
        auto l = std::strlen(s);
        if(name.size() > l && name.compare(name.size() - l, l, s) == 0)
            return name.substr(0, name.size() - l);
    }
    return name;
}

/*
 * Alias candidates for common CJK font names.
 * Keys are pre-normalized (lowercase, no separators).
 * Values are preferred real family names, in order.
 */
const std::vector<std::pair<std::string, std::vector<std::string>>> & alias_table()
{
    static const std::vector<std::pair<std::string, std::vector<std::string>>> table = {
        // 宋体
        {"simsun",            {"SimSun"}},
        {"宋体",             {"SimSun"}},
        {"nsimsun",           {"NSimSun", "SimSun"}},
        {"新宋体",            {"NSimSun", "SimSun"}},
        // 黑体
        {"simhei",            {"SimHei"}},
        {"黑体",             {"SimHei"}},
        // 仿宋
        {"fangsong",          {"FangSong"}},
        {"仿宋",             {"FangSong"}},
        {"fangsonggb2312",    {"FangSong"}},
        {"仿宋gb2312",       {"FangSong"}},
        // 楷体
        {"kaiti",             {"KaiTi"}},
        {"楷体",             {"KaiTi"}},
        {"kaitigb2312",       {"KaiTi"}},
        {"楷体gb2312",       {"KaiTi"}},
        // 微软雅黑
        {"microsoftyahei",    {"Microsoft YaHei"}},
        {"微软雅黑",          {"Microsoft YaHei"}},
        // 隶书 / 幼圆
        {"simli",             {"LiSu"}},
        {"隶书",             {"LiSu"}},
        {"lisu",              {"LiSu"}},
        {"simyou",            {"YouYuan"}},
        {"幼圆",             {"YouYuan"}},
        {"youyuan",           {"YouYuan"}},
        // 华文系列
        {"stsong",            {"STSong"}},
        {"华文中宋",          {"STSong"}},
        {"stkaiti",           {"STKaiti"}},
        {"华文楷体",          {"STKaiti"}},
        {"stfangsong",        {"STFangsong"}},
        {"华文仿宋",          {"STFangsong"}},
        {"stxihei",           {"STXihei"}},
        {"华文细黑",          {"STXihei"}},
        // 等线 / 其他常见
        {"dengxian",          {"DengXian"}},
        {"等线",             {"DengXian"}},
    };
    return table;
}

bool file_exists(const std::string & path)
{
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool has_font_suffix(const std::string & path)
{
    static const char * suffixes[] = {".ttf", ".ttc", ".otf"};
    if(path.size() < 4)
        return false;
    std::string tail = path.substr(path.size() - 4);
    std::transform(tail.begin(), tail.end(), tail.begin(),
            [](unsigned char c){ return std::tolower(c); });
    for(auto * s : suffixes)
        if(tail == s)
            return true;
    return false;
}

// run a command, capture stdout; returns empty optional on failure
std::string run_command(const std::string & cmd)
{
    std::string out;
    std::unique_ptr<FILE, int(*)(FILE*)> pipe(popen(cmd.c_str(), "r"), pclose);
    if(!pipe)
        return "";
    char buf[512];
    while(fgets(buf, sizeof(buf), pipe.get()))
        out += buf;
    return out;
}

// escape a string for safe embedding in single quotes in a shell command
std::string shell_quote(const std::string & s)
{
    std::string out = "'";
    for(char c : s)
    {
        if(c == '\'')
            out += "'\\''";
        else
            out += c;
    }
    out += "'";
    return out;
}

/*
 * fc-match for one candidate family name.
 * Fills `out` when the match is usable AND its family is consistent
 * with the requested name (normalized mutual containment).
 */
bool fc_match(const std::string & family, bool bold, bool italic, LocatedFontFile & out)
{
    std::string pattern = family;
    if(bold)
        pattern += ":style=Bold";
    else if(italic)
        pattern += ":style=Italic";

    std::string cmd = "fc-match -f \"%{file}\\t%{family}\\t%{index}\" " + shell_quote(pattern);
    std::string result = run_command(cmd);

    std::vector<std::string> parts;
    std::string cur;
    for(char c : result)
    {
        if(c == '\t' || c == '\n' || c == '\r')
        {
            if(!cur.empty()) { parts.push_back(cur); cur.clear(); }
        }
        else
            cur.push_back(c);
    }
    if(!cur.empty())
        parts.push_back(cur);
    if(parts.size() < 2)
        return false;

    if(!font_family_matches(family, parts[1]))
    {
        std::cerr << "font_locator: fc-match mismatch: requested '" << family
                  << "', got '" << parts[1] << "'" << std::endl;
        return false;
    }

    if(!file_exists(parts[0]) || !has_font_suffix(parts[0]))
        return false;

    out.path = parts[0];
    out.face_index = -1;
    if(parts.size() >= 3)
    {
        try { out.face_index = std::stoi(parts[2]); }
        catch(...) { out.face_index = -1; }
    }
    return true;
}

// last-ditch: scan directories, match by normalized filename stem
bool scan_dirs(const std::vector<std::string> & dirs,
               const std::vector<std::string> & normalized_candidates,
               LocatedFontFile & out)
{
    for(const auto & dir : dirs)
    {
        std::string cmd = "find " + shell_quote(dir)
            + " -maxdepth 3 -type f \\( -iname '*.ttf' -o -iname '*.ttc' -o -iname '*.otf' \\) 2>/dev/null";
        std::string listing = run_command(cmd);
        std::string line;
        std::string best;
        for(char c : listing)
        {
            if(c == '\n')
            {
                if(!line.empty())
                {
                    // stem: basename without extension
                    auto slash = line.find_last_of('/');
                    std::string stem = line.substr(slash == std::string::npos ? 0 : slash + 1);
                    auto dot = stem.find_last_of('.');
                    if(dot != std::string::npos)
                        stem = stem.substr(0, dot);
                    std::string nstem = normalize_font_name(stem);
                    for(const auto & cand : normalized_candidates)
                    {
                        if(!cand.empty() && (nstem.find(cand) != std::string::npos
                                    || cand.find(nstem) != std::string::npos))
                        {
                            best = line;
                            break;
                        }
                    }
                    line.clear();
                    if(!best.empty())
                        break;
                }
            }
            else
                line.push_back(c);
        }
        if(!best.empty())
        {
            out.path = best;
            out.face_index = -1;
            return true;
        }
    }
    return false;
}

} // namespace

bool font_family_matches(const std::string & requested, const std::string & matched_family)
{
    std::string req = normalize_font_name(requested);
    std::string got = normalize_font_name(matched_family);
    if(req.empty() || got.empty())
        return false;
    return req.find(got) != std::string::npos || got.find(req) != std::string::npos;
}

bool font_file_consistent(const LocatedFontFile & located, const std::string & requested)
{
    if(located.path.empty() || !file_exists(located.path))
        return false;

    std::string cmd = "fc-query -f \"%{family}\" ";
    if(located.face_index >= 0)
        cmd += "--index=" + std::to_string(located.face_index) + " ";
    cmd += shell_quote(located.path);

    std::string result = run_command(cmd);
    if(result.empty())
        return true; // cannot check; do not distrust the match

    // family list may be "SimSun,宋体": accept if ANY entry matches
    std::string cleaned = strip_style_suffixes(strip_subset_prefix(requested));
    std::string cur;
    for(size_t i = 0; i <= result.size(); ++i)
    {
        char c = (i < result.size()) ? result[i] : ',';
        if(c == ',' || c == '\n' || c == '\r')
        {
            if(!cur.empty() && font_family_matches(cleaned, cur))
                return true;
            cur.clear();
        }
        else
            cur.push_back(c);
    }

    std::cerr << "font_locator: located file family mismatch: requested '" << requested
              << "', file families '" << result << "'" << std::endl;
    return false;
}

bool locate_full_font(const std::string & pdf_font_name,
                      bool bold, bool italic,
                      const std::vector<std::string> & extra_dirs,
                      const std::string & default_font,
                      LocatedFontFile & out)
{
    // build the ordered candidate list
    std::vector<std::string> candidates;

    std::string cleaned = strip_style_suffixes(strip_subset_prefix(pdf_font_name));
    if(!cleaned.empty())
        candidates.push_back(cleaned);

    // alias expansion over the ORIGINAL candidates only (aliases of aliases
    // are not followed); skip candidates already present (normalized)
    {
        std::vector<std::string> seen_norm;
        for(const auto & c : candidates)
            seen_norm.push_back(normalize_font_name(c));

        std::vector<std::string> additions;
        for(const auto & cand : candidates)
        {
            std::string norm = normalize_font_name(cand);
            for(const auto & kv : alias_table())
            {
                if(kv.first == norm)
                {
                    for(const auto & v : kv.second)
                    {
                        std::string vnorm = normalize_font_name(v);
                        if(std::find(seen_norm.begin(), seen_norm.end(), vnorm) == seen_norm.end())
                        {
                            seen_norm.push_back(vnorm);
                            additions.push_back(v);
                        }
                    }
                }
            }
        }
        for(auto & a : additions)
            candidates.push_back(std::move(a));
    }

    // 1. fc-match for each candidate
    for(const auto & cand : candidates)
    {
        if(fc_match(cand, bold, italic, out))
            return true;
    }

    // 2. directory scan by filename (fontconfig may be broken/absent)
    if(!extra_dirs.empty())
    {
        std::vector<std::string> normalized;
        for(const auto & cand : candidates)
            normalized.push_back(normalize_font_name(cand));
        if(scan_dirs(extra_dirs, normalized, out))
            return true;
    }

    // 3. configured last-resort font, no name checking
    if(!default_font.empty() && file_exists(default_font) && has_font_suffix(default_font))
    {
        out.path = default_font;
        out.face_index = -1;
        return true;
    }

    return false;
}

} // namespace pdf2htmlEX
