#pragma once

#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

class DnsServer {
public:
    DnsServer(uint32_t redirect_ip);
    ~DnsServer() { stop(); }

    bool start(uint16_t port = 53, int stack_size = 4096, UBaseType_t prio = 5);
    void stop();

private:
    static void taskTrampoline(void* arg);
    void run();
    void handleDnsRequest(uint8_t* buffer, int len, const sockaddr_in& client_addr);
    int buildDnsResponse(uint8_t* request, int request_len, uint8_t* response, int max_response_len);

    std::atomic<bool> running_{false};
    TaskHandle_t task_{nullptr};
    int sock_{-1};
    uint16_t port_{53};
    uint32_t redirect_addr_;
};