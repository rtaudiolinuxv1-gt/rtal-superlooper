#include "Lv2Host.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <QStringList>
#include <utility>
#include <vector>

#if SUPERLOOPER_HAS_LILV
#include <lilv/lilv.h>
#endif

namespace {
constexpr uint32_t kMaxLv2BlockFrames = 8192;

#if SUPERLOOPER_HAS_LILV
struct CachedPluginInfo {
    Lv2PluginInfo info;
    const LilvPlugin* plugin = nullptr;
    uint32_t audioInputPorts[2] { 0, 0 };
    uint32_t audioOutputPorts[2] { 0, 0 };
    std::vector<uint32_t> controlInputPorts;
    std::vector<uint32_t> controlOutputPorts;
    std::vector<float> defaultControlValues;
};

struct Lv2WorldState {
    LilvWorld* world = nullptr;
    LilvNode* audioPortClass = nullptr;
    LilvNode* inputPortClass = nullptr;
    LilvNode* outputPortClass = nullptr;
    LilvNode* controlPortClass = nullptr;
    LilvNode* hardRtCapable = nullptr;
    std::vector<CachedPluginInfo> plugins;

    Lv2WorldState()
    {
        world = lilv_world_new();
        if (world == nullptr) {
            return;
        }

        audioPortClass = lilv_new_uri(world, LILV_URI_AUDIO_PORT);
        inputPortClass = lilv_new_uri(world, LILV_URI_INPUT_PORT);
        outputPortClass = lilv_new_uri(world, LILV_URI_OUTPUT_PORT);
        controlPortClass = lilv_new_uri(world, LILV_URI_CONTROL_PORT);
        hardRtCapable = lilv_new_uri(world, "http://lv2plug.in/ns/lv2core#hardRTCapable");
        lilv_world_load_all(world);

        const LilvPlugins* allPlugins = lilv_world_get_all_plugins(world);
        if (allPlugins == nullptr) {
            return;
        }

        LILV_FOREACH(plugins, it, allPlugins) {
            const LilvPlugin* plugin = lilv_plugins_get(allPlugins, it);
            if (plugin == nullptr || !lilv_plugin_verify(plugin)) {
                continue;
            }

            CachedPluginInfo cached;
            cached.plugin = plugin;
            cached.info.uri = QString::fromUtf8(lilv_node_as_uri(lilv_plugin_get_uri(plugin)));

            const LilvNode* nameNode = lilv_plugin_get_name(plugin);
            cached.info.name = nameNode != nullptr
                ? QString::fromUtf8(lilv_node_as_string(nameNode))
                : cached.info.uri;

            const LilvNode* authorNode = lilv_plugin_get_author_name(plugin);
            if (authorNode != nullptr) {
                cached.info.author = QString::fromUtf8(lilv_node_as_string(authorNode));
            }

            cached.info.hardRealtimeCapable = lilv_world_ask(
                world,
                lilv_plugin_get_uri(plugin),
                nullptr,
                hardRtCapable);

            const uint32_t portCount = lilv_plugin_get_num_ports(plugin);
            for (uint32_t portIndex = 0; portIndex < portCount; ++portIndex) {
                const LilvPort* port = lilv_plugin_get_port_by_index(plugin, portIndex);
                if (port == nullptr) {
                    continue;
                }

                const bool isAudio = lilv_port_is_a(plugin, port, audioPortClass);
                const bool isInput = lilv_port_is_a(plugin, port, inputPortClass);
                const bool isOutput = lilv_port_is_a(plugin, port, outputPortClass);
                const bool isControl = lilv_port_is_a(plugin, port, controlPortClass);

                if (isAudio && isInput && cached.info.audioInputCount < 2) {
                    cached.audioInputPorts[static_cast<size_t>(cached.info.audioInputCount)] = portIndex;
                    ++cached.info.audioInputCount;
                } else if (isAudio && isOutput && cached.info.audioOutputCount < 2) {
                    cached.audioOutputPorts[static_cast<size_t>(cached.info.audioOutputCount)] = portIndex;
                    ++cached.info.audioOutputCount;
                } else if (isControl && isInput) {
                    cached.controlInputPorts.push_back(portIndex);
                    LilvNode* def = nullptr;
                    LilvNode* min = nullptr;
                    LilvNode* max = nullptr;
                    lilv_port_get_range(plugin, port, &def, &min, &max);
                    float defValue = 0.0F;
                    if (def != nullptr && (lilv_node_is_float(def) || lilv_node_is_int(def))) {
                        defValue = lilv_node_as_float(def);
                    } else if (min != nullptr && (lilv_node_is_float(min) || lilv_node_is_int(min))) {
                        defValue = lilv_node_as_float(min);
                    }
                    cached.defaultControlValues.push_back(defValue);
                    Lv2ControlPortInfo controlInfo;
                    const LilvNode* symbolNode = lilv_port_get_symbol(plugin, port);
                    const LilvNode* portNameNode = lilv_port_get_name(plugin, port);
                    if (symbolNode != nullptr) {
                        controlInfo.symbol = QString::fromUtf8(lilv_node_as_string(symbolNode));
                    }
                    controlInfo.name = portNameNode != nullptr
                        ? QString::fromUtf8(lilv_node_as_string(portNameNode))
                        : controlInfo.symbol;
                    controlInfo.minimumValue = min != nullptr && (lilv_node_is_float(min) || lilv_node_is_int(min))
                        ? lilv_node_as_float(min)
                        : 0.0F;
                    controlInfo.maximumValue = max != nullptr && (lilv_node_is_float(max) || lilv_node_is_int(max))
                        ? lilv_node_as_float(max)
                        : 1.0F;
                    controlInfo.defaultValue = defValue;
                    if (controlInfo.maximumValue <= controlInfo.minimumValue) {
                        controlInfo.maximumValue = controlInfo.minimumValue + 1.0F;
                    }
                    cached.info.controlInputs.push_back(controlInfo);
                    lilv_node_free(def);
                    lilv_node_free(min);
                    lilv_node_free(max);
                } else if (isControl && isOutput) {
                    cached.controlOutputPorts.push_back(portIndex);
                }
            }

            if (cached.info.audioInputCount == 2 && cached.info.audioOutputCount == 2) {
                plugins.push_back(std::move(cached));
            }
        }

        std::sort(plugins.begin(), plugins.end(), [](const CachedPluginInfo& left, const CachedPluginInfo& right) {
            return QString::localeAwareCompare(left.info.name, right.info.name) < 0;
        });
    }

    ~Lv2WorldState()
    {
        lilv_node_free(audioPortClass);
        lilv_node_free(inputPortClass);
        lilv_node_free(outputPortClass);
        lilv_node_free(controlPortClass);
        lilv_node_free(hardRtCapable);
        lilv_world_free(world);
    }

    [[nodiscard]] const CachedPluginInfo* find(const QString& uri) const
    {
        const auto it = std::find_if(plugins.cbegin(), plugins.cend(), [&uri](const CachedPluginInfo& plugin) {
            return plugin.info.uri == uri;
        });
        return it != plugins.cend() ? &(*it) : nullptr;
    }
};

Lv2WorldState& worldState()
{
    static Lv2WorldState state;
    return state;
}

QStringList unsupportedRequiredFeatures(const CachedPluginInfo& plugin)
{
    QStringList unsupported;
    if (plugin.plugin == nullptr) {
        return unsupported;
    }

    const LilvNodes* requiredFeatures = lilv_plugin_get_required_features(plugin.plugin);
    if (requiredFeatures == nullptr) {
        return unsupported;
    }

    LILV_FOREACH(nodes, it, requiredFeatures) {
        const LilvNode* node = lilv_nodes_get(requiredFeatures, it);
        if (node == nullptr || !lilv_node_is_uri(node)) {
            continue;
        }

        const QString featureUri = QString::fromUtf8(lilv_node_as_uri(node));
        unsupported.push_back(featureUri);
    }

    return unsupported;
}
#endif
}

struct Lv2Processor::Impl {
    QString uri;
    QString name;
#if SUPERLOOPER_HAS_LILV
    LilvInstance* instance = nullptr;
    std::array<float, kMaxLv2BlockFrames> inputLeft {};
    std::array<float, kMaxLv2BlockFrames> inputRight {};
    std::array<float, kMaxLv2BlockFrames> outputLeft {};
    std::array<float, kMaxLv2BlockFrames> outputRight {};
    std::vector<float> controlInputs;
    std::vector<float> controlOutputs;
    std::vector<Lv2ControlPortInfo> controlPortInfo;
#endif
};

Lv2Processor::Lv2Processor()
    : impl_(std::make_unique<Impl>())
{
}

Lv2Processor::~Lv2Processor()
{
#if SUPERLOOPER_HAS_LILV
    if (impl_ != nullptr && impl_->instance != nullptr) {
        lilv_instance_deactivate(impl_->instance);
        lilv_instance_free(impl_->instance);
        impl_->instance = nullptr;
    }
#endif
}

void Lv2Processor::process(float* left, float* right, uint32_t frameCount) noexcept
{
#if SUPERLOOPER_HAS_LILV
    if (impl_ == nullptr || impl_->instance == nullptr || left == nullptr || right == nullptr || frameCount == 0U) {
        return;
    }

    const uint32_t clampedFrames = std::min(frameCount, kMaxLv2BlockFrames);
    std::memcpy(impl_->inputLeft.data(), left, sizeof(float) * clampedFrames);
    std::memcpy(impl_->inputRight.data(), right, sizeof(float) * clampedFrames);
    std::fill_n(impl_->outputLeft.data(), clampedFrames, 0.0F);
    std::fill_n(impl_->outputRight.data(), clampedFrames, 0.0F);
    lilv_instance_run(impl_->instance, clampedFrames);
    std::memcpy(left, impl_->outputLeft.data(), sizeof(float) * clampedFrames);
    std::memcpy(right, impl_->outputRight.data(), sizeof(float) * clampedFrames);
#else
    (void)left;
    (void)right;
    (void)frameCount;
#endif
}

QString Lv2Processor::uri() const
{
    return impl_ != nullptr ? impl_->uri : QString();
}

QString Lv2Processor::name() const
{
    return impl_ != nullptr ? impl_->name : QString();
}

bool Lv2Processor::isValid() const noexcept
{
#if SUPERLOOPER_HAS_LILV
    return impl_ != nullptr && impl_->instance != nullptr;
#else
    return false;
#endif
}

size_t Lv2Processor::controlPortCount() const noexcept
{
#if SUPERLOOPER_HAS_LILV
    return impl_ != nullptr ? impl_->controlInputs.size() : 0U;
#else
    return 0U;
#endif
}

std::optional<Lv2ControlPortInfo> Lv2Processor::controlPortInfo(size_t index) const
{
#if SUPERLOOPER_HAS_LILV
    if (impl_ == nullptr || index >= impl_->controlPortInfo.size()) {
        return std::nullopt;
    }
    return impl_->controlPortInfo[index];
#else
    (void)index;
    return std::nullopt;
#endif
}

float Lv2Processor::controlValue(size_t index) const noexcept
{
#if SUPERLOOPER_HAS_LILV
    if (impl_ == nullptr || index >= impl_->controlInputs.size()) {
        return 0.0F;
    }
    return impl_->controlInputs[index];
#else
    (void)index;
    return 0.0F;
#endif
}

void Lv2Processor::setControlValue(size_t index, float value) noexcept
{
#if SUPERLOOPER_HAS_LILV
    if (impl_ == nullptr || index >= impl_->controlInputs.size()) {
        return;
    }

    const Lv2ControlPortInfo& info = index < impl_->controlPortInfo.size()
        ? impl_->controlPortInfo[index]
        : Lv2ControlPortInfo();
    impl_->controlInputs[index] = std::clamp(value, info.minimumValue, info.maximumValue);
#else
    (void)index;
    (void)value;
#endif
}

bool Lv2Host::isAvailable() noexcept
{
#if SUPERLOOPER_HAS_LILV
    return worldState().world != nullptr;
#else
    return false;
#endif
}

QString Lv2Host::unavailableReason()
{
#if SUPERLOOPER_HAS_LILV
    return isAvailable() ? QString() : QStringLiteral("Lilv could not initialize the LV2 world.");
#else
    return QStringLiteral("SuperLooper was built without Lilv/LV2 support.");
#endif
}

std::vector<Lv2PluginInfo> Lv2Host::availableStereoPlugins()
{
#if SUPERLOOPER_HAS_LILV
    std::vector<Lv2PluginInfo> result;
    if (!isAvailable()) {
        return result;
    }

    result.reserve(worldState().plugins.size());
    for (const CachedPluginInfo& plugin : worldState().plugins) {
        result.push_back(plugin.info);
    }
    return result;
#else
    return {};
#endif
}

QString Lv2Host::displayNameForUri(const QString& uri)
{
#if SUPERLOOPER_HAS_LILV
    if (const CachedPluginInfo* plugin = worldState().find(uri); plugin != nullptr) {
        return plugin->info.name;
    }
#endif
    return uri;
}

std::optional<Lv2PluginInfo> Lv2Host::pluginInfoForUri(const QString& uri)
{
#if SUPERLOOPER_HAS_LILV
    if (const CachedPluginInfo* plugin = worldState().find(uri); plugin != nullptr) {
        return plugin->info;
    }
#else
    (void)uri;
#endif
    return std::nullopt;
}

std::unique_ptr<Lv2Processor> Lv2Host::createProcessor(
    const QString& uri,
    double sampleRate,
    QString* errorMessage)
{
    if (uri.trimmed().isEmpty()) {
        return {};
    }

#if !SUPERLOOPER_HAS_LILV
    if (errorMessage != nullptr) {
        *errorMessage = unavailableReason();
    }
    return {};
#else
    if (!isAvailable()) {
        if (errorMessage != nullptr) {
            *errorMessage = unavailableReason();
        }
        return {};
    }

    const CachedPluginInfo* plugin = worldState().find(uri);
    if (plugin == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("LV2 plugin %1 was not found.").arg(uri);
        }
        return {};
    }

    const QStringList unsupportedFeatures = unsupportedRequiredFeatures(*plugin);
    if (!unsupportedFeatures.isEmpty()) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral(
                "Plugin %1 requires unsupported LV2 host features: %2")
                                .arg(plugin->info.name, unsupportedFeatures.join(QStringLiteral(", ")));
        }
        return {};
    }

    auto processor = std::unique_ptr<Lv2Processor>(new Lv2Processor());
    processor->impl()->uri = plugin->info.uri;
    processor->impl()->name = plugin->info.name;
    processor->impl()->controlInputs = plugin->defaultControlValues;
    processor->impl()->controlOutputs.assign(plugin->controlOutputPorts.size(), 0.0F);
    processor->impl()->controlPortInfo = plugin->info.controlInputs;

    processor->impl()->instance = lilv_plugin_instantiate(
        plugin->plugin,
        sampleRate > 0.0 ? sampleRate : 48000.0,
        nullptr);
    if (processor->impl()->instance == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Could not instantiate %1.").arg(plugin->info.name);
        }
        return {};
    }

    lilv_instance_connect_port(processor->impl()->instance, plugin->audioInputPorts[0], processor->impl()->inputLeft.data());
    lilv_instance_connect_port(processor->impl()->instance, plugin->audioInputPorts[1], processor->impl()->inputRight.data());
    lilv_instance_connect_port(processor->impl()->instance, plugin->audioOutputPorts[0], processor->impl()->outputLeft.data());
    lilv_instance_connect_port(processor->impl()->instance, plugin->audioOutputPorts[1], processor->impl()->outputRight.data());

    for (size_t index = 0; index < plugin->controlInputPorts.size(); ++index) {
        lilv_instance_connect_port(
            processor->impl()->instance,
            plugin->controlInputPorts[index],
            processor->impl()->controlInputs.data() + static_cast<std::ptrdiff_t>(index));
    }
    for (size_t index = 0; index < plugin->controlOutputPorts.size(); ++index) {
        lilv_instance_connect_port(
            processor->impl()->instance,
            plugin->controlOutputPorts[index],
            processor->impl()->controlOutputs.data() + static_cast<std::ptrdiff_t>(index));
    }

    lilv_instance_activate(processor->impl()->instance);
    return processor;
#endif
}
