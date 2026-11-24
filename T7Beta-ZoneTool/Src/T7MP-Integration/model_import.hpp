#pragma once
#include <cstdint>
#include <cstddef>
namespace ModelImport {
    unsigned StringId(const char* name);
    void* PublishRecord(unsigned type, void* record, std::size_t size);
    bool ApplyFamas(uintptr_t view, uintptr_t world, unsigned char* definition);
    bool ApplyWorld(uintptr_t stock, unsigned char* definition);
    bool Apply(std::uintptr_t stockModel, unsigned char* definition);
}
