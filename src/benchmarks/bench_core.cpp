#include <benchmark/benchmark.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

using json = nlohmann::json;

// ---------------------------------------------------------------------------
// Sample configuration mirroring crankshaft.json structure
// ---------------------------------------------------------------------------
static const char* kSampleConfig = R"({
    "system": {
        "name": "crankshaft",
        "version": "1.0.0",
        "debug": false
    },
    "audio": {
        "default_route": "speaker",
        "sample_rate": 48000,
        "channels": 2,
        "bluetooth": {
            "enabled": true,
            "auto_connect": true
        }
    },
    "android_auto": {
        "enabled": true,
        "resolution": "1080p",
        "fps": 60,
        "wifi_projection": false,
        "usb": {
            "auto_detect": true,
            "timeout_ms": 5000
        }
    },
    "display": {
        "brightness": 80,
        "night_mode": true,
        "theme": "dark"
    }
})";

// ---------------------------------------------------------------------------
// Helpers – mirror the nested-key logic used in ConfigService
// ---------------------------------------------------------------------------
static json getNestedValue(const json& config, const std::string& dotKey) {
    std::istringstream stream(dotKey);
    std::string segment;
    const json* current = &config;
    while (std::getline(stream, segment, '.')) {
        if (!current->is_object() || !current->contains(segment)) {
            return nullptr;
        }
        current = &(*current)[segment];
    }
    return *current;
}

static void setNestedValue(json& config, const std::string& dotKey,
                           const json& value) {
    std::istringstream stream(dotKey);
    std::string segment;
    std::vector<std::string> keys;
    while (std::getline(stream, segment, '.')) {
        keys.push_back(segment);
    }

    json* current = &config;
    for (size_t i = 0; i < keys.size() - 1; ++i) {
        if (!current->contains(keys[i]) ||
            !(*current)[keys[i]].is_object()) {
            (*current)[keys[i]] = json::object();
        }
        current = &(*current)[keys[i]];
    }
    (*current)[keys.back()] = value;
}

// ---------------------------------------------------------------------------
// JSON / Config benchmarks  (ConfigService patterns)
// ---------------------------------------------------------------------------

static void BM_JsonConfigParse(benchmark::State& state) {
    std::string config_str(kSampleConfig);
    for (auto _ : state) {
        auto config = json::parse(config_str);
        benchmark::DoNotOptimize(config);
    }
}
BENCHMARK(BM_JsonConfigParse);

static void BM_JsonConfigSerialize(benchmark::State& state) {
    auto config = json::parse(kSampleConfig);
    for (auto _ : state) {
        auto result = config.dump();
        benchmark::DoNotOptimize(result);
    }
}
BENCHMARK(BM_JsonConfigSerialize);

static void BM_JsonNestedGet_Shallow(benchmark::State& state) {
    auto config = json::parse(kSampleConfig);
    for (auto _ : state) {
        auto val = getNestedValue(config, "display.brightness");
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_JsonNestedGet_Shallow);

static void BM_JsonNestedGet_Deep(benchmark::State& state) {
    auto config = json::parse(kSampleConfig);
    for (auto _ : state) {
        auto val = getNestedValue(config, "android_auto.usb.auto_detect");
        benchmark::DoNotOptimize(val);
    }
}
BENCHMARK(BM_JsonNestedGet_Deep);

static void BM_JsonNestedSet(benchmark::State& state) {
    auto config = json::parse(kSampleConfig);
    for (auto _ : state) {
        setNestedValue(config, "audio.bluetooth.enabled", false);
        benchmark::DoNotOptimize(config);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_JsonNestedSet);

static void BM_JsonConfigRoundtrip(benchmark::State& state) {
    std::string config_str(kSampleConfig);
    for (auto _ : state) {
        auto config = json::parse(config_str);
        config["display"]["brightness"] = 50;
        config["audio"]["sample_rate"] = 44100;
        auto result = config.dump(2);
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_JsonConfigRoundtrip);

// ---------------------------------------------------------------------------
// Event dispatch benchmarks  (EventBus patterns)
// ---------------------------------------------------------------------------

static void BM_EventDispatch_MapLookup(benchmark::State& state) {
    std::map<std::string, std::function<void(const json&)>> handlers;
    handlers["android_auto/device_connected"] = [](const json&) {};
    handlers["android_auto/projection_started"] = [](const json&) {};
    handlers["audio/route_changed"] = [](const json&) {};
    handlers["media/playback_started"] = [](const json&) {};
    handlers["bluetooth/device_paired"] = [](const json&) {};
    handlers["display/brightness_changed"] = [](const json&) {};
    handlers["system/config_updated"] = [](const json&) {};
    handlers["session/device_registered"] = [](const json&) {};

    json payload = {{"device_id", "AA123"}, {"connected", true}};

    for (auto _ : state) {
        auto it = handlers.find("audio/route_changed");
        if (it != handlers.end()) {
            it->second(payload);
        }
        benchmark::DoNotOptimize(it);
    }
}
BENCHMARK(BM_EventDispatch_MapLookup);

static void BM_EventDispatch_WithMutex(benchmark::State& state) {
    std::mutex mtx;
    std::map<std::string, std::function<void(const json&)>> handlers;
    handlers["android_auto/device_connected"] = [](const json&) {};
    handlers["audio/route_changed"] = [](const json&) {};
    handlers["media/playback_started"] = [](const json&) {};

    json payload = {{"device_id", "AA123"}, {"timestamp", 1234567890}};

    for (auto _ : state) {
        std::lock_guard<std::mutex> lock(mtx);
        auto it = handlers.find("audio/route_changed");
        if (it != handlers.end()) {
            it->second(payload);
        }
        benchmark::DoNotOptimize(it);
    }
}
BENCHMARK(BM_EventDispatch_WithMutex);

// ---------------------------------------------------------------------------
// Structured logging benchmarks  (Logger patterns)
// ---------------------------------------------------------------------------

static void BM_LogEntryCreate(benchmark::State& state) {
    for (auto _ : state) {
        json entry;
        entry["timestamp"] = "2025-01-15T10:30:00";
        entry["level"] = "INFO";
        entry["component"] = "AndroidAutoService";
        entry["message"] = "Device connected successfully";
        entry["thread"] = "12345";

        auto result = entry.dump();
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_LogEntryCreate);

static void BM_LogEntryCreateWithContext(benchmark::State& state) {
    json context = {{"device_id", "AA001"},
                    {"device_name", "Pixel 6"},
                    {"connection_type", "USB"},
                    {"firmware_version", "1.2.3"}};

    for (auto _ : state) {
        json entry;
        entry["timestamp"] = "2025-01-15T10:30:00";
        entry["level"] = "INFO";
        entry["component"] = "AndroidAutoService";
        entry["message"] = "Device connected";
        entry["thread"] = "12345";

        for (auto& [key, value] : context.items()) {
            entry[key] = value;
        }

        auto result = entry.dump();
        benchmark::DoNotOptimize(result);
        benchmark::ClobberMemory();
    }
}
BENCHMARK(BM_LogEntryCreateWithContext);

BENCHMARK_MAIN();
