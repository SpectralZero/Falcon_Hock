// ipc.hpp — minimal JSON-RPC 2.0 client for the umf_runtime named pipe.
//
// The runtime (once injected) listens on \\.\pipe\umf-studio-<pid> and speaks
// newline-delimited JSON. This client issues requests and drains `log`
// notifications that the runtime streams to subscribers.
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace hx {

class RuntimeClient {
public:
    ~RuntimeClient() { disconnect(); }

    bool connect(uint32_t pid, std::string& err);
    void disconnect();
    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    // Issue a request and wait (up to ~5s) for its response line. Returns the
    // raw JSON response, or "" on timeout/error (err set).
    std::string call(const std::string& method, const std::string& paramsJson, std::string& err);

    // Drain any pending notifications into the log buffer. Returns new count.
    int pump();

    const std::vector<std::string>& logs() const { return logs_; }
    void clear_logs() { logs_.clear(); }

private:
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    uint64_t next_id_ = 1;
    std::string rbuf_;
    std::vector<std::string> logs_;

    bool read_available();
    void process_lines(std::vector<std::pair<uint64_t, std::string>>& responses);
};

} // namespace hx
