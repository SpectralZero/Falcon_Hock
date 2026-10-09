// ipc.cpp — JSON-RPC 2.0 named-pipe client (see ipc.hpp).
#include "ipc.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hx {

bool RuntimeClient::connect(uint32_t pid, std::string& err) {
    disconnect();
    std::string name = "\\\\.\\pipe\\umf-studio-" + std::to_string(pid);
    HANDLE h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "no runtime pipe for pid " + std::to_string(pid) +
              " (is the runtime loaded there?)";
        return false;
    }
    pipe_ = h;
    rbuf_.clear();
    // Subscribe so the runtime streams log notifications to us.
    std::string ignored;
    call("subscribe", "null", ignored);
    return true;
}

void RuntimeClient::disconnect() {
    if (pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    rbuf_.clear();
}

bool RuntimeClient::read_available() {
    if (pipe_ == INVALID_HANDLE_VALUE) return false;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return false;
        if (avail == 0) return true;
        char chunk[4096];
        DWORD toread = avail < sizeof(chunk) ? avail : (DWORD)sizeof(chunk);
        DWORD got = 0;
        if (!ReadFile(pipe_, chunk, toread, &got, nullptr) || got == 0) return false;
        rbuf_.append(chunk, got);
    }
}

void RuntimeClient::process_lines(std::vector<std::pair<uint64_t, std::string>>& responses) {
    size_t pos;
    while ((pos = rbuf_.find('\n')) != std::string::npos) {
        std::string line = rbuf_.substr(0, pos);
        rbuf_.erase(0, pos + 1);
        if (line.empty()) continue;

        bool has_result = line.find("\"result\"") != std::string::npos;
        bool has_error = line.find("\"error\"") != std::string::npos;
        size_t idpos = line.find("\"id\":");
        if ((has_result || has_error) && idpos != std::string::npos) {
            uint64_t id = strtoull(line.c_str() + idpos + 5, nullptr, 10);
            responses.push_back({id, line});
        } else if (line.find("\"method\":\"log\"") != std::string::npos) {
            // Extract the "msg" for readability, keep raw otherwise.
            size_t m = line.find("\"msg\":\"");
            if (m != std::string::npos) {
                m += 7;
                std::string msg;
                for (size_t i = m; i < line.size(); i++) {
                    if (line[i] == '\\' && i + 1 < line.size()) {
                        char c = line[++i];
                        msg += (c == 'n') ? '\n' : (c == 't') ? '\t' : c;
                    } else if (line[i] == '"') {
                        break;
                    } else {
                        msg += line[i];
                    }
                }
                logs_.push_back(msg);
            } else {
                logs_.push_back(line);
            }
            if (logs_.size() > 2000) logs_.erase(logs_.begin(), logs_.begin() + (logs_.size() - 2000));
        }
    }
}

std::string RuntimeClient::call(const std::string& method, const std::string& paramsJson,
                                std::string& err) {
    if (pipe_ == INVALID_HANDLE_VALUE) {
        err = "not connected to a runtime";
        return "";
    }
    uint64_t id = next_id_++;
    char req[8192];
    int n = snprintf(req, sizeof(req),
                     "{\"jsonrpc\":\"2.0\",\"id\":%llu,\"method\":\"%s\",\"params\":%s}\n",
                     (unsigned long long)id, method.c_str(),
                     paramsJson.empty() ? "null" : paramsJson.c_str());
    DWORD written = 0;
    if (!WriteFile(pipe_, req, (DWORD)n, &written, nullptr) || written != (DWORD)n) {
        err = "pipe write failed";
        return "";
    }

    DWORD start = GetTickCount();
    std::vector<std::pair<uint64_t, std::string>> responses;
    for (;;) {
        read_available();
        process_lines(responses);
        for (auto& r : responses)
            if (r.first == id) return r.second;
        responses.clear();
        if (GetTickCount() - start > 5000) {
            err = "rpc timeout";
            return "";
        }
        Sleep(2);
    }
}

int RuntimeClient::pump() {
    size_t before = logs_.size();
    std::vector<std::pair<uint64_t, std::string>> responses;
    read_available();
    process_lines(responses);
    return (int)(logs_.size() - before);
}

} // namespace hx
