#include "weapondef.hpp"
#include "weaponzone.hpp"
#include "build_profiles.hpp"
#include "bots.hpp"
#include "dvar.h"
#include "patches.hpp"
#include "utils/hook.hpp"
#include <Windows.h>
#include <imgui.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <climits>
#include <mutex>
#include <vector>
#include <cstring>
#include "offsets/weapondef.hpp"

namespace WeaponDef
{
    namespace
    {
        using Json = nlohmann::json;
        namespace fs = std::filesystem;
        using namespace offsets::aug21::weapon;
        std::atomic<bool> pending{}, showError{};
        std::mutex catalogMutex, dumpMutex;
        std::string requestedDump;
        std::atomic<uintptr_t> mapDvar{};
        fs::path path() { return Mod::Root() / L"assets/weapondef/weapondef.json"; }

        template<class T> T read(uintptr_t p)
        {
            T v{};
            if (!utils::hook::read(p, &v, sizeof(v)))
                throw std::runtime_error("weapon registry changed during read");
            return v;
        }
        struct Gun { uintptr_t damage, interval, clip; };
        using Guns = std::map<std::string, Gun>;

        Guns scan(unsigned last)
        {
            Guns result;
            auto table = patches::image_base() + patches::rva::WeaponCompleteDefTable;
            for (unsigned i = 1; i <= last; ++i)
            {
                auto complete = read<uintptr_t>(table + i * sizeof(uintptr_t));
                if (!complete) continue;
                auto def = read<uintptr_t>(complete + defPointer);
                auto text = read<uintptr_t>(complete);
                if (!text || !def) continue;
                std::string name;
                for (unsigned k = 0; k < 128; ++k)
                {
                    char c = read<char>(text + k);
                    if (!c) break;
                    name.push_back(c);
                }
                if (name.empty() || name.size() == 128) continue;
                result.emplace(name, Gun{def + damageOffset, def + intervalOffset, complete + clipOffset});
            }
            return result;
        }

        std::string contents()
        {
            std::ifstream f(path(), std::ios::binary);
            if (!f) throw std::runtime_error("cannot open weapondef.json");
            return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
        }

        void publish(const Json &j, bool replace, const std::string &prior = {})
        {
            auto target = path();
            fs::create_directories(target.parent_path());
            auto temp = target;
            temp += L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out << j.dump(2) << '\n';
            out.close();
            if (!out) throw std::runtime_error("cannot write weapondef.json");
            if (replace && contents() != prior)
            {
                fs::remove(temp);
                throw std::runtime_error("file changed during export; retry loadef");
            }
            if (!MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH |
                            (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
            {
                auto err = GetLastError();
                fs::remove(temp);
                if (!replace && (err == ERROR_ALREADY_EXISTS || err == ERROR_FILE_EXISTS)) return;
                throw std::runtime_error("cannot publish weapondef.json");
            }
        }

        void ensureFile()
        {
            if (!fs::exists(path())) publish(Json{{"weapons", Json::object()}}, false);
        }

        bool playableMap()
        {
            auto dvar = mapDvar.load();
            uintptr_t text{};
            uint32_t hash{}, type{};
            char map[16]{};
            return dvar && utils::hook::read(dvar, &hash, sizeof(hash)) && hash == 3079603908u &&
                   utils::hook::read(dvar + 20, &type, sizeof(type)) && type == 8 &&
                   utils::hook::read(dvar + 32, &text, sizeof(text)) && text &&
                   utils::hook::read(text, map, sizeof(map)) && map[0] &&
                   std::strncmp(map, "core_frontend", sizeof(map)) != 0;
        }

        int integer(const Json &row, const char *key, int current)
        {
            const auto &v = row.at(key);
            if (v.is_number_integer() && v == current) return current;
            if (!v.is_number_integer() || v.get<double>() < 0 || v.get<double>() > INT_MAX)
                throw std::runtime_error(std::string(key) + " must be a nonnegative integer");
            return v.get<int>();
        }

        int interval(const Json &row)
        {
            const auto &v = row.at("fireRateRPM");
            if (!v.is_number()) throw std::runtime_error("fireRateRPM must be numeric");
            double rpm = v.get<double>();
            if (!(rpm > 0)) throw std::runtime_error("fireRateRPM must be positive or null");
            double ms = std::round(60000.0 / rpm);
            if (rpm < 0 || !std::isfinite(ms) || ms < 1 || ms > INT_MAX)
                throw std::runtime_error("fireRateRPM is outside the representable range");
            return static_cast<int>(ms);
        }

        void update(const Guns &guns, bool apply)
        {
            auto before = contents();
            auto j = Json::parse(before);
            auto &rows = j.at("weapons");
            if (!rows.is_object()) throw std::runtime_error("weapons must be a JSON object");
            size_t added = 0;
            std::map<uintptr_t, int> desired;
            for (const auto &[name, gun] : guns)
            {
                if (!rows.contains(name))
                {
                    int ms = read<int>(gun.interval);
                    rows[name] = {{"damage",read<int>(gun.damage)}, {"fireRateRPM", ms > 0 ? Json(60000.0 / ms) : Json(nullptr)},
                                  {"magSize",read<int>(gun.clip)}};
                    ++added;
                }
                if (!apply) continue;
                try
                {
                    const auto &row = rows.at(name);
                    auto set = [&](uintptr_t addr, int value)
                    {
                        auto [it, inserted] = desired.emplace(addr, value);
                        if (!inserted && it->second != value)
                            throw std::runtime_error("conflicting values for a shared definition");
                    };
                    set(gun.damage, integer(row,"damage",read<int>(gun.damage)));
                    if (!row.at("fireRateRPM").is_null()) set(gun.interval, interval(row));
                    set(gun.clip, integer(row,"magSize",read<int>(gun.clip)));
                }
                catch (const std::exception &e) { throw std::runtime_error(name + ": " + e.what()); }
            }
            if (added) publish(j,true,before);
            if (!apply) return;
            struct Edit { uintptr_t addr; int oldValue, value; };
            std::vector<Edit> edits;
            for (auto [addr,value] : desired)
            {
                int old = read<int>(addr);
                if (old != value) edits.push_back({addr,old,value});
            }
            size_t done = 0;
            for (const auto &edit : edits)
            {
                if (!utils::hook::write(edit.addr, &edit.value, sizeof(int)))
                {
                    bool restored = true;
                    for (size_t i = done + 1; i; --i)
                    {
                        const auto &prev = edits[i-1];
                        restored &= utils::hook::write(prev.addr,&prev.oldValue,sizeof(int));
                    }
                    throw std::runtime_error(restored ? "write failed; reverted changes" : "write failure; restart the match");
                }
                ++done;
            }
            Mod::QueueLine("^2loadef: " + std::to_string(done) + " changed field(s), " +
                           std::to_string(guns.size()) + " registered weapons.");
        }
    }

    void Setup()
    {
        if (!BuildProfiles::IsAug21()) return;
        WeaponZone::Install();
        try { ensureFile(); }
        catch (const std::exception &e) { Mod::QueueLine(std::string("^1WeaponDef: ") + e.what()); }
    }

    void CatalogFrame()
    {
        static ULONGLONG next{};
        auto now = GetTickCount64();
        if (now < next) return;
        next = now + 3000;
        if (!BuildProfiles::IsAug21() || !BuildProfiles::Verified()) return;
        mapDvar = reinterpret_cast<uintptr_t>(Dvar::FindVar("mapname"));
        if (Bots::WeaponMatchActive() && playableMap()) return;
        // Frontend zones also register weapons. Export those before the first match.
        std::unique_lock lock(catalogMutex, std::try_to_lock);
        if (!lock) return;
        static unsigned lastSeen = 0;
        try
        {
            auto last = read<unsigned>(patches::image_base() + lastIndexRva);
            if (!last || last >= 512) { lastSeen = 0; return; }
            if (last == lastSeen && fs::exists(path())) return;
            ensureFile();
            auto guns = scan(last);
            if (!guns.empty() && last == read<unsigned>(patches::image_base() + lastIndexRva))
            {
                update(guns, false);
                lastSeen = last;
            }
        }
        catch (const std::exception &) { lastSeen = 0; } // retry when zones finish loading
    }

    void RequestDump(const std::string& name)
    {
        if (!WeaponZone::ValidName(name)) { Mod::QueueLine("Usage: dumpweapon smg_standard_mp"); return; }
        if (!BuildProfiles::IsAug21() || !BuildProfiles::Verified() || !Bots::ServerTickInstalled() ||
            !Bots::WeaponMatchActive() || !playableMap()) {
            Mod::QueueLine("dumpweapon requires a running August 21 local match."); return;
        }
        std::lock_guard lock(dumpMutex);
        if (!requestedDump.empty()) { Mod::QueueLine("A weapon export is pending."); return; }
        requestedDump = name;
    }

    void Request()
    {
        if (!BuildProfiles::IsAug21() || !BuildProfiles::Verified())
        {
            Mod::QueueLine("^1loadef requires the verified August 21 build.");
            return;
        }
        mapDvar = reinterpret_cast<uintptr_t>(Dvar::FindVar("mapname"));
        if (!Bots::WeaponMatchActive() || !playableMap()) { showError = true; return; }
        if (!Bots::ServerTickInstalled()) { Mod::QueueLine("^1WeaponDef server tick unavailable."); return; }
        pending = true;
    }

    void ServerFrame(bool active)
    {
        static ULONGLONG nextScan = 0;
        static unsigned previousLast = 0;
        static ULONGLONG next = 0;
        static std::string previousText, lastError, notifiedText;
        WeaponZone::ServerFrame(active && playableMap());
        const bool apply = pending.exchange(false);
        std::string dump;
        { std::lock_guard lock(dumpMutex); dump.swap(requestedDump); }
        if (!dump.empty() && (!active || !playableMap())) Mod::QueueLine("Weapon export canceled: match ended.");
        if (!active)
        {
            if (apply) showError = true;
            previousLast = 0; next = 0;
            return;
        }
        auto now = GetTickCount64();
        if (dump.empty() && !apply && now < next) return;
        next = now + 1000;
        if (!playableMap())
        {
            if (apply) showError = true;
            previousLast = 0;
            return;
        }
        std::lock_guard lock(catalogMutex);
        try
        {
            ensureFile();
            auto text = contents();
            unsigned last = read<unsigned>(patches::image_base() + lastIndexRva);
            if (!last || last >= 512)
            {
                if (apply || !dump.empty()) Mod::QueueLine("^1Weapon registry is not ready.");
                previousLast = 0;
                return;
            }
            if (!dump.empty()) {
                auto guns = scan(last);
                auto gun = guns.find(dump);
                if (gun == guns.end()) Mod::QueueLine("Weapon is not registered: " + dump);
                else WeaponZone::Export(dump, gun->second.clip - clipOffset);
            }
            if (previousLast != last || now >= nextScan || apply || text != previousText)
            {
                if (!apply && !previousText.empty() && text != previousText && text != notifiedText)
                    Mod::QueueLine("^3weapondef.json changed. Type loadef to apply.");
                notifiedText = text;
                update(scan(last), apply);
                previousText = contents();
                previousLast = last;
                nextScan = now + 15000;
            }
            lastError.clear();
        }
        catch (const std::exception &e)
        {
            if (apply || !dump.empty() || lastError != e.what()) Mod::QueueLine(std::string("^1WeaponDef: ") + e.what());
            lastError = e.what();
        }
    }

    void DrawError()
    {
        if (showError.exchange(false)) ImGui::OpenPopup("ERROR:###WeaponDefLobby");
        if (ImGui::BeginPopupModal("ERROR:###WeaponDefLobby", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::TextUnformatted("WeaponDef Cannot be loaded in lobby go into game for that.");
            ImGui::TextUnformatted("-t5dw");
            if (ImGui::Button("OK") || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }
}
