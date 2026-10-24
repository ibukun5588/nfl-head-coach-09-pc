// Xenia .patch.toml files for the launcher's patch manager (xenia_patch.h). The file format is a small part of TOML:
// top-level title_name / title_id / hash, then [[patch]] tables (name, desc, author, is_enabled) each followed by
// [[patch.<type>]] tables (address, value). This reads exactly that, not TOML in general.
#include "xenia_patch.h"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sstream>

namespace {

std::string Trim(const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

// The line up to a '#' that is not inside a string.
std::string StripComment(const std::string& s)
{
    char quote = 0;
    for (size_t i = 0; i < s.size(); ++i)
    {
        char c = s[i];
        if (quote)
        {
            if (quote == '"' && c == '\\') { ++i; continue; }
            if (c == quote) quote = 0;
        }
        else if (c == '"' || c == '\'') quote = c;
        else if (c == '#') return s.substr(0, i);
    }
    return s;
}

void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) out += char(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
    else { out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F)); out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F)); }
}

// A value: a string ("..." with escapes, or '...'), true/false, a number, or an array of those.
struct Value
{
    enum Kind { None, String, Bool, Int, Float, Array } kind = None;
    std::string s;
    bool b = false;
    int64_t i = 0;
    double f = 0;
    std::vector<Value> items;
};

bool ParseValue(const std::string& text, size_t& at, Value& v, std::string& err)
{
    while (at < text.size() && std::isspace((unsigned char)text[at])) ++at;
    if (at >= text.size()) { err = "missing value"; return false; }
    char c = text[at];
    if (c == '"' || c == '\'')
    {
        v.kind = Value::String;
        ++at;
        while (at < text.size() && text[at] != c)
        {
            if (c == '"' && text[at] == '\\' && at + 1 < text.size())
            {
                char e = text[++at];
                if (e == 'n') v.s += '\n';
                else if (e == 't') v.s += '\t';
                else if (e == 'r') v.s += '\r';
                else if ((e == 'u' || e == 'U') && at + (e == 'u' ? 4 : 8) < text.size())
                {
                    int n = e == 'u' ? 4 : 8;
                    AppendUtf8(v.s, uint32_t(std::stoul(text.substr(at + 1, size_t(n)), nullptr, 16)));
                    at += size_t(n);
                }
                else v.s += e;
                ++at;
            }
            else v.s += text[at++];
        }
        if (at >= text.size()) { err = "unterminated string"; return false; }
        ++at;
        return true;
    }
    if (c == '[')
    {
        v.kind = Value::Array;
        ++at;
        for (;;)
        {
            while (at < text.size() && (std::isspace((unsigned char)text[at]) || text[at] == ',')) ++at;
            if (at >= text.size()) { err = "unterminated array"; return false; }
            if (text[at] == ']') { ++at; return true; }
            Value item;
            if (!ParseValue(text, at, item, err)) return false;
            v.items.push_back(item);
        }
    }
    size_t end = at;
    while (end < text.size() && text[end] != ',' && text[end] != ']' && !std::isspace((unsigned char)text[end])) ++end;
    std::string word = text.substr(at, end - at);
    at = end;
    if (word == "true" || word == "false") { v.kind = Value::Bool; v.b = word == "true"; return true; }
    std::string clean;
    for (char ch : word) if (ch != '_') clean += ch;
    try
    {
        bool hex = clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X');
        bool neg = !clean.empty() && clean[0] == '-';
        if (!hex && clean.find_first_of(".eE") != std::string::npos && clean.find_first_of("xX") == std::string::npos)
        {
            v.kind = Value::Float;
            v.f = std::stod(clean);
            return true;
        }
        v.kind = Value::Int;
        if (hex) v.i = int64_t(std::stoull(clean.substr(2), nullptr, 16));
        else if (neg) v.i = std::stoll(clean);
        else v.i = int64_t(std::stoull(clean));
        v.f = double(v.i);
        return true;
    }
    catch (...)
    {
        err = "unreadable value \"" + word + "\"";
        return false;
    }
}

void PutBe(std::vector<uint8_t>& out, uint64_t v, int bytes)
{
    for (int k = bytes - 1; k >= 0; --k) out.push_back(uint8_t(v >> (8 * k)));
}

bool HexBytes(const std::string& s, std::vector<uint8_t>& out)
{
    std::string digits;
    for (size_t i = 0; i < s.size(); ++i)
    {
        if ((s[i] == 'x' || s[i] == 'X') && i > 0 && s[i - 1] == '0' && !digits.empty() && digits.back() == '0')
        {
            digits.pop_back();   // a "0x" prefix
            continue;
        }
        if (std::isxdigit((unsigned char)s[i])) digits += s[i];
        else if (!std::isspace((unsigned char)s[i]) && s[i] != ',') return false;
    }
    if (digits.empty() || digits.size() % 2) return false;
    for (size_t i = 0; i < digits.size(); i += 2) out.push_back(uint8_t(std::stoul(digits.substr(i, 2), nullptr, 16)));
    return true;
}

// One [[patch.<type>]] table's value as bytes.
bool WriteBytes(const std::string& type, const Value& v, std::vector<uint8_t>& out, std::string& err)
{
    auto needNumber = [&]() { if (v.kind != Value::Int && v.kind != Value::Float) { err = type + ": the value is not a number"; return false; } return true; };
    if (type == "be8" || type == "be16" || type == "be32" || type == "be64")
    {
        if (!needNumber()) return false;
        int bytes = type == "be8" ? 1 : type == "be16" ? 2 : type == "be32" ? 4 : 8;
        PutBe(out, uint64_t(v.kind == Value::Int ? v.i : int64_t(v.f)), bytes);
        return true;
    }
    if (type == "f32" || type == "f64")
    {
        if (!needNumber()) return false;
        if (type == "f32") { float f = float(v.f); uint32_t u; std::memcpy(&u, &f, 4); PutBe(out, u, 4); }
        else { double d = v.f; uint64_t u; std::memcpy(&u, &d, 8); PutBe(out, u, 8); }
        return true;
    }
    if (type == "string")
    {
        if (v.kind != Value::String) { err = "string: the value is not a string"; return false; }
        out.assign(v.s.begin(), v.s.end());
        return true;
    }
    if (type == "u16string")
    {
        if (v.kind != Value::String) { err = "u16string: the value is not a string"; return false; }
        for (size_t i = 0; i < v.s.size();)   // UTF-8 -> UTF-16BE
        {
            uint32_t cp = uint8_t(v.s[i]);
            int n = cp < 0x80 ? 1 : cp < 0xE0 ? 2 : cp < 0xF0 ? 3 : 4;
            if (n > 1) cp &= (0x7F >> n);
            for (int k = 1; k < n && i + size_t(k) < v.s.size(); ++k) cp = (cp << 6) | (uint8_t(v.s[i + size_t(k)]) & 0x3F);
            i += size_t(n);
            if (cp >= 0x10000) { cp -= 0x10000; PutBe(out, 0xD800 | (cp >> 10), 2); PutBe(out, 0xDC00 | (cp & 0x3FF), 2); }
            else PutBe(out, cp, 2);
        }
        return true;
    }
    if (type == "array")
    {
        if (v.kind == Value::String && HexBytes(v.s, out)) return true;
        if (v.kind == Value::Array)
        {
            for (const Value& item : v.items)
                if (item.kind == Value::Int) out.push_back(uint8_t(item.i));
                else if (!(item.kind == Value::String && HexBytes(item.s, out))) { err = "array: unreadable item"; return false; }
            return true;
        }
        err = "array: the value is not a hex string";
        return false;
    }
    err = "unsupported write type \"" + type + "\"";
    return false;
}

std::string ReadAll(const std::wstring& path)
{
    std::ifstream in(path, std::ios::binary);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);   // BOM
    return s;
}

}  // namespace

bool LoadXeniaPatchFile(const std::wstring& path, XeniaPatchFile& out)
{
    out = XeniaPatchFile{};
    out.path = path;
    size_t slash = path.find_last_of(L"\\/");
    out.fileName = slash == std::wstring::npos ? path : path.substr(slash + 1);
    std::string text = ReadAll(path);
    if (text.empty()) { out.error = "the file is empty or cannot be read"; return false; }

    enum class Table { Top, Patch, Write, Other } table = Table::Top;
    std::string writeType;
    struct PendingWrite { std::string type; bool hasAddress = false, hasValue = false; uint32_t address = 0; Value value; int line = 0; };
    std::vector<PendingWrite> pending;   // the writes of the current patch, resolved when it ends
    auto finishPatch = [&]() -> bool {
        if (out.patches.empty()) return true;
        for (PendingWrite& w : pending)
        {
            if (!w.hasAddress || !w.hasValue) { out.error = "line " + std::to_string(w.line) + ": a [[patch." + w.type + "]] needs address and value"; return false; }
            XeniaWrite x;
            x.address = w.address;
            std::string err;
            if (!WriteBytes(w.type, w.value, x.bytes, err)) { out.error = "line " + std::to_string(w.line) + ": " + err; return false; }
            if (!x.bytes.empty()) out.patches.back().writes.push_back(std::move(x));
        }
        pending.clear();
        return true;
    };

    std::istringstream lines(text);
    std::string raw;
    int lineNo = 0;
    while (std::getline(lines, raw))
    {
        ++lineNo;
        std::string line = Trim(StripComment(raw));
        if (line.empty()) continue;
        if (line[0] == '[')
        {
            std::string name = line;
            bool arrayTable = line.size() > 4 && line.compare(0, 2, "[[") == 0 && line.compare(line.size() - 2, 2, "]]") == 0;
            name = Trim(arrayTable ? line.substr(2, line.size() - 4) : line.substr(1, line.size() - 2));
            if (arrayTable && name == "patch")
            {
                if (!finishPatch()) return false;
                out.patches.emplace_back();
                table = Table::Patch;
            }
            else if (arrayTable && name.rfind("patch.", 0) == 0)
            {
                if (out.patches.empty()) { out.error = "line " + std::to_string(lineNo) + ": a write before any [[patch]]"; return false; }
                pending.push_back({ name.substr(6) });
                pending.back().line = lineNo;
                table = Table::Write;
            }
            else table = Table::Other;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos) { out.error = "line " + std::to_string(lineNo) + ": not \"key = value\""; return false; }
        std::string key = Trim(line.substr(0, eq)), valueText = line.substr(eq + 1);
        // an array value may go on over several lines
        if (Trim(valueText).rfind("[", 0) == 0)
            while (valueText.find(']') == std::string::npos && std::getline(lines, raw)) { ++lineNo; valueText += " " + StripComment(raw); }
        Value v;
        size_t at = 0;
        std::string err;
        if (!ParseValue(valueText, at, v, err)) { out.error = "line " + std::to_string(lineNo) + ": " + err; return false; }
        auto text1 = [&]() {
            if (v.kind == Value::String) return v.s;
            std::string all;
            for (const Value& item : v.items) if (item.kind == Value::String) all += (all.empty() ? "" : ", ") + item.s;
            return all;
        };
        switch (table)
        {
        case Table::Top:
            if (key == "title_name") out.titleName = text1();
            else if (key == "title_id") out.titleId = text1();
            else if (key == "hash")
            {
                if (v.kind == Value::String) out.hashes.push_back(v.s);
                for (const Value& item : v.items) if (item.kind == Value::String) out.hashes.push_back(item.s);
            }
            break;
        case Table::Patch:
            if (key == "name") out.patches.back().name = text1();
            else if (key == "desc") out.patches.back().desc = text1();
            else if (key == "author") out.patches.back().author = text1();
            else if (key == "is_enabled") out.patches.back().enabled = v.kind != Value::Bool || v.b;
            break;
        case Table::Write:
            if (key == "address")
            {
                if (v.kind != Value::Int) { out.error = "line " + std::to_string(lineNo) + ": the address is not a number"; return false; }
                pending.back().address = uint32_t(v.i);
                pending.back().hasAddress = true;
            }
            else if (key == "value") { pending.back().value = v; pending.back().hasValue = true; }
            break;
        case Table::Other:
            break;
        }
    }
    if (!finishPatch()) return false;
    if (out.patches.empty()) { out.error = "no [[patch]] in the file"; return false; }
    for (size_t i = 0; i < out.patches.size(); ++i)
        if (out.patches[i].name.empty()) out.patches[i].name = "Patch " + std::to_string(i + 1);
    return true;
}

std::string XeniaWritesText(const std::vector<std::pair<const XeniaPatchFile*, const XeniaPatch*>>& patches)
{
    std::string s;
    char buf[32];
    for (const auto& [file, patch] : patches)
    {
        std::string fileName;
        for (wchar_t c : file->fileName) fileName += c < 128 ? char(c) : '?';
        s += "# " + fileName + ": " + patch->name + "\n";
        for (const XeniaWrite& w : patch->writes)
        {
            snprintf(buf, sizeof buf, "%08X ", w.address);
            s += buf;
            for (uint8_t b : w.bytes) { snprintf(buf, sizeof buf, "%02X", b); s += buf; }
            s += "\n";
        }
    }
    return s;
}

bool ApplyXeniaWrites(const std::string& text, std::vector<uint8_t>& image, uint32_t base, std::string& err)
{
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line))
    {
        line = Trim(line.substr(0, line.find('#')));
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string addr, hex;
        ss >> addr >> hex;
        uint32_t a = uint32_t(std::stoul(addr, nullptr, 16));
        std::vector<uint8_t> bytes;
        if (!HexBytes(hex, bytes)) { err = "bad write line: " + line; return false; }
        if (a < base || uint64_t(a - base) + bytes.size() > image.size()) { err = "write outside the game image: " + addr; return false; }
        std::copy(bytes.begin(), bytes.end(), image.begin() + (a - base));
    }
    return true;
}
