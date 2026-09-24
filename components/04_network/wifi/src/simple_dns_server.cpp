#include "simple_dns_server.hpp"
#include <esp_log.h>
#include <cstring>
#include <lwip/sockets.h>
#include <arpa/inet.h>

static const char* TAG_DNS = "DNS";

// Minimal DNS header
#pragma pack(push, 1)
struct DnsHeader {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;
    uint16_t ancount;
    uint16_t nscount;
    uint16_t arcount;
};

struct DnsAnswer {
    uint16_t name;      // Pointer to name (0xC00C for compression)
    uint16_t type;      // A record
    uint16_t class_;    // IN class
    uint32_t ttl;       // Time to live
    uint16_t length;    // Data length
    uint32_t addr;      // IP address
};
#pragma pack(pop)

DnsServer::DnsServer(uint32_t redirect_ip) {
    redirect_addr_= redirect_ip; 
}

bool DnsServer::start(uint16_t port, int stack_size, UBaseType_t prio) {
    if (running_.load()) return true;
    
    port_ = port;
    running_.store(true);
    
    BaseType_t result = xTaskCreate(taskTrampoline, "dns_server", stack_size, this, prio, &task_);
    if (result != pdPASS) {
        running_.store(false);
        return false;
    }
    
    return true;
}

void DnsServer::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
    // Закрываем сокет для выхода из блокирующего recvfrom
    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
    
    // Ждем завершения задачи
    if (task_) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
        task_ = nullptr;
    }
}

void DnsServer::taskTrampoline(void* arg) {
    static_cast<DnsServer*>(arg)->run();
}

void DnsServer::run() {
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ < 0) {
        ESP_LOGE(TAG_DNS, "Socket creation failed: %d", errno);
        running_.store(false);
        vTaskDelete(nullptr);
        return;
    }

    // Устанавливаем timeout для recvfrom
    struct timeval timeout;
    timeout.tv_sec = 1;
    timeout.tv_usec = 0;
    setsockopt(sock_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG_DNS, "Bind failed: %d", errno);
        close(sock_);
        sock_ = -1;
        running_.store(false);
        vTaskDelete(nullptr);
        return;
    }

    ESP_LOGI(TAG_DNS, "DNS Server started on port %d", port_);

    uint8_t buffer[512];
    sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    while (running_.load()) {
        int len = recvfrom(sock_, buffer, sizeof(buffer), 0, 
                          (sockaddr*)&client_addr, &client_len);
        
        if (len < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - продолжаем цикл
                continue;
            }
            ESP_LOGE(TAG_DNS, "recvfrom failed: %d", errno);
            break;
        }

        if (len < sizeof(DnsHeader)) {
            ESP_LOGW(TAG_DNS, "DNS packet too small: %d bytes", len);
            continue;
        }

        handleDnsRequest(buffer, len, client_addr);
    }

    if (sock_ >= 0) {
        close(sock_);
        sock_ = -1;
    }
    
    ESP_LOGI(TAG_DNS, "DNS Server stopped");
    running_.store(false);
    vTaskDelete(nullptr);
}

void DnsServer::handleDnsRequest(uint8_t* buffer, int len, const sockaddr_in& client_addr) {
    DnsHeader* header = (DnsHeader*)buffer;
    
    // Проверяем, что это запрос (не ответ)
    if (ntohs(header->flags) & 0x8000) {
        return; // Это ответ, игнорируем
    }

    // Проверяем, что есть хотя бы один вопрос
    if (ntohs(header->qdcount) == 0) {
        return;
    }

    ESP_LOGD(TAG_DNS, "DNS Query ID: 0x%04X, Questions: %d", 
             ntohs(header->id), ntohs(header->qdcount));

    // Формируем ответ
    uint8_t response[512];
    int response_len = buildDnsResponse(buffer, len, response, sizeof(response));
    
    if (response_len > 0) {
        sendto(sock_, response, response_len, 0, 
               (sockaddr*)&client_addr, sizeof(client_addr));
        
        ESP_LOGD(TAG_DNS, "DNS Response sent: %d bytes", response_len);
    }
}

int DnsServer::buildDnsResponse(uint8_t* request, int request_len, 
                               uint8_t* response, int max_response_len) {
    if (request_len < sizeof(DnsHeader)) {
        return 0;
    }

    DnsHeader* req_header = (DnsHeader*)request;
    DnsHeader* resp_header = (DnsHeader*)response;

    // Копируем заголовок
    *resp_header = *req_header;
    
    // Устанавливаем флаги ответа
    resp_header->flags = htons(0x8180); // Response, Authoritative, No error
    resp_header->nscount = 0;
    resp_header->arcount = 0;

    // ancount выставим в конце по фактически записанным ответам

    int pos = sizeof(DnsHeader);
    
    // Копируем секцию вопросов
    uint8_t* question_start = request + sizeof(DnsHeader);
    int question_len = request_len - sizeof(DnsHeader);
    
    // Находим конец вопросов (нужно пропарсить имена)
    int questions_size = 0;
    uint8_t* ptr = question_start;
    uint8_t* const end = request + request_len; // граница пакета

    for (int q = 0; q < ntohs(req_header->qdcount); q++) {
        // Пропускаем имя домена. Метки: <len><данные len байт>; байт 0 —
        // конец имени; 0xC0xx — указатель сжатия (2 байта). На каждом шаге
        // проверяем границы пакета, чтобы не выйти за request_len (OOB-read).
        for (;;) {
            if (ptr >= end) return 0; // имя не завершилось в пределах пакета
            uint8_t c = *ptr;
            if (c == 0) {
                ptr++; // завершающий ноль имени
                break;
            }
            if ((c & 0xC0) == 0xC0) {
                ptr += 2; // указатель сжатия — сам по себе полное имя
                break;
            }
            ptr += (size_t)c + 1; // метка + её данные
            if (ptr > end) return 0; // метка выходит за границу пакета
        }
        // Type (2) + Class (2)
        if ((size_t)(end - ptr) < 4) return 0;
        ptr += 4;
    }

    questions_size = ptr - question_start;

    if (pos + questions_size > max_response_len) {
        return 0;
    }

    // Копируем вопросы в ответ
    memcpy(response + pos, question_start, questions_size);
    pos += questions_size;

    // Добавляем ответы для каждого вопроса. Считаем реально записанные
    // ответы и выставляем ancount правдиво (не больше записанного).
    int answers_written = 0;
    for (int q = 0; q < ntohs(req_header->qdcount); q++) {
        if (pos + sizeof(DnsAnswer) > max_response_len) {
            break;
        }

        DnsAnswer* answer = (DnsAnswer*)(response + pos);
        answer->name = htons(0xC000 | sizeof(DnsHeader));        // Указатель на имя в вопросе
        answer->type = htons(1);             // A record
        answer->class_ = htons(1);           // IN class
        answer->ttl = htonl(300);            // TTL 5 минут
        answer->length = htons(4);           // IPv4 адрес
        answer->addr = redirect_addr_;       // IP адрес для перенаправления

        pos += sizeof(DnsAnswer);
        answers_written++;
    }
    resp_header->ancount = htons(static_cast<uint16_t>(answers_written));

    return pos;
}