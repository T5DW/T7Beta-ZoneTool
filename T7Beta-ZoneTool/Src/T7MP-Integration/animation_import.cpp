#include "animation_import.hpp"
#include "model_import.hpp"
#include "dvar.h"
#include "utils/hook.hpp"
#include <nlohmann/json.hpp>
#include <array>
#include <vector>
#include <memory>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstring>
namespace AnimationImport {
namespace {
using Json=nlohmann::json;
struct Animation {
 alignas(16) std::array<unsigned char,0xf8> header{};
 // The prototype event walker dereferences entry zero, even for an empty
 // event list. Every imported animation owns a terminal event at time 1.
 struct Notify { unsigned name; float time; unsigned type; unsigned param; };
 static_assert(sizeof(Notify)==16);
 std::array<Notify,1> notifies{};
 std::string name;
 std::vector<unsigned> bones;
 std::vector<unsigned char> dataBytes,dataShort,dataInt,randomShort;
 void* native{};
};
struct AnimSet {
std::vector<std::unique_ptr<Animation>> animations;
std::array<const char*,189> names{};
bool loaded{};
};
AnimSet msmcSet, famasSet;
template<class T>void put(std::array<unsigned char,0xf8>& h,size_t p,T value){std::memcpy(h.data()+p,&value,sizeof(value));}
std::vector<unsigned char> bytes(const Json& j,const char* field) {
 auto text=j.at(field).get<std::string>();
 if(text.size()%2 || text.size()>4*1024*1024)throw std::runtime_error("Invalid animation stream");
 auto digit=[](char c)->unsigned{if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Invalid animation hex");};
 std::vector<unsigned char> out(text.size()/2);
 for(size_t i=0;i<out.size();++i)out[i]=static_cast<unsigned char>((digit(text[2*i])<<4)|digit(text[2*i+1]));
 return out;
}
}
bool Apply(unsigned char* weapon,bool famas) {
 auto& [animations,names,loaded]=famas?famasSet:msmcSet;
 const std::string prefix=famas?"am_bo_famas_":"am_t6_msmc_";
 try {
  auto folder=Mod::Root()/"assets/models"/(famas?"famas":"msmc");
  if(!loaded) {
   std::vector<std::unique_ptr<Animation>> pending;
   std::ifstream indexFile(folder/"animations.json");Json index;indexFile>>index;
   if(!index.is_array() || index.size()>32)throw std::runtime_error("Invalid animation catalog");
   for(const auto& row:index) {
    auto source=row.at("source").get<std::string>();
    if(source.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_")!=source.npos)throw std::runtime_error("Invalid animation ID");
    std::ifstream file(folder/"animations"/(source+".json"));Json j;file>>j;
    auto a=std::make_unique<Animation>();a->name=j.at("name");
    auto frames=j.at("frames").get<unsigned>();auto boneNames=j.at("bones").get<std::vector<std::string>>();
    float fps=j.at("fps");
    if(frames>254 || !frames || boneNames.empty() || boneNames.size()>254 || fps<1 || fps>240)throw std::runtime_error("Invalid animation dimensions");
    const bool adsTorso = j.value("adsTorsoDelta",false);
    if(adsTorso && ((source!=prefix+"ads_up" && source!=prefix+"ads_down") ||
                    boneNames.size()!=1 || boneNames.front()!="tag_torso" || a->name!="t5dw_"+source))
     throw std::runtime_error("Invalid MSMC sight transition");
    for(const auto& name:boneNames) {
     if(name=="t7mp_msmc_root")
      throw std::runtime_error("Old COD4 pose data in "+source+". Extract the updated MSMC animation folder.");
     if(name=="tag_view" || name=="tag_ads" || name=="tag_cambone" || name=="tag_camera" || (name=="tag_torso" && !adsTorso))
      throw std::runtime_error("Camera/root bone tracks require reconversion; replace the MSMC animation assets");
     a->bones.push_back(ModelImport::StringId(name.c_str()));
    }
    a->dataBytes=bytes(j,"dataBytes");a->dataShort=bytes(j,"dataShort");
    a->dataInt=bytes(j,"dataInt");a->randomShort=bytes(j,"randomShort");
    size_t count=a->bones.size();
    auto keyFrames=j.at("keyFrames").get<std::vector<unsigned>>();
    if(keyFrames.size()<2 || keyFrames.front()!=0 || keyFrames.back()!=frames ||
       keyFrames.size()>=frames+1)throw std::runtime_error("Animation keyframes must use a sparse stream");
    auto keys=keyFrames.size();
    if(a->dataBytes.size()!=count*(2*keys+1) || a->dataShort.size()!=count*4 ||
       a->dataInt.size()!=count*24 || a->randomShort.size()!=count*keys*14)
       throw std::runtime_error("Animation stream lengths do not match the frame counts");
    put(a->header,0,a->name.c_str());
    put(a->header,0x14,unsigned(a->dataBytes.size()));
    put(a->header,0x0c,unsigned(a->dataShort.size()/2));
    put(a->header,0x18,unsigned(a->dataInt.size()/4));
    put(a->header,0x48,unsigned(a->randomShort.size()/2));
    put(a->header,0x20,uint16_t(frames));a->header[0x24]=j.at("loop").get<bool>()?1:0;
    put(a->header,0x34,uint16_t(count)); // full quaternion rotations
    put(a->header,0x3c,uint16_t(count)); // precise ushort translations
    put(a->header,0x42,uint16_t(count));
    if(adsTorso) a->header[0x44]=6; // XAnim additive overlay, relative to hip pose.
    put(a->header,0x50,fps);put(a->header,0x54,fps/frames);
    put(a->header,0x68,a->bones.data());put(a->header,0x70,a->dataBytes.data());
    put(a->header,0x78,a->dataShort.data());put(a->header,0x80,a->dataInt.data());
    put(a->header,0x88,a->randomShort.data());
    a->notifies[0]={ModelImport::StringId("end"),1.0f,0,0};
    put(a->header,0xc0,a->notifies.data());a->header[0xc8]=1;
    pending.push_back(std::move(a));
   }
   animations.swap(pending);
   loaded=true;
  }
  for(auto& a:animations) {
   if(!a->notifies[0].name || a->notifies[0].time!=1.0f)
    throw std::runtime_error("Animation terminal event is invalid");
   a->native=ModelImport::PublishRecord(3,a->header.data(),a->header.size());
   uintptr_t table{};unsigned char count{};
   auto native=reinterpret_cast<uintptr_t>(a->native);
   if(!utils::hook::read(native+0xc0,&table,sizeof(table)) ||
      !utils::hook::read(native+0xc8,&count,sizeof(count)) ||
      table!=reinterpret_cast<uintptr_t>(a->notifies.data()) || count!=1)
    throw std::runtime_error("Registered animation has a stale event table");
  }
  uintptr_t old{};
  std::memcpy(&old,weapon+0x48,8);
  if(!old || !utils::hook::read(old,names.data(),sizeof(names)))throw std::runtime_error("Weapon animation-name array is unavailable");
  const std::pair<unsigned,const char*> replacements[]={
   {1,"idle"},{4,"fire"},{10,"fire"},{35,"reload"},{36,"reload_empty"},
   {41,"fastmag_reload"},{42,"fastmag_reload_empty"},{43,"fastmag_reload"},{44,"fastmag_reload_empty"},
   {45,"pullout"},{46,"first_raise"},{47,"putaway"},
   {52,"pullout"},{53,"putaway"},{54,"pullout"},{55,"putaway"},
   {64,"sprint_in"},{65,"sprint_loop"},{66,"sprint_out"},
   {76,"crawl_in"},{77,"crawl_forward"},{78,"crawl_back"},{79,"crawl_right"},{80,"crawl_left"},{81,"crawl_out"},
   {96,"ads_fire"},{98,"ads_fire"},{101,"slide_in"},{102,"slide_air_in"},{103,"slide_loop"},{104,"slide_out"},
   {126,"ads_fire"},{165,"ads_up"},{166,"ads_down"}
  };
  // Slots are recovered from the prototype weapon field table, including holes.
  for(auto [slot,suffix]:replacements) {
   if (famas) {
    if(slot==10) suffix="last_shot";
    if(slot==52) suffix="pullout_quick";
    if(slot==53) suffix="putaway_quick";
    if(slot==41 || slot==43) suffix="reload";
    if(slot==42 || slot==44) suffix="reload_empty";
   }
   auto wanted="t5dw_"+prefix+suffix;
   for(const auto& a:animations)if(a->name==wanted){names.at(slot)=a->name.c_str();break;}
  }
  uint32_t reload=famas?2500:2733,reloadEmpty=famas?3250:3300;
  std::memcpy(weapon+0x198,&reload,4);std::memcpy(weapon+0x19c,&reloadEmpty,4);
  uintptr_t def{};std::memcpy(&def,weapon+0x18,8);
  uint32_t add=famas?1867:2100;
  std::memcpy(reinterpret_cast<void*>(def+0xa60),&add,4);std::memcpy(reinterpret_cast<void*>(def+0xa64),&add,4);
  const char** owned=names.data();std::memcpy(weapon+0x48,&owned,8);
  Mod::QueueLine(prefix+" animation names installed.");
  return true;
 }catch(const std::exception& e){Mod::QueueLine(std::string("[MSMC animation] ")+e.what());return false;}
}
}
