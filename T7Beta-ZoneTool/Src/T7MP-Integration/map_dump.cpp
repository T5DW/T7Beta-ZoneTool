#include "map_dump.hpp"
#include "offsets/maps.hpp"
#include "build_profiles.hpp"
#include "bots.hpp"
#include "dvar.h"
#include "patches.hpp"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace MapDump {
namespace {
namespace fs = std::filesystem;
using Json = nlohmann::json;
using Bytes = std::vector<unsigned char>;
using namespace offsets::aug21::maps;
std::atomic<bool> pending{false};
unsigned sequence{};

bool copy(std::uintptr_t address, void* output, std::size_t size) {
    SIZE_T received{};
    return address && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
                                        output, size, &received) && received == size;
}
template<class T> T read(std::uintptr_t address) {
    T value{};
    if (!copy(address, &value, sizeof(value)))
        throw std::runtime_error("Map record memory is unavailable");
    return value;
}
std::string hex(std::uintptr_t value) {
    std::ostringstream text;
    text << "0x" << std::hex << value;
    return text.str();
}
std::string assetName(std::uintptr_t address) {
    std::string result;
    for (unsigned i = 0; i < 160; ++i) {
        char value{};
        if (!copy(address + i, &value, 1)) return {};
        if (!value) return result;
        if (value < 32 || value > 126) return {};
        result.push_back(value);
    }
    return {};
}
std::uintptr_t header(unsigned type, std::size_t expected, const std::string& match = {}) {
    const auto descriptor = patches::image_base() + pools + type * poolStride;
    const auto data = read<std::uintptr_t>(descriptor);
    const auto stride = read<unsigned>(descriptor + 8);
    const auto capacity = read<unsigned>(descriptor + 12);
    const auto used = read<unsigned>(descriptor + 20);
    if (stride != expected || capacity != 2 || used > capacity || !used)
        throw std::runtime_error("Map asset pool does not match the August 21 layout");
    for (unsigned i = 0; i < capacity; ++i) {
        const auto record = data + i * stride;
        const auto name = assetName(read<std::uintptr_t>(record));
        if (name.find("mp_") != std::string::npos && (match.empty() || name == match))
            return record;
    }
    throw std::runtime_error("A matching resident map asset was not found");
}
struct Snapshot {
    fs::path folder;
    Json records = Json::array();
    std::size_t total{};
    unsigned missing{};

    void save(const std::string& label, std::uintptr_t address, std::size_t count, std::size_t stride) {
        Json entry{{"label", label}, {"address", hex(address)}, {"count", count}, {"stride", stride}};
        if (!count) {
            entry["status"] = "empty";
            records.push_back(entry);
            return;
        }
        constexpr std::size_t limit = 128 * 1024 * 1024;
        if (!stride || count > limit / stride || total > limit - count * stride)
            throw std::runtime_error("Map snapshot exceeded its byte budget");
        Bytes bytes(count * stride);
        if (!copy(address, bytes.data(), bytes.size())) {
            entry["status"] = "unavailable";
            ++missing;
            records.push_back(entry);
            return;
        }
        const auto file = label + ".bin";
        std::ofstream output(folder / file, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        output.close();
        if (!output) throw std::runtime_error("Cannot save map snapshot data");
        total += bytes.size();
        entry["status"] = "saved";
        entry["file"] = file;
        records.push_back(entry);
    }
    void array(const std::string& label, std::uintptr_t record, std::size_t countOffset,
               std::size_t pointerOffset, std::size_t stride, bool shortCount = false) {
        const auto count = shortCount ? read<std::uint16_t>(record + countOffset)
                                      : read<std::uint32_t>(record + countOffset);
        const auto pointer = read<std::uintptr_t>(record + pointerOffset);
        save(label, pointer, count, stride);
    }
};
void capture() {
    const auto world = header(worldType, worldSize);
    const auto map = assetName(read<std::uintptr_t>(world));
    const auto collision = header(collisionType, collisionSize, map);
    const auto leaf = fs::path(map).stem().string();
    if (leaf.empty() || leaf.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") != std::string::npos)
        throw std::runtime_error("Map name cannot be used as an export folder");
    const auto parent = Mod::Root() / "assets/maps/dumps";
    fs::create_directories(parent);
    const auto suffix = leaf + "-" + std::to_string(GetTickCount64()) + "-" + std::to_string(++sequence);
    const auto staging = parent / (suffix + ".partial");
    const auto output = parent / suffix;
    if (!fs::create_directory(staging)) throw std::runtime_error("Map export folder already exists");
    try {
        Snapshot dump{staging};
        dump.save("gfxworld", world, 1, worldSize);
        dump.save("clipmap", collision, 1, collisionSize);
        const auto info = collision + collisionInfo;
        dump.array("clip_planes", info, 0, 8, 20);
        dump.array("clip_materials", info, 0x10, 0x18, 16);
        dump.array("clip_brush_sides", info, 0x20, 0x28, 16);
        dump.array("clip_leaf_brush_nodes", info, 0x30, 0x38, 24);
        dump.array("clip_leaf_brushes", info, 0x40, 0x48, 2);
        dump.array("clip_vertices", info, 0x50, 0x58, 12);
        dump.array("clip_indices", info, 0x60, 0x68, 4);
        dump.array("clip_brushes", info, 0x70, 0x78, 112, true);
        dump.array("clip_brush_bounds", info, 0x70, 0x80, 24, true);
        dump.array("clip_brush_contents", info, 0x70, 0x88, 4, true);
        dump.array("terrain_vertices", collision, 0xd8, 0xe0, 12);
        dump.array("terrain_triangles", collision, 0xe8, 0xf0, 12);

        const auto draw = world + worldDraw;
        dump.array("world_vertex_stream0", draw, 0x14, 0x18, 1);
        dump.array("world_vertex_stream1", draw, 0x28, 0x30, 1);
        dump.array("world_indices", draw, 0x40, 0x48, 2);
        dump.save("world_surfaces", read<std::uintptr_t>(world + worldDpvs + 0x138),
                  read<unsigned>(world + 0x18), 96);
        dump.array("world_static_model_draw", world + worldDpvs, 4, 0x140, 152);

        const auto entities = read<std::uintptr_t>(collision + collisionEntities);
        if (entities) {
            dump.save("mapents", entities, 1, 0x48);
            dump.array("entity_string", entities, 0x10, 8, 1);
            dump.array("trigger_models", entities, 0x18, 0x20, 8);
            dump.array("trigger_hulls", entities, 0x28, 0x30, 32);
            dump.array("trigger_slabs", entities, 0x38, 0x40, 20);
        }
        Json manifest{{"schema", 1}, {"build", "T7 August 21 2015 PC"}, {"map", map},
                      {"moduleBase", hex(patches::image_base())}, {"bytes", dump.total},
                      {"unavailableArrays", dump.missing}, {"records", dump.records}};
        std::ofstream report(staging / "map-snapshot.json");
        report << manifest.dump(2);
        report.close();
        if (!report) throw std::runtime_error("Cannot save map snapshot manifest");
        fs::rename(staging, output);
        Mod::QueueLine("[Map port] Snapshot saved to " + output.string());
        if (dump.missing) Mod::QueueLine("[Map port] Some arrays are unavailable. See map-snapshot.json.");
    } catch (...) {
        fs::remove_all(staging);
        throw;
    }
}
} // namespace
void Request() {
    if (!BuildProfiles::IsAug21() || !BuildProfiles::Verified() || !Bots::WeaponMatchActive()) {
        Mod::QueueLine("dumpmap requires an active August 21 local match.");
        return;
    }
    pending = true;
    Mod::QueueLine("[Map port] Snapshot queued for the next server frame.");
}
void ServerFrame(bool active) {
    if (!pending.exchange(false)) return;
    if (!active || !BuildProfiles::IsAug21() || !BuildProfiles::Verified()) {
        Mod::QueueLine("[Map port] Snapshot canceled because the match ended.");
        return;
    }
    try { capture(); }
    catch (const std::exception& error) {
        Mod::QueueLine(std::string("[Map port] Snapshot failed: ") + error.what());
    }
}
} // namespace MapDump
