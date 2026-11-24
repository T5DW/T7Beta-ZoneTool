#include "weaponzone.hpp"
#include "map_dump.hpp"
#include "lua_runtime.hpp"
#include "crash_capture.hpp"
#include "model_import.hpp"
#include "animation_import.hpp"
#include "weapon_audio.hpp"
#include "build_profiles.hpp"
#include "bots.hpp"
#include "dvar.h"
#include "patches.hpp"
#include "offsets/weaponzone.hpp"
#include "offsets/weapondef.hpp"
#include "utils/hook.hpp"
#include <Windows.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace WeaponZone {
namespace {
using Json = nlohmann::json;
using namespace offsets::aug21::zone;
using namespace offsets::aug21::weapon;
namespace fs = std::filesystem;
using Bytes = std::vector<unsigned char>;
using Lookup = uint64_t(__fastcall*)(const char*,int,bool);
using Register = unsigned(__fastcall*)(void*,unsigned,int);
utils::hook::detour lookupHook;
std::atomic<bool> installed{};
std::atomic<int> givePending{-1};
std::recursive_mutex registrationLock;
struct Weapon {
    std::string baseName, variantName;
    int magazine{};
    bool registering{};
    unsigned publishedIndex{};
    alignas(16) std::array<unsigned char,completeSize> variant{};
    alignas(16) std::array<unsigned char,definitionSize> definition{};
    std::array<uintptr_t,64> attachments{};
    std::array<uintptr_t,128> uniqueAttachments{};
    alignas(16) std::array<unsigned char,0xf0> sounds{};
    std::array<uintptr_t,5> viewAttachments{}, worldAttachments{};
};
std::array<Weapon,2> weapons;
template<class T> T get(uintptr_t p) {
    T v{};
    if (!utils::hook::read(p,&v,sizeof(v))) throw std::runtime_error("Weapon data became unavailable");
    return v;
}
template<class A,class T> void set(A& dest,size_t offset,T value) {
    std::memcpy(dest.data()+offset,&value,sizeof(value));
}
std::string assetString(uintptr_t addr) {
    std::string s;
    for (unsigned i=0; addr && i<256; ++i) {
        unsigned char c{};
        if (!utils::hook::read(addr+i,&c,1)) return {};
        if (!c) return s;
        if (c<32 || c>126) return {};
        s+=static_cast<char>(c);
    }
    return {};
}
uintptr_t table() { return patches::image_base()+patches::rva::WeaponCompleteDefTable; }
uintptr_t countAddr() { return patches::image_base()+lastIndexRva; }
uintptr_t slot(unsigned i) { return get<uintptr_t>(table()+8*i); }

Json readVariant(const fs::path& path) {
    auto len=fs::file_size(path);
    if (len<700 || len>4096) throw std::runtime_error("Invalid weapon-zone size");
    Bytes buf(static_cast<size_t>(len));
    std::ifstream f(path,std::ios::binary);
    if (!f.read(reinterpret_cast<char*>(buf.data()),buf.size())) throw std::runtime_error("Cannot read weapon zone");
    auto word=[&](size_t at,unsigned n) {
        uint64_t v=0; for (unsigned i=0;i<n;++i) v|=uint64_t(buf.at(at+i))<<(8*i); return v;
    };
    constexpr size_t h=0x248, body=h+16;
    const uint64_t next=UINT64_MAX;
    auto size=word(h,4), padded=word(h+8,4);
    if (memcmp(buf.data(),"TAff0000",8) || word(8,4)!=0x1fb || word(12,4)!=0 ||
        memcmp(buf.data()+248,"t7mp_weapon",12) || word(h+12,4)!=h || size!=word(144,8) ||
        size!=word(h+4,4) || padded!=(size+3)/4*4 || body+padded+80!=len ||
        word(body+padded,8)!=0 || word(body+padded+8,4)!=0 || word(body+padded+12,4)!=body+padded)
        throw std::runtime_error("Expected a stored single-block T7MP weapon zone");
    constexpr size_t record=body+72;
    size_t cursor=record+completeSize;
    size_t end=body+size;
    if (cursor>=end || word(168,8)!=completeSize || word(body,8)!=1 || word(body+8,8)!=next ||
        word(body+16,8) || word(body+24,8) || word(body+32,8)!=1 || word(body+40,8)!=next ||
        word(body+48,8) || word(body+56,8)!=23 || word(body+64,8)!=next ||
        word(record,8)!=next || word(record+8,8)!=next)
        throw std::runtime_error("Invalid T7MP stock-derived weapon record");
    auto id=[&]() {
        size_t start=cursor;
        while (cursor<end && buf[cursor]!=0) ++cursor;
        if (cursor==end) throw std::runtime_error("Unterminated weapon ID");
        std::string result(buf.begin()+start,buf.begin()+cursor++);
        if (!ValidName(result)) throw std::runtime_error("Invalid weapon ID");
        return result;
    };
    auto name=id(), base=id();
    auto clip=word(record+clipOffset,4);
    if (base==name || cursor!=end || !clip || clip>999)
        throw std::runtime_error("Invalid weapon identity or clip size");
    for(size_t i=16;i<completeSize;++i)
        if ((i<clipOffset || i>=clipOffset+4) && buf[record+i])
            throw std::runtime_error("Unsupported field in stock-derived weapon record");
    Json j={{"base",base},{"name",name},{"magSize",clip}};
    return j;
}

unsigned findRegistered(unsigned last,const std::string& name) {
    for (unsigned i=1;i<=last;++i) {
        auto record=slot(i);
        std::array<char,64> text{};
        if (record && utils::hook::read(get<uintptr_t>(record),text.data(),name.size()+1) &&
            std::memcmp(text.data(),name.c_str(),name.size()+1)==0) return i;
    }
    return 0;
}

// Called on the engine's registration path. A clone gets private mutable records.
// Attachment registration rewrites attachment-owned backreferences. The bare variant
// therefore supplies private, empty attachment arrays.
unsigned registerFrom(Weapon& weapon,unsigned stockIndex,int mode) {
    auto& [baseName,variantName,magazine,registering,publishedIndex,variant,definition,
           attachments,uniqueAttachments,sounds,viewAttachments,worldAttachments]=weapon;
    std::lock_guard lock(registrationLock);
    auto last=get<unsigned>(countAddr());
    if (!last || last>=512 || !stockIndex || stockIndex>last) return 0;
    if (publishedIndex && publishedIndex<=last && slot(publishedIndex)==reinterpret_cast<uintptr_t>(variant.data()))
        return publishedIndex;
    auto existing=findRegistered(last,variantName);
    if (existing) return slot(existing)==reinterpret_cast<uintptr_t>(variant.data()) ? existing : 0;
    if (last==511 || registering) return 0;
    auto stock=slot(stockIndex);
    if (!stock || assetString(get<uintptr_t>(stock))!=baseName) return 0;
    auto def=get<uintptr_t>(stock+defPointer);
    if (!utils::hook::read(stock,variant.data(),variant.size()) ||
        !utils::hook::read(def,definition.data(),definition.size())) return 0;
    set(variant,0,variantName.c_str());
    set(variant,8,variantName.c_str());
    set(variant,defPointer,definition.data());
    set(variant,0x38,attachments.data());
    set(variant,0x40,uniqueAttachments.data());
    set(variant,clipOffset,magazine);
    if (variantName=="t5dw_msmc_mp" &&
        (!ModelImport::Apply(get<uintptr_t>(def+8),definition.data()) ||
         !ModelImport::ApplyWorld(get<uintptr_t>(def+0x850),definition.data()) ||
         !AnimationImport::Apply(variant.data()))) return 0;
    const bool famas = variantName=="t5dw_famas_mp";
    if (famas) {
        if (!ModelImport::ApplyFamas(get<uintptr_t>(def+8), get<uintptr_t>(def+0x850), definition.data()) ||
            !AnimationImport::Apply(variant.data(),true)) return 0;
        set(definition,damageOffset,40);
        set(definition,intervalOffset,65);
        set(variant,0x198,2500);
        set(variant,0x19c,3250);
    }
    if (famas || variantName=="t5dw_msmc_mp") {
        // The bare port contains its own magazine. Both donor arrays are
        // five XModel handles; keep valid empty arrays for indexed consumers.
        set(variant,0x58,viewAttachments.data());
        set(variant,0x60,worldAttachments.data());
        set(definition,0x878,uintptr_t(0)); // Kuda worldClipModel
        const char* display=famas?"FAMAS":"MSMC";set(variant,0x20,display);
        // Native +0x120c40 selects layered fire sounds through thirteen
        // pointers at +0x18. +0x120c70 separately selects direct fire hashes.
        // Own and silence both tables only on this weapon clone.
        auto soundData=get<uintptr_t>(def+0x360);
        if(soundData) {
            if(!utils::hook::read(soundData,sounds.data(),sounds.size()))
                throw std::runtime_error("MSMC base sound record is unavailable");
            std::memset(sounds.data()+0x18,0,13*sizeof(uintptr_t));
            std::memset(sounds.data()+0x80,0,12*sizeof(uint32_t));
            set(definition,0x360,sounds.data());
        }
    }
    registering=true;
    struct Done { bool& flag; ~Done(){flag=false;} } done{registering};
    unsigned index=last+1;
    *reinterpret_cast<unsigned*>(countAddr())=index; // same reservation as the August 21 caller
    auto registerFn=patches::as<Register>(patches::image_base()+registerWeapon);
    registerFn(variant.data(),index,mode);
    publishedIndex=index;
    Mod::QueueLine("Weapon zone registered "+variantName+" ["+std::to_string(index)+"] from "+baseName);
    return index;
}
uint64_t __fastcall lookupWeapon(const char* name,int mode,bool allowRegister) {
    auto orig=lookupHook.original<Lookup>();
    if (!name) return orig(name,mode,allowRegister);
    for (auto& weapon:weapons) {
        if (weapon.variantName.empty()) continue;
        const bool donor=weapon.baseName==name;
        if (!donor && weapon.variantName!=name) continue;
        uint64_t stock=0;
        try {
            if (donor) {
                stock=orig(name,mode,allowRegister);
                if (allowRegister && (stock&511)) registerFrom(weapon,unsigned(stock&511),mode);
                return stock;
            }
            auto last=get<unsigned>(countAddr());
            if (last>=512) return 0;
            auto known=findRegistered(last,weapon.variantName);
            if (known || !allowRegister) return known;
            stock=orig(weapon.baseName.c_str(),mode,true);
            return registerFrom(weapon,unsigned(stock&511),mode);
        } catch(const std::exception& e){Mod::QueueLine(std::string("Weapon zone: ")+e.what());return donor?stock:0;}
    }
    return orig(name,mode,allowRegister);
}

std::string hex(const unsigned char* bytes,size_t size) {
    static constexpr char chars[]="0123456789abcdef";
    std::string out(size*2,'0');
    for(size_t i=0;i<size;++i) {out[2*i]=chars[bytes[i]>>4];out[2*i+1]=chars[bytes[i]&15];}
    return out;
}
}
bool ValidName(const std::string& s) {
    return !s.empty() && s.size()<=63 && s.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")==s.npos;
}
void Install() {
    if (!BuildProfiles::IsAug21() || !BuildProfiles::Verified()) return;
    try {
        auto path=Mod::Root()/"zone/t7mp_weapon.ff";
        if (!fs::exists(path)) return;
        auto j=readVariant(path);
        CrashCapture::Install();
        auto configure=[](Weapon& weapon,const Json& data){
            weapon.baseName=data.at("base"); weapon.variantName=data.at("name"); weapon.magazine=data.at("magSize");
        };
        configure(weapons[0],j);
        auto second=path.parent_path()/"t7mp_famas.ff";
        if (fs::exists(second)) {
            auto famas=readVariant(second);
            if (famas.at("name")!="t5dw_famas_mp" || famas.at("base")!="ar_standard_mp")
                throw std::runtime_error("Unexpected FAMAS zone identity");
            configure(weapons[1],famas);
        }
        auto base=patches::image_base();
        if (std::memcmp(reinterpret_cast<const void*>(base+lookup),lookupEntry,sizeof(lookupEntry)) ||
            std::memcmp(reinterpret_cast<const void*>(base+registerWeapon),registerEntry,sizeof(registerEntry)))
            throw std::runtime_error("Weapon registration signature mismatch");
        if (!lookupHook.create(base+lookup,reinterpret_cast<void*>(lookupWeapon)))
            throw std::runtime_error("Could not install weapon lookup");
        installed=true;
        for (auto& w:weapons) if (!w.variantName.empty()) Mod::QueueLine("Loaded weapon zone "+w.variantName+"; waiting for "+w.baseName);
    } catch(const std::exception& e){ Mod::QueueLine(std::string("Weapon zone: ")+e.what()); }
}
void Status() {
    if (!installed) { Mod::QueueLine("Weapon zone inactive. Check .t7mp-mod/zone/t7mp_weapon.ff and startup messages."); return; }
    try {
        auto last=get<unsigned>(countAddr());
        if (last>=512) throw std::runtime_error("Weapon registry out of range");
        for(auto& w:weapons) for(const auto& name:{w.baseName,w.variantName}) {
            if(name.empty()) continue;
            auto i=findRegistered(last,name);
            if (!i) { Mod::QueueLine(name+" is waiting for registration."); continue; }
            auto record=slot(i);
            auto model=get<uintptr_t>(get<uintptr_t>(record+defPointer)+8);
            Mod::QueueLine(name+" index "+std::to_string(i)+", magazine "+std::to_string(get<int>(record+clipOffset))+
                           ", viewmodel "+assetString(get<uintptr_t>(model)));
        }
    } catch(const std::exception& e){Mod::QueueLine(e.what());}
}
void Give(bool famas) {
    auto& variantName=weapons[famas?1:0].variantName;
    if(variantName.empty()){Mod::QueueLine("Weapon zone unavailable.");return;}
    if (!installed || !Bots::WeaponMatchActive()) { Mod::QueueLine("Start a local match with the weapon zone loaded."); return; }
    if(variantName=="t5dw_msmc_mp") CrashCapture::Arm();
    givePending=famas?1:0;
}
void ServerFrame(bool active) {
    static bool wasActive=false;
    if (active && !wasActive && installed) LuaRuntime::SyncClassWeapons();
    wasActive=active;
    MapDump::ServerFrame(active);
    WeaponAudio::Pump(active);
    int request=givePending.exchange(-1);
    if (request<0) return;
    auto& weapon=weapons.at(request);
    auto& variantName=weapon.variantName;
    if (!active) { Mod::QueueLine("Weapon grant canceled: match ended."); return; }
    try {
        auto last=get<unsigned>(countAddr());
        if (last>=512) throw std::runtime_error("Invalid weapon count");
        auto i=findRegistered(last,variantName);
        if (!i) i=registerFrom(weapon,findRegistered(last,weapon.baseName),1); // multiplayer host
        if (!i || slot(i)!=reinterpret_cast<uintptr_t>(weapon.variant.data())) {
            Mod::QueueLine("Weapon grant failed: base unavailable, ID in use, or registry full."); return;
        }
        if (Dvar::Execute(("set t7mp_zone_give "+variantName).c_str())) {
            Mod::QueueLine("Sent weapon grant to the local host GSC.");
            if (variantName=="t5dw_msmc_mp") {
                try { Export(variantName, slot(i)); }
                catch (const std::exception& e) {
                    Mod::QueueLine(std::string("[MSMC] Weapon export failed: ")+e.what());
                }
            }
        }
    } catch(const std::exception& e){ Mod::QueueLine(e.what()); }
}


namespace {
std::string captureBytes(uintptr_t address,size_t size) {
    if (!size) return {};
    if (!address || size>16*1024*1024) throw std::runtime_error("Invalid model block size");
    Bytes block(size);
    if (!utils::hook::read(address,block.data(),size))
        throw std::runtime_error("Model block is unavailable");
    return hex(block.data(),block.size());
}
Json captureModel(uintptr_t model) {
    Json result={{"name",assetString(get<uintptr_t>(model))}};
    try {
        result["headerHex"]=captureBytes(model,0x170);
        auto bones=get<uint8_t>(model+8);
        auto roots=get<uint8_t>(model+9);
        auto cosmetic=get<uint16_t>(model+10);
        auto total=unsigned(bones)+cosmetic;
        if (roots>bones || total>512) throw std::runtime_error("Invalid model bone counts");
        result["boneCount"]=bones; result["rootCount"]=roots; result["cosmeticCount"]=cosmetic;
        result["boneIdsHex"]=captureBytes(get<uintptr_t>(model+0x10),total*4);
        result["parentsHex"]=captureBytes(get<uintptr_t>(model+0x18),total-roots);
        result["localRotationsHex"]=captureBytes(get<uintptr_t>(model+0x20),(total-roots)*8);
        result["localTranslationsHex"]=captureBytes(get<uintptr_t>(model+0x28),(total-roots)*12);
        result["baseMatricesHex"]=captureBytes(get<uintptr_t>(model+0x38),total*32);
        auto lod=get<uintptr_t>(model+0x70);
        if (!lod) throw std::runtime_error("LOD0 is unavailable");
        result["lod0HeaderHex"]=captureBytes(lod,0x78);
        auto surfaces=get<uint8_t>(lod+0x3c);
        if (!surfaces || surfaces>128) throw std::runtime_error("Invalid LOD0 surface count");
        result["surfaceCount"]=surfaces;
        auto surfaceTable=get<uintptr_t>(lod+0x68);
        result["surfacesHex"]=captureBytes(surfaceTable,surfaces*0x60);
        result["modelPoseMaskHex"]=captureBytes(model+0x13c,0x30);
        result["lodPoseMaskHex"]=captureBytes(lod+0x0c,0x30);
        auto& rigidParts=result["rigidParts"]=Json::array();
        for(unsigned i=0;i<surfaces;++i) {
            auto surface=surfaceTable+i*0x60;
            auto runCount=get<uint8_t>(surface+1);
            if (!runCount) continue;
            rigidParts.push_back({{"surface",i},{"maskHex",captureBytes(surface+0x20,0x30)},
                {"runsHex",captureBytes(get<uintptr_t>(surface+0x18),runCount*0x10)}});
        }
        auto mesh=get<uintptr_t>(lod+0x70);
        result["meshHeaderHex"]=captureBytes(mesh,0x50);
        auto flags=get<uint32_t>(mesh);
        auto length=get<uint32_t>(mesh+0x18);
        result["meshFlags"]=flags;
        result["meshBytes"]=length;
        if ((flags&2) && length) {
            result["meshDataHex"]=captureBytes(get<uintptr_t>(mesh+0x10),length);
        } else result["meshDataStatus"]="Mesh is streamed out. Equip the Kuda and export again.";
        auto materials=get<uintptr_t>(model+0xb0);
        auto handles=materials ? get<uintptr_t>(materials+8) : 0;
        result["materials"]=Json::array();
        for(unsigned n=0;handles && n<surfaces;++n) {
            Json row={{"surface",n}};
            try {
                auto material=get<uintptr_t>(handles+n*8);
                row["name"]=assetString(get<uintptr_t>(material));
                row["headerHex"]=captureBytes(material,0x2a0);
                auto textures=get<uint8_t>(material+0x270);
                if(textures>64) throw std::runtime_error("Invalid texture count");
                row["textureTableHex"]=captureBytes(get<uintptr_t>(material+0x280),textures*0x20);
            } catch(const std::exception& e) {row["captureError"]=e.what();}
            result["materials"].push_back(std::move(row));
        }
    } catch(const std::exception& e) {result["captureError"]=e.what();}
    return result;
}
}
void Export(const std::string& name,uintptr_t record) {
    if (!ValidName(name)) throw std::runtime_error("Invalid weapon ID");
    auto def=get<uintptr_t>(record+defPointer);
    auto complete=get<std::array<unsigned char,completeSize>>(record);
    auto data=get<std::array<unsigned char,definitionSize>>(def);
    Json models=Json::object(), anims=Json::array(), snapshots=Json::object();
    for (const auto& field:std::array<std::pair<const char*,unsigned>,4>{{
        {"gunModel",8},{"frontendModel",0x28},{"handModel",0x50},{"worldModel",0x850}}}) {
        auto p=get<uintptr_t>(def+field.second);
        if (p) {
            models[field.first]=assetString(get<uintptr_t>(p));
            if (field.second==8) snapshots[field.first]=captureModel(p);
        }
    }
    auto animArray=get<uintptr_t>(record+0x48);
    if (animArray) for(unsigned i=0;i<189;++i) {
        auto p=get<uintptr_t>(animArray+i*sizeof(uintptr_t));
        if (p) anims.push_back({{"slot",i},{"name",assetString(p)}});
    }
    Json j={{"schema",1},{"build","T7MP-Aug21"},{"name",name},{"magSize",get<int>(record+clipOffset)},
        {"damage",get<int>(def+damageOffset)},{"fireIntervalMs",get<int>(def+intervalOffset)},
        {"models",models},{"modelSnapshots",snapshots},{"animations",anims},{"completeRecordHex",hex(complete.data(),complete.size())},
        {"definitionRecordHex",hex(data.data(),data.size())}};
    auto folder=Mod::Root()/"assets/weapon_exports";
    fs::create_directories(folder);
    auto path=folder/(name+".json");
    auto tmp=folder/(name+"."+std::to_string(GetCurrentProcessId())+".tmp");
    std::ofstream out(tmp,std::ios::binary);
    out<<j.dump(2)<<'\n'; out.close();
    if (!out || !MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec; fs::remove(tmp,ec); throw std::runtime_error("Could not publish weapon export");
    }
    Mod::QueueLine("Exported "+path.string());
}
}
