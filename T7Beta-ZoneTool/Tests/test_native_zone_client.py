from pathlib import Path
import subprocess
import json
import tempfile
base = Path(__file__).resolve().parents[1]
source = (base / 'Src/T7MP-Integration/native_zone.cpp').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include "'))
prefix = r'''
#include <map>
#include <cassert>
#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
std::map<std::uintptr_t, std::vector<unsigned char>> memory;
bool validBuild=true, active=false, queueWorks=true;
std::vector<std::string> lines, commands;
namespace utils::hook {
bool read(std::uintptr_t address,void* target,std::size_t size){
 auto i=memory.upper_bound(address);if(i==memory.begin())return false;--i;
 if(address-i->first>i->second.size() || size>i->second.size()-(address-i->first))return false;
 std::memcpy(target,i->second.data()+address-i->first,size);return true;
}}
namespace BuildProfiles {bool IsAug21(){return validBuild;}bool Verified(){return validBuild;}}
namespace Bots {bool WeaponMatchActive(){return active;}}
namespace patches {
std::uintptr_t image_base(){return 0x140000000;}
struct signature{std::uintptr_t address;const unsigned char* bytes;std::size_t size;};
bool verify(signature s){std::vector<unsigned char>b(s.size);return utils::hook::read(s.address,b.data(),b.size())&&!std::memcmp(b.data(),s.bytes,b.size());}
}
namespace Dvar {float GetFloat(const char*){return active?1:0;}bool Execute(const char* text){if(!queueWorks)return false;commands.push_back(text);return true;}}
namespace Mod {std::filesystem::path Root(){return "nuketown/native-zone-client-fixture/mod";}void QueueLine(const std::string&s){lines.push_back(s);}}
#include "../msmc-diag/source/offsets/maps.hpp"
template<class T> void set(std::uintptr_t address,T value){auto i=memory.upper_bound(address);assert(i!=memory.begin());--i;assert(address-i->first+sizeof(value)<=i->second.size());std::memcpy(i->second.data()+address-i->first,&value,sizeof(value));}
void stringSegment(std::uintptr_t address,const std::string& value){memory[address].assign(value.begin(),value.end());memory[address].push_back(0);}
'''
tests = r'''
int main(){
 namespace fs=std::filesystem;
 auto root=fs::path("nuketown/native-zone-client-fixture");fs::remove_all(root);
 auto engine=fs::absolute(root/"engine");fs::create_directories(engine);fs::create_directories(Mod::Root()/"zone");
 fs::copy_file("nuketown/native-zone-tests/t7zt_probe.ff",Mod::Root()/"zone/t7zt_probe.ff");
 auto base=patches::image_base();
 memory[base+0x5ae1e0]={0x48,0x8d,0x05,0xf9,0x3c,0xe1,0x0b,0xc3};
 stringSegment(base+0x63cbee0,engine.string());
 validBuild=false;NativeZone::Test();assert(commands.empty()&&!fs::exists(engine/"t7zt_probe.ff"));validBuild=true;
 active=true;NativeZone::Test();assert(commands.empty()&&!fs::exists(engine/"t7zt_probe.ff"));active=false;
 NativeZone::Test();assert(commands.size()==1&&commands[0]=="loadzone t7zt_probe");assert(fs::exists(engine/"t7zt_probe.ff"));
 NativeZone::Test();assert(commands.size()==2); // identical installed file can be reused
 auto table=base+offsets::aug21::maps::pools;
 memory[table].resize(47*32);
 auto descriptor=table+46*32;
 set<std::uintptr_t>(descriptor,0x200000);set<unsigned>(descriptor+8,24);set<unsigned>(descriptor+12,2);set<unsigned>(descriptor+20,1);
 memory[0x200000].resize(48);set<std::uintptr_t>(0x200000,0x300000);set<unsigned>(0x200008,sizeof(NativeZone::probeText)-1);set<std::uintptr_t>(0x200010,0x400000);
 stringSegment(0x300000,"t7zt/probe.txt");stringSegment(0x400000,NativeZone::probeText);
 NativeZone::Status();bool pass=false;for(auto&s:lines)if(s.find("PASS: engine RawFile")!=std::string::npos)pass=true;assert(pass);
 memory[0x400000][0]^=1;assert(!NativeZone::probeResident());
 {std::ofstream bad(engine/"t7zt_probe.ff",std::ios::binary|std::ios::trunc);bad<<"different";}
 auto count=commands.size();NativeZone::Test();assert(commands.size()==count);assert(fs::file_size(engine/"t7zt_probe.ff")==9);
 // Corrupt a stored payload. The frontend must refuse before touching engine files.
 {auto file=NativeZone::loadFile(Mod::Root()/"zone/t7zt_probe.ff");file[600]^=1;std::ofstream f(Mod::Root()/"zone/t7zt_probe.ff",std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<char*>(file.data()),file.size());}
 NativeZone::Test();assert(commands.size()==count);
 fs::remove_all(root);
 std::cout<<"PASS native zone build/match gates, exact probe validation, native command queuing, resident asset check and existing-file preservation\n";
}
'''
prefix = prefix.replace('"../msmc-diag/source/offsets/maps.hpp"', json.dumps(str(base / 'Src/T7MP-Integration/offsets/maps.hpp')))
tests = tests.replace('"nuketown/native-zone-tests/t7zt_probe.ff"', json.dumps(str(base / 'Examples/NativeZone/t7zt_probe.ff')))
with tempfile.TemporaryDirectory() as temporary:
    file = Path(temporary) / 'native_zone_client_test.cpp'
    binary = Path(temporary) / 'native_zone_client_test'
    file.write_text(prefix + source + tests)
    subprocess.run(['g++', '-std=c++17', str(file), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], cwd=temporary, check=True)

