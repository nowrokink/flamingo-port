// The helpers' way in (dxil_cache.serve): plain HTTP on every interface, from private addresses only and with the
// build's token, protocol "1" (tools/worker/flamingo_worker.cpp is the other side):
//     GET  /job?w=<name>            200 with X-Job-Id, X-Profile, X-Opt, X-Timeout and the HLSL; 204 nothing now;
//                                   410 the build is over
//     POST /result?id=<id>&ok=1     the DXIL (ok=0: the error)
// A wrong token is answered 403, another protocol version 426.
#pragma once
#include <functional>
#include <memory>
#include <string>

#include "cache/queue.h"

namespace fp::cache {

class Server {
public:
    using Finish = std::function<void(const JobPtr& job, std::string dxil)>;
    // Listens on `port` (all IPv4 interfaces) until stopped. False if the port can't be opened.
    bool start(Queue& queue, int port, const std::string& token, const std::string& opt, int timeout_s, Finish finish);
    void stop();
    ~Server();

    struct State;

private:
    std::shared_ptr<State> st_;
};

// Whether an IPv4 address (dotted) is private or loopback, as Python's ipaddress says.
bool private_address(const std::string& addr);

}  // namespace fp::cache
