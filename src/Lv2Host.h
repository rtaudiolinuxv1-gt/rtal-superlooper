#pragma once

#include <optional>
#include <cstdint>
#include <memory>
#include <vector>

#include <QString>

struct Lv2ControlPortInfo {
    QString symbol;
    QString name;
    float minimumValue = 0.0F;
    float maximumValue = 1.0F;
    float defaultValue = 0.0F;
};

struct Lv2PluginInfo {
    QString uri;
    QString name;
    QString author;
    int audioInputCount = 0;
    int audioOutputCount = 0;
    int controlInputCount = 0;
    int controlOutputCount = 0;
    bool hardRealtimeCapable = false;
    std::vector<Lv2ControlPortInfo> controlInputs;
};

class Lv2Processor final
{
public:
    struct Impl;

    Lv2Processor();
    ~Lv2Processor();

    Lv2Processor(const Lv2Processor&) = delete;
    Lv2Processor& operator=(const Lv2Processor&) = delete;

    void process(float* left, float* right, uint32_t frameCount) noexcept;
    [[nodiscard]] QString uri() const;
    [[nodiscard]] QString name() const;
    [[nodiscard]] bool isValid() const noexcept;
    [[nodiscard]] size_t controlPortCount() const noexcept;
    [[nodiscard]] std::optional<Lv2ControlPortInfo> controlPortInfo(size_t index) const;
    [[nodiscard]] float controlValue(size_t index) const noexcept;
    void setControlValue(size_t index, float value) noexcept;
    [[nodiscard]] Impl* impl() noexcept { return impl_.get(); }
    [[nodiscard]] const Impl* impl() const noexcept { return impl_.get(); }

private:
    std::unique_ptr<Impl> impl_;
};

namespace Lv2Host {

[[nodiscard]] bool isAvailable() noexcept;
[[nodiscard]] QString unavailableReason();
[[nodiscard]] std::vector<Lv2PluginInfo> availableStereoPlugins();
[[nodiscard]] QString displayNameForUri(const QString& uri);
[[nodiscard]] std::optional<Lv2PluginInfo> pluginInfoForUri(const QString& uri);
[[nodiscard]] std::unique_ptr<Lv2Processor> createProcessor(
    const QString& uri,
    double sampleRate,
    QString* errorMessage = nullptr);

}
