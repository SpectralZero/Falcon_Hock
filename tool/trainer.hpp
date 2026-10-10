// trainer.hpp — shared cheat model + .hexforge format used by both the main
// Hexforge tool and the exported standalone trainer runner.
//
// Keeping the Cheat layout, the typed read/write helpers, and the file format
// in one place guarantees the tool and the trainer it exports always agree.
#pragma once

#include "backend.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace hx {

// A single cheat entry.
//   mode 0 = Toggle   : the hotkey flips Freeze on/off (god mode, infinite ammo).
//   mode 1 = Set once : the hotkey writes `value` a single time (give 999 ammo).
struct Cheat {
    uintptr_t addr = 0;
    int type = 2;            // ScanType index (int32)
    bool freeze = false;     // currently locking the value every frame
    int hotkey = 0;          // VK code (0 = none)
    int mode = 0;            // 0 = toggle freeze, 1 = set once
    char label[48] = "value";
    char value[32] = "0";
};

// A whole trainer: a titled list of cheats bound to one target process.
struct TrainerDoc {
    std::string title = "My Trainer";
    std::string target;      // process exe name, e.g. "re3.exe"
    std::vector<Cheat> cheats;
};

inline ScanType scan_type_of(int i) {
    switch (i) {
        case 0: return ScanType::I8;
        case 1: return ScanType::I16;
        case 2: return ScanType::I32;
        case 3: return ScanType::I64;
        case 4: return ScanType::F32;
        default: return ScanType::F64;
    }
}

inline const char* scan_type_name(int i) {
    static const char* names[] = { "int8", "int16", "int32", "int64", "float", "double" };
    return (i >= 0 && i < 6) ? names[i] : "int32";
}

// Read `addr` as type `ty` and format it for display. "??" if unreadable.
inline std::string fmt_value_at(const Target& t, uintptr_t addr, ScanType ty) {
    uint8_t b[8] = {};
    if (!t.read(addr, b, type_size(ty))) return "??";
    char out[64];
    switch (ty) {
        case ScanType::I8:  snprintf(out, 64, "%d", *(int8_t*)b); break;
        case ScanType::I16: snprintf(out, 64, "%d", *(int16_t*)b); break;
        case ScanType::I32: snprintf(out, 64, "%d", *(int32_t*)b); break;
        case ScanType::I64: snprintf(out, 64, "%lld", (long long)*(int64_t*)b); break;
        case ScanType::F32: snprintf(out, 64, "%.4f", *(float*)b); break;
        case ScanType::F64: snprintf(out, 64, "%.4f", *(double*)b); break;
    }
    return out;
}

inline void write_typed(const Target& t, uintptr_t addr, ScanType ty, double v) {
    uint8_t b[8] = {};
    switch (ty) {
        case ScanType::I8:  *(int8_t*)b  = (int8_t)v; break;
        case ScanType::I16: *(int16_t*)b = (int16_t)v; break;
        case ScanType::I32: *(int32_t*)b = (int32_t)v; break;
        case ScanType::I64: *(int64_t*)b = (int64_t)v; break;
        case ScanType::F32: *(float*)b   = (float)v; break;
        case ScanType::F64: *(double*)b  = v; break;
    }
    t.write(addr, b, type_size(ty));
}

// Hotkey mapping: combo index 0 = none, 1..12 = F1..F12.
inline int hk_index(int vk) {
    if (vk >= VK_F1 && vk <= VK_F12) return vk - VK_F1 + 1;
    return 0;
}
inline int hk_vk(int index) { return index == 0 ? 0 : VK_F1 + (index - 1); }
inline const char* hk_name(int vk) {
    static const char* names[] = { "none", "F1", "F2", "F3", "F4", "F5", "F6",
                                   "F7", "F8", "F9", "F10", "F11", "F12" };
    int i = hk_index(vk);
    return (i >= 0 && i <= 12) ? names[i] : "none";
}

// .hexforge text format (one source of truth):
//   HEXFORGE1
//   #TITLE=<title>
//   #TARGET=<exe name>
//   <label>|<addr hex>|<type>|<value>|<freeze 0/1>|<hotkey vk>|<mode 0/1>
std::string serialize_trainer(const TrainerDoc& doc);
bool parse_trainer(const std::string& text, TrainerDoc& out);

// Overlay embedding: an exported trainer .exe carries its TrainerDoc appended
// to the end of the file as [payload][u64 len][8-byte magic]. These read/write
// that overlay so a single shared .exe can double as any game's trainer.
extern const char kTrainerMagic[8];   // "HXFTRN1"
bool read_embedded_trainer(TrainerDoc& out);                      // from own exe
bool append_trainer_overlay(const std::wstring& exe_path, const TrainerDoc& doc);

} // namespace hx
