#include "LittleFsService.hpp"
#include "esp_littlefs.h"
#include "esp_log.h"
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static const char *TAG = "LittleFsService";

LittleFsService::LittleFsService(const char *basePath,
                                 const char *partitionLabel,
                                 bool formatIfFail)
    : basePath_(basePath),
      partitionLabel_(partitionLabel),
      formatIfFail_(formatIfFail) {}

LittleFsService::~LittleFsService()
{
    unmount();
}

esp_err_t LittleFsService::mount()
{
    return mount(formatIfFail_);
}

esp_err_t LittleFsService::mount(bool formatIfFail)
{
    if (mounted_)
    {
        ESP_LOGD(TAG, "LittleFS already mounted");
        return ESP_OK;
    }

    esp_vfs_littlefs_conf_t conf = {
        .base_path = basePath_.c_str(),
        .partition_label = partitionLabel_.c_str(),
        .partition = nullptr,
        .format_if_mount_failed = formatIfFail,
        .read_only = false,
        .dont_mount = false,
        .grow_on_mount = false,
    };

    esp_err_t ret = esp_vfs_littlefs_register(&conf);
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to mount LittleFS: %s", esp_err_to_name(ret));
        return ret;
    }

    size_t total = 0, used = 0;
    ret = esp_littlefs_info(partitionLabel_.c_str(), &total, &used);
    if (ret == ESP_OK)
    {
        ESP_LOGI(TAG, "LittleFS mounted: total=%d KB, used=%d KB", total / 1024, used / 1024);
    }
    else
    {
        ESP_LOGW(TAG, "LittleFS mounted, but failed to get info: %s", esp_err_to_name(ret));
    }

    mounted_ = true;
    return ESP_OK;
}

esp_err_t LittleFsService::unmount()
{
    if (!mounted_)
    {
        return ESP_OK;
    }

    esp_err_t ret = esp_vfs_littlefs_unregister(partitionLabel_.c_str());
    if (ret != ESP_OK)
    {
        ESP_LOGE(TAG, "Failed to unmount LittleFS: %s", esp_err_to_name(ret));
        return ret;
    }

    mounted_ = false;
    ESP_LOGI(TAG, "LittleFS unmounted");
    return ESP_OK;
}

LittleFsService::FileStream LittleFsService::openStream(const std::string &path)
{
    FileStream stream = openRawStream(path); // пробуем обычный
    if (stream.ok)
    {
        return stream;
    }

    // Пробуем сжатую версию
    std::string gzPath = path + ".gz";
    stream = openRawStream(gzPath);
    if (stream.ok)
    {
        stream.is_gz = true;
    }

    return stream;
}

LittleFsService::FileStream LittleFsService::openRawStream(const std::string &path)
{
    FileStream stream;

    if (!mounted_)
    {
        ESP_LOGE(TAG, "FS not mounted, cannot open %s", path.c_str());
        return stream;
    }

    std::string fullPath = basePath_ + path;

    struct stat st;
    if (stat(fullPath.c_str(), &st) != 0 || !S_ISREG(st.st_mode))
    {
        // Нормальная ситуация: openStream() сначала пробует plain-версию, затем .gz.
        // Не WARN — это штатный fallback, а не ошибка.
        ESP_LOGD(TAG, "File not found: %s", fullPath.c_str());
        return stream;
    }

    int fd = open(fullPath.c_str(), O_RDONLY);
    if (fd < 0)
    {
        ESP_LOGE(TAG, "Failed to open file: %s", fullPath.c_str());
        return stream;
    }

    stream.fd = fd;
    stream.size = static_cast<size_t>(st.st_size);
    stream.ok = true;

    ESP_LOGD(TAG, "File opened: %s (%d bytes)", fullPath.c_str(), (int)stream.size);
    return stream;
}

ssize_t LittleFsService::readChunk(FileStream &stream, void *buf, size_t len)
{
    if (stream.fd < 0)
    {
        return -1;
    }
    return read(stream.fd, buf, len);
}

void LittleFsService::closeStream(FileStream &stream)
{
    if (stream.fd >= 0)
    {
        close(stream.fd);
        stream.fd = -1;
    }
}
