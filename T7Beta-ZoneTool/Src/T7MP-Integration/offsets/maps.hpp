#pragma once
#include <cstddef>
#include <cstdint>

namespace offsets::aug21::maps {
    // Aug 21 dump: descriptor data/stride/capacity/used/free-head at +0/+8/+12/+20/+24.
    inline constexpr std::uintptr_t pools = 0x5f257b0;
    inline constexpr std::size_t poolStride = 32;
    inline constexpr unsigned collisionType = 12, worldType = 16;
    inline constexpr std::size_t collisionSize = 0x2d0, worldSize = 0x2040;
    // The native loadzone command is registered at RVA 0x1d680b.
    // Its callback queues the engine loader. Directory accessor returns a char buffer.
    inline constexpr std::uintptr_t loadZoneCallback = 0x1d6ca0;
    inline constexpr std::uintptr_t zoneDirectoryAccessor = 0x5ae1e0;
    inline constexpr std::uintptr_t zoneDirectoryString = 0x63cbee0;
    inline constexpr unsigned rawfileType = 46;
    // Load_GfxWorld 0x1b45a0, Load_clipMap_t 0x1c56b0, Load_MapEnts 0x1b6840.
    inline constexpr std::size_t worldDraw = 0x278, worldDpvs = 0x318;
    inline constexpr std::size_t collisionInfo = 0x10, collisionEntities = 0x148;
}
