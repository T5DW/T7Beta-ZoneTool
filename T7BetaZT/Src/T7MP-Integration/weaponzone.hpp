#pragma once
#include <cstdint>
#include <string>
namespace WeaponZone {
    bool ValidName(const std::string& name);
    void Install();
    void Status();
    void Give(bool famas=false);
    void ServerFrame(bool active);
    void Export(const std::string& name, std::uintptr_t record);
}
