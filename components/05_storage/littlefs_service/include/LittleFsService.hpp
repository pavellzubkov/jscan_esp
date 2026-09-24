#pragma once

#include "esp_err.h"
#include <cstddef>
#include <cstdint>
#include <string>

/**
 * @brief Сервис для работы с LittleFS
 */
class LittleFsService {
public:
    LittleFsService(const char* basePath = "/littlefs",
                    const char* partitionLabel = "storage",
                    bool formatIfFail = true);

    ~LittleFsService();

    esp_err_t mount();
    esp_err_t mount(bool formatIfFail);
    esp_err_t unmount();

    /**
     * @brief Открытый файл для потоковой передачи (чанкового чтения)
     */
    struct FileStream {
        int fd = -1;          ///< файловый дескриптор, -1 = не открыт
        size_t size = 0;      ///< размер файла в байтах
        bool is_gz = false;   ///< отдаётся ли сжатая (.gz) версия
        bool ok = false;      ///< успешно ли открыт
    };

    /**
     * @brief Открыть файл для потокового чтения. Автоматически пробует plain,
     *        затем .gz версию.
     * @param path путь относительно basePath (например "/index.html")
     * @return FileStream (нужно закрыть через closeStream())
     */
    FileStream openStream(const std::string& path);

    /**
     * @brief Прочитать порцию данных из открытого потока
     * @param stream открытый через openStream поток
     * @param buf буфер назначения
     * @param len размер буфера
     * @return число прочитанных байт, 0 = EOF, -1 = ошибка
     */
    static ssize_t readChunk(FileStream& stream, void* buf, size_t len);

    /**
     * @brief Закрыть поток
     */
    static void closeStream(FileStream& stream);

    bool isMounted() const { return mounted_; }

private:
    FileStream openRawStream(const std::string& path);

    std::string basePath_;
    std::string partitionLabel_;
    bool formatIfFail_;
    bool mounted_ = false;
};
