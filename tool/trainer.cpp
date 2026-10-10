// trainer.cpp — .hexforge (de)serialisation and single-file trainer overlay.
#include "trainer.hpp"

#include <windows.h>
#include <cstring>

namespace hx {

const char kTrainerMagic[8] = "HXFTRN1";   // 7 chars + NUL = 8 bytes

static std::string sanitize(const std::string& in) {
    std::string o;
    o.reserve(in.size());
    for (char c : in) {
        if (c == '|' || c == '\n' || c == '\r') o += ' ';
        else o += c;
    }
    return o;
}

std::string serialize_trainer(const TrainerDoc& doc) {
    std::string o = "HEXFORGE1\n";
    o += "#TITLE=" + sanitize(doc.title) + "\n";
    o += "#TARGET=" + sanitize(doc.target) + "\n";
    for (const auto& c : doc.cheats) {
        char line[320];
        snprintf(line, sizeof(line), "%s|%llX|%d|%s|%d|%d|%d\n",
                 sanitize(c.label).c_str(), (unsigned long long)c.addr, c.type,
                 sanitize(c.value).c_str(), c.freeze ? 1 : 0, c.hotkey, c.mode);
        o += line;
    }
    return o;
}

bool parse_trainer(const std::string& text, TrainerDoc& out) {
    out = TrainerDoc{};
    out.cheats.clear();
    size_t pos = 0;
    bool sawHeader = false;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() : nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) continue;

        if (line == "HEXFORGE1") { sawHeader = true; continue; }
        if (line[0] == '#') {
            size_t eq = line.find('=');
            if (eq != std::string::npos) {
                std::string key = line.substr(1, eq - 1);
                std::string val = line.substr(eq + 1);
                if (key == "TITLE") out.title = val;
                else if (key == "TARGET") out.target = val;
            }
            continue;
        }

        // cheat line: label|addr|type|value|freeze|hotkey|mode
        std::string f[7];
        int nf = 0;
        size_t start = 0;
        for (size_t i = 0; i <= line.size() && nf < 7; ++i) {
            if (i == line.size() || line[i] == '|') {
                f[nf++] = line.substr(start, i - start);
                start = i + 1;
                if (i == line.size()) break;
            }
        }
        if (nf >= 6) {
            Cheat c;
            snprintf(c.label, sizeof(c.label), "%s", f[0].c_str());
            c.addr = (uintptr_t)strtoull(f[1].c_str(), nullptr, 16);
            c.type = atoi(f[2].c_str());
            snprintf(c.value, sizeof(c.value), "%s", f[3].c_str());
            c.freeze = atoi(f[4].c_str()) != 0;
            c.hotkey = atoi(f[5].c_str());
            c.mode = (nf >= 7) ? atoi(f[6].c_str()) : 0;
            out.cheats.push_back(c);
        }
    }
    return sawHeader || !out.cheats.empty();
}

bool read_embedded_trainer(TrainerDoc& out) {
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return false;
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f) return false;

    _fseeki64(f, 0, SEEK_END);
    long long sz = _ftelli64(f);
    if (sz < 16) { fclose(f); return false; }

    char magic[8] = {};
    _fseeki64(f, sz - 8, SEEK_SET);
    if (fread(magic, 1, 8, f) != 8 || memcmp(magic, kTrainerMagic, 8) != 0) {
        fclose(f);
        return false;
    }
    uint64_t len = 0;
    _fseeki64(f, sz - 16, SEEK_SET);
    if (fread(&len, 1, 8, f) != 8 || len == 0 || (long long)len > sz - 16) {
        fclose(f);
        return false;
    }
    std::string payload;
    payload.resize((size_t)len);
    _fseeki64(f, sz - 16 - (long long)len, SEEK_SET);
    size_t got = fread(payload.data(), 1, (size_t)len, f);
    fclose(f);
    if (got != (size_t)len) return false;
    return parse_trainer(payload, out);
}

bool append_trainer_overlay(const std::wstring& exe_path, const TrainerDoc& doc) {
    std::string payload = serialize_trainer(doc);
    FILE* f = nullptr;
    _wfopen_s(&f, exe_path.c_str(), L"ab");
    if (!f) return false;
    uint64_t len = payload.size();
    bool ok = fwrite(payload.data(), 1, payload.size(), f) == payload.size();
    ok = ok && fwrite(&len, 1, 8, f) == 8;
    ok = ok && fwrite(kTrainerMagic, 1, 8, f) == 8;
    fclose(f);
    return ok;
}

} // namespace hx
