#include "model_import.hpp"
#include "camo.hpp"
#include "patches.hpp"
#include "offsets/weaponzone.hpp"
#include "dvar.h"
#include "utils/hook.hpp"
#include <d3d11.h>
#include <wrl/client.h>
#include <array>
#include <fstream>
#include <mutex>
#include <vector>
#include <stdexcept>
#include <cstring>
#include <cmath>
#include <nlohmann/json.hpp>
namespace ModelImport {
namespace {
using Microsoft::WRL::ComPtr;
struct ModelState {
std::mutex mutex;
alignas(16) std::array<unsigned char,0x170> model{};
alignas(16) std::array<unsigned char,0x78> lod{};
alignas(16) std::array<std::array<unsigned char,0x60>,10> surfaces{};
alignas(16) std::array<unsigned char,0x50> mesh{};
alignas(16) std::array<unsigned char,0x2a0> material{};
alignas(16) std::array<unsigned char,0x18> materials{};
alignas(16) std::array<std::array<unsigned char,0x10>,10> rigids{};
std::vector<unsigned char> geometry,textureTable,parents,rotations,translations,baseMatrices,classification;
std::vector<unsigned> boneIds;
std::array<void*,10> materialHandle{};
std::array<float,10> materialMips{};
ComPtr<ID3D11Buffer> positions,attributes,weights,indices;
std::string name;
bool prepared{};
uintptr_t sourceModel{};
unsigned char* registeredModel{};
unsigned char* registeredMaterial{};
std::string materialName;
ModelState(const char* id):name(id),materialName(std::string("mc/")+id+"_body"){}
};
ModelState viewState("t5dw_msmc_prepared_view"),worldState("t5dw_msmc_prepared_world");
ModelState famasView("t5dw_famas_view"), famasWorld("t5dw_famas_world");
bool inPool(const void* asset, unsigned poolRva, unsigned count, size_t stride) {
 auto pointer=reinterpret_cast<uintptr_t>(asset);
 auto first=patches::image_base()+poolRva;
 return pointer>=first && pointer-first<count*stride && (pointer-first)%stride==0;
}
unsigned char* publish(unsigned type, void* asset) {
 using namespace offsets::aug21::zone;
 auto address=patches::image_base()+publishAsset;
 if(!patches::verify({address,publishEntry,sizeof(publishEntry)}))
     throw std::runtime_error("Native asset publication signature mismatch");
 using Publish=void(__fastcall*)(void**,int);
 void* pointer=asset;
 patches::as<Publish>(address)(&pointer,type);
 if(!pointer)throw std::runtime_error("Native asset registration returned no header");
 if(type==4 && !inPool(pointer,modelPool,modelCount,0x170))
     throw std::runtime_error("Imported model is outside the engine model pool");
 if(type==6 && !inPool(pointer,materialPool,materialCount,0x2a0))
     throw std::runtime_error("Imported material is outside the engine material pool");
 return static_cast<unsigned char*>(pointer);
}
template<class T>T read(uintptr_t p) {
 T value{};
 if(!utils::hook::read(p,&value,sizeof(value))) throw std::runtime_error("Unavailable source model record");
 return value;
}
template<class A,class T>void put(A& a,size_t n,T value){std::memcpy(a.data()+n,&value,sizeof(value));}
void copy(uintptr_t p,void* data,size_t n) {
 if(!utils::hook::read(p,data,n))throw std::runtime_error("Source model snapshot failed");
}
bool upload(ID3D11Device* device,const unsigned char* data,UINT size,UINT bind,ComPtr<ID3D11Buffer>& out) {
 D3D11_BUFFER_DESC desc{};desc.ByteWidth=size;desc.Usage=D3D11_USAGE_IMMUTABLE;desc.BindFlags=bind;
 D3D11_SUBRESOURCE_DATA init{data,0,0};
 return SUCCEEDED(device->CreateBuffer(&desc,&init,&out));
}
}

unsigned StringId(const char* name) {
 static const unsigned char entry[]={0x45,0x8b,0xc8,0x48,0x83,0xc8,0xff};
 auto address=patches::image_base()+0x1695b0;
 if(!patches::verify({address,entry,sizeof(entry)}))throw std::runtime_error("Script-string signature mismatch");
 using Get=unsigned(__fastcall*)(const char*,unsigned,unsigned);
 auto id=patches::as<Get>(address)(name,4,8);
 if(!id)throw std::runtime_error("Bone string registration failed");
 return id;
}
void* PublishRecord(unsigned type,void* record,size_t size) {
 std::vector<unsigned char> staged(size);std::memcpy(staged.data(),record,size);
 auto expectedName=read<const char*>(reinterpret_cast<uintptr_t>(record));
 auto native=publish(type,record);
 char text[128]{};
 size_t length=std::strlen(expectedName)+1;
 if(length>sizeof(text) || !utils::hook::read(read<uintptr_t>(reinterpret_cast<uintptr_t>(native)),text,length) ||
    std::memcmp(text,expectedName,length))throw std::runtime_error("Native asset identity mismatch");
 if(!utils::hook::write(reinterpret_cast<uintptr_t>(native),staged.data(),size))
    throw std::runtime_error("Native asset initialization failed");
 std::vector<unsigned char> stored(size);copy(reinterpret_cast<uintptr_t>(native),stored.data(),size);
 if(stored!=staged)throw std::runtime_error("Native asset verification failed");
 return native;
}
bool ApplyModel(uintptr_t stock,unsigned char* definition,unsigned offset,const std::filesystem::path& folder,ModelState& state) {
 auto& mutex=state.mutex;
 auto& model=state.model;
 auto& lod=state.lod;
 auto& surfaces=state.surfaces;
 auto& mesh=state.mesh;
 auto& material=state.material;
 auto& materials=state.materials;
 auto& rigids=state.rigids;
 auto& geometry=state.geometry;
 auto& textureTable=state.textureTable;
 auto& parents=state.parents;
 auto& rotations=state.rotations;
 auto& translations=state.translations;
 auto& baseMatrices=state.baseMatrices;
 auto& classification=state.classification;
 auto& boneIds=state.boneIds;
 auto& materialHandle=state.materialHandle;
 auto& materialMips=state.materialMips;
 auto& positions=state.positions;
 auto& attributes=state.attributes;
 auto& weights=state.weights;
 auto& indices=state.indices;
 auto& prepared=state.prepared;
 auto& sourceModel=state.sourceModel;
 auto& registeredModel=state.registeredModel;
 auto& registeredMaterial=state.registeredMaterial;
 auto& surface=surfaces[0];
 auto name=state.name.c_str();auto materialName=state.materialName.c_str();
 std::lock_guard lock(mutex);
 try {
  if(prepared && sourceModel==stock &&
     inPool(registeredModel,offsets::aug21::zone::modelPool,offsets::aug21::zone::modelCount,0x170) &&
     read<uintptr_t>(reinterpret_cast<uintptr_t>(registeredModel)+0xb0)==reinterpret_cast<uintptr_t>(materials.data()) &&
     read<uintptr_t>(reinterpret_cast<uintptr_t>(materials.data())+0x10)==reinterpret_cast<uintptr_t>(materialMips.data())) {
   auto pointer=registeredModel;std::memcpy(definition+offset,&pointer,8);return true;
  }
  auto device=Camo::ModelDevice();
  if(!device)throw std::runtime_error("D3D11 model device is unavailable");
  auto file=folder/"mesh.bin";
  auto bytes=std::filesystem::file_size(file);
  if(bytes<16 || bytes>4*1024*1024)throw std::runtime_error("Invalid MSMC mesh size");
  std::ifstream in(file,std::ios::binary);geometry.resize(bytes);
  if(!in.read(reinterpret_cast<char*>(geometry.data()),bytes))throw std::runtime_error("Cannot read MSMC mesh");
  if(std::memcmp(geometry.data(),"MSMC0001",8))throw std::runtime_error("Invalid MSMC mesh header");
  uint32_t vertices{},triangles{};
  std::memcpy(&vertices,geometry.data()+8,4);std::memcpy(&triangles,geometry.data()+12,4);
  if(!vertices || vertices>65535 || !triangles || triangles>65535 ||
     bytes!=16ull+vertices*40ull+triangles*6ull)throw std::runtime_error("Invalid MSMC geometry counts");
  auto p=geometry.data()+16;
  for(unsigned i=0;i<vertices*3;++i) {
   float coordinate{};
   std::memcpy(&coordinate,p+i*4,4);
   if(!std::isfinite(coordinate))throw std::runtime_error("Invalid MSMC vertex position");
  }
  for(unsigned i=0;i<triangles*3;++i) {
   uint16_t index{};
   std::memcpy(&index,p+vertices*40+i*2,2);
   if(index>=vertices)throw std::runtime_error("Invalid MSMC triangle index");
  }
  if(!upload(device,p,vertices*12,D3D11_BIND_VERTEX_BUFFER,positions) ||
     !upload(device,p+vertices*12,vertices*16,D3D11_BIND_VERTEX_BUFFER,attributes) ||
     !upload(device,p+vertices*28,vertices*12,D3D11_BIND_VERTEX_BUFFER,weights) ||
     !upload(device,p+vertices*40,triangles*6,D3D11_BIND_INDEX_BUFFER,indices))
     throw std::runtime_error("MSMC GPU buffer upload failed");
  copy(stock,model.data(),model.size());
  std::ifstream skeletonFile(folder/"skeleton.json");
  nlohmann::json skeleton;skeletonFile>>skeleton;
  auto boneNames=skeleton.at("names").get<std::vector<std::string>>();
  if(boneNames.empty() || boneNames.size()>32)throw std::runtime_error("Invalid MSMC skeleton size");
  // Older COD4 packages used a private root, preventing the weapon and
  // player model from sharing the native weapon attachment.
  if(boneNames.front()=="t7mp_msmc_root") boneNames.front()="tag_weapon";
  boneIds.clear();for(const auto& bone:boneNames)boneIds.push_back(StringId(bone.c_str()));
  auto stream=[&](const char* key,size_t length) {
   auto text=skeleton.at(key).get<std::string>();
   if(text.size()!=length*2)throw std::runtime_error("Invalid MSMC skeleton stream");
   std::vector<unsigned char> result(length);
   auto digit=[](char c)->unsigned {if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;throw std::runtime_error("Invalid skeleton hex");};
   for(size_t i=0;i<length;++i)result[i]=static_cast<unsigned char>((digit(text[2*i])<<4)|digit(text[2*i+1]));
   return result;
  };
  parents=stream("parents",boneIds.size()-1);
  for(size_t child=1;child<boneIds.size();++child) {
   if(!parents[child-1] || parents[child-1]>child)
    throw std::runtime_error("Invalid MSMC bone parent");
  }
  rotations=stream("rotations",(boneIds.size()-1)*8);
  translations=stream("translations",(boneIds.size()-1)*12);
  baseMatrices=stream("baseMatrices",boneIds.size()*32);
  classification.assign(boneIds.size(),0);
  model[8]=static_cast<unsigned char>(boneIds.size());model[9]=1;put(model,10,uint16_t(0));
  put(model,0x10,boneIds.data());put(model,0x18,parents.data());put(model,0x20,rotations.data());
  put(model,0x28,translations.data());put(model,0x30,classification.data());put(model,0x38,baseMatrices.data());
  auto stockLod=read<uintptr_t>(stock+0x70);
  copy(stockLod,lod.data(),lod.size());
  copy(read<uintptr_t>(stockLod+0x68),surface.data(),surface.size());
  copy(read<uintptr_t>(stockLod+0x70),mesh.data(),mesh.size());
  auto materialList=read<uintptr_t>(stock+0xb0);
  auto handles=read<uintptr_t>(materialList+8);
  auto firstMaterial=read<uintptr_t>(handles);
  copy(firstMaterial,material.data(),material.size());
  auto count=read<uint8_t>(firstMaterial+0x270);
  if(!count || count>64)throw std::runtime_error("Invalid model material");
  textureTable.resize(count*0x20);
  copy(read<uintptr_t>(firstMaterial+0x280),textureTable.data(),textureTable.size());
  bool colored=false;
  for(unsigned n=0;n<count;++n) {
   uint32_t semantic{};std::memcpy(&semantic,textureTable.data()+n*32+8,4);
   const char* map=nullptr;
   switch(semantic) {
    case 0xa0ab1041: case 0x264f0732:map="color.rgba";break;
    case 0x59d30d0f:map="normal.rgba";break;
    case 0x6d0a6c98:map="gloss.rgba";break;
    case 0x07176bf2:map="ao.rgba";break;
    case 0x77151208:map="mask.rgba";break;
   }
   if(!map)continue;
   uintptr_t original{};std::memcpy(&original,textureTable.data()+n*32,8);
   auto textureFolder = (&state==&famasWorld) ? folder.parent_path() : folder;
   auto image=Camo::ModelImage(original,textureFolder/map);
   if(!image)throw std::runtime_error("MSMC diffuse image upload failed");
   std::memcpy(textureTable.data()+n*32,&image,8);
   if(semantic==0xa0ab1041 || semantic==0x264f0732)colored=true;
  }
  if(!colored)throw std::runtime_error("Model material has no known diffuse slot");
  put(material,0,materialName);
  put(material,0x280,textureTable.data());
  registeredMaterial=static_cast<unsigned char*>(PublishRecord(6,material.data(),material.size()));
  if(read<uintptr_t>(reinterpret_cast<uintptr_t>(registeredMaterial)+0x280)!=reinterpret_cast<uintptr_t>(textureTable.data()))
      throw std::runtime_error("Registered material has a stale texture table");
  std::ifstream surfaceFile(folder/"surfaces.json");
  nlohmann::json groups;surfaceFile>>groups;
  if(!groups.is_array() || groups.empty() || groups.size()>surfaces.size())
      throw std::runtime_error("Invalid MSMC surface catalog");
  // At game RVA 0x3da426 the texture streamer reads list+0x10 and
  // immediately dereferences its float entry. Every material needs storage.
  float mip=1.0f;auto sourceMips=read<uintptr_t>(materialList+0x10);
  if(sourceMips) {
   float candidate{};
   if(utils::hook::read(sourceMips,&candidate,sizeof(candidate)) && std::isfinite(candidate) && candidate>0)
    mip=candidate;
  }
  materialMips.fill(mip);
  // The August prototype builds the DObj bone request from twelve words
  // at LOD+0x0c or model+0x13c. Neither may reference the donor skeleton.
  std::array<uint32_t,12> poseMask{};
  uint32_t nextVertex=0,nextIndex=0;
  for(size_t n=0;n<groups.size();++n) {
   const auto& group=groups[n];
   auto bone=group.at("bone").get<unsigned>();
   auto first=group.at("vertexFirst").get<uint32_t>();
   auto count=group.at("vertexCount").get<uint32_t>();
   auto index=group.at("indexFirst").get<uint32_t>();
   auto faces=group.at("triangleCount").get<uint32_t>();
   if(bone>=boneIds.size() || first!=nextVertex || index!=nextIndex ||
      !count || count>65535 || !faces || faces>65535 || first+count>vertices || index+faces*3>triangles*3)
       throw std::runtime_error("Invalid MSMC surface bounds");
   for(unsigned k=0;k<faces*3;++k) {
    uint16_t vertex{};std::memcpy(&vertex,p+vertices*40+(index+k)*2,2);
    if(vertex>=count)throw std::runtime_error("MSMC surface index out of bounds");
   }
   // Each native rigid draw run must agree with the vertex weight stream.
   // Reject inconsistent palettes rather than drawing vertices with a
   // different bone's matrix.
   for(unsigned k=0;k<count;++k) {
    auto weights=p+vertices*28+(first+k)*12;
    uint16_t vertexBone{};std::memcpy(&vertexBone,weights+4,2);
    if(weights[0]!=255 || weights[1] || weights[2] || weights[3] || vertexBone!=bone)
     throw std::runtime_error("MSMC rigid part and vertex bone disagree");
   }
   materialHandle[n]=registeredMaterial;
   auto& run=rigids[n];run.fill(0);
   // The prototype reads a plain bone index at +0 and scales it by 32
   // at RVA 0x3a128c. This is not an IW-style byte offset.
   put(run,0,uint16_t(bone));put(run,2,uint16_t(count));put(run,6,uint16_t(faces));
   auto& part=surfaces[n];part.fill(0);part[1]=1;part[2]=0x22;
   put(part,4,uint16_t(count));put(part,6,uint16_t(faces));
   put(part,8,first);put(part,12,index);
   put(part,0x10,mesh.data());put(part,0x18,run.data());
   put(part,0x20,uint32_t(0x80000000u >> bone));
   for(unsigned ancestor=bone;;ancestor-=parents[ancestor-1]) {
    poseMask[ancestor/32] |= 0x80000000u >> (ancestor%32);
    if(ancestor==0)break;
   }
   nextVertex+=count;nextIndex+=faces*3;
  }
  if(nextVertex!=vertices || nextIndex!=triangles*3)
      throw std::runtime_error("MSMC surfaces omit geometry");
  materials.fill(0);put(materials,0,uint16_t(groups.size()));put(materials,8,materialHandle.data());put(materials,0x10,materialMips.data());
  auto flags=uint32_t(2);
  mesh.fill(0);put(mesh,0,flags);
  put(mesh,4,vertices);put(mesh,8,vertices);put(mesh,12,triangles*3);
  put(mesh,0x10,p);put(mesh,0x18,uint32_t(bytes-16));
  put(mesh,0x1c,uint32_t(0));put(mesh,0x20,vertices*12);
  put(mesh,0x28,vertices*28);put(mesh,0x24,vertices*40);
  put(mesh,0x30,positions.Get());put(mesh,0x38,attributes.Get());
  put(mesh,0x40,weights.Get());put(mesh,0x48,indices.Get());
  put(lod,0x48,uint64_t(0));
  std::memcpy(lod.data()+0x0c,poseMask.data(),sizeof(poseMask));
  std::memcpy(model.data()+0x13c,poseMask.data(),sizeof(poseMask));
  lod[0x3c]=static_cast<unsigned char>(groups.size());lod[0x3d]=0;put(lod,0x68,surfaces.data());put(lod,0x70,mesh.data());
  put(model,0,name);model[0x40]=1;
  for(unsigned n=0;n<8;++n)put(model,0x70+n*8,n==0 ? lod.data() : nullptr);
  put(model,0xb0,materials.data());
  // The native linker may return an existing header for this asset name.
  // Publish first to obtain a pool slot, then initialize our own slot from the
  // complete staged record before the weapon can expose it to the renderer.
  registeredModel=static_cast<unsigned char*>(PublishRecord(4,model.data(),model.size()));
  auto pointer=registeredModel;std::memcpy(definition+offset,&pointer,8);
  sourceModel=stock;prepared=true;
  auto index=(reinterpret_cast<uintptr_t>(registeredModel)-patches::image_base()-offsets::aug21::zone::modelPool)/0x170;
  Mod::QueueLine("[MSMC] Native model pool index "+std::to_string(index)+", vertices "+std::to_string(vertices)+".");
  return true;
 }catch(const std::exception& e){Mod::QueueLine(std::string("[MSMC] ")+e.what());return false;}
}
bool Apply(uintptr_t stock,unsigned char* definition) {
 return ApplyModel(stock,definition,8,Mod::Root()/"assets/models/msmc",viewState);
}
bool ApplyWorld(uintptr_t stock,unsigned char* definition) {
 return ApplyModel(stock,definition,0x850,Mod::Root()/"assets/models/msmc/world",worldState);
}

bool ApplyFamas(uintptr_t view, uintptr_t world, unsigned char* definition) {
 const auto folder=Mod::Root()/"assets/models/famas";
 return ApplyModel(view,definition,8,folder,famasView) &&
        ApplyModel(world,definition,0x850,folder/"world",famasWorld);
}

}
