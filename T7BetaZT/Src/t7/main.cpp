#include "lz4.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;
constexpr size_t headerSize = 0x248;
constexpr uint64_t nextPointer = UINT64_MAX;

uint64_t readLE(const Bytes& data, size_t pos, unsigned width) {
    uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= uint64_t(data.at(pos+i)) << (8*i);
    return value;
}
void put(Bytes& data, size_t pos, uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        data.at(pos+i) = uint8_t(value >> (8*i));
}
void append(Bytes& data, uint64_t value, unsigned width) {
    size_t pos = data.size();
    data.resize(pos+width);
    put(data, pos, value, width);
}
bool validName(const std::string& value, bool path) {
    if (value.empty() || value.front() == '/' || value.back() == '/' ||
        value.find("..") != value.npos || value.find("//") != value.npos) return false;
    const std::string allowed = "abcdefghijklmnopqrstuvwxyz0123456789_";
    return value.find_first_not_of(allowed + (path ? "/.-" : "")) == value.npos;
}
void inspect(const fs::path& file) {
    Bytes h(headerSize);
    std::ifstream in(file, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(h.data()), h.size()))
        throw std::runtime_error("Cannot read full zone header");
    if (std::string(h.begin(), h.begin()+8) != "TAff0000" || readLE(h,8,4)!=0x1fb)
        throw std::runtime_error("Expected a T7 beta fastfile with version 0x1FB");
    auto end = std::find(h.begin()+248, h.begin()+312, 0);
    if (end == h.begin()+312) throw std::runtime_error("Unterminated zone name");
    std::cout << "Name: " << std::string(h.begin()+248,end)
        << "\nVersion: 0x1FB\nCompression ID: " << int(h[13])
        << "\nEncrypted: " << int(h[15]) << "\nPayload bytes: " << readLE(h,144,8) << '\n';
}
void writeZone(const std::string& zone,const Bytes& body,size_t tempBytes,size_t virtualBytes,
               const fs::path& output,bool stored) {
    Bytes ff(headerSize);
    const std::string magic = "TAff0000";
    std::copy(magic.begin(),magic.end(),ff.begin());
    put(ff,8,0x1fb,4); ff[13]=stored ? 0 : 3; // LZ4
    put(ff,144,body.size(),8);
    put(ff,168,tempBytes,8);
    put(ff,168+5*8,virtualBytes,8);
    std::copy(zone.begin(),zone.end(),ff.begin()+248);
    for (size_t pos=0; pos<body.size();) {
        int count = static_cast<int>(std::min<size_t>(0x10000,body.size()-pos));
        Bytes block(LZ4_compressBound(count));
        int size = stored ? count : LZ4_compress_default(reinterpret_cast<const char*>(body.data()+pos),
            reinterpret_cast<char*>(block.data()),count,static_cast<int>(block.size()));
        if (stored) std::copy_n(body.begin()+pos,count,block.begin());
        if (!size) throw std::runtime_error("LZ4 compression failed");
        size_t aligned = (size+3u)&~3u;
        size_t offset=ff.size();
        append(ff,size,4); append(ff,count,4); append(ff,aligned,4); append(ff,offset,4);
        block.resize(aligned,0);
        ff.insert(ff.end(),block.begin(),block.end()); pos+=count;
    }
    size_t endOffset=ff.size(); ff.resize(endOffset+16+64);
    put(ff,endOffset+12,endOffset,4);
    if (fs::exists(output)) throw std::runtime_error("Output already exists");
    std::ofstream out(output,std::ios::binary);
    out.write(reinterpret_cast<const char*>(ff.data()),ff.size()); out.close();
    if (!out) throw std::runtime_error("Cannot write output");
}
void writeRawfile(const std::string& zone,const std::string& asset,const Bytes& source,const fs::path& output,bool stored) {
    Bytes body(48); // XAssetList, one null script-string entry and one asset
    put(body,0,1,4); put(body,8,nextPointer,8);
    put(body,32,1,4); put(body,40,nextPointer,8);
    append(body,0,8);
    append(body,46,8); // Aug 21 asset-name table, rawfile
    append(body,nextPointer,8);
    append(body,nextPointer,8); append(body,source.size(),8); append(body,nextPointer,8);
    body.insert(body.end(),asset.begin(),asset.end()); body.push_back(0);
    body.insert(body.end(),source.begin(),source.end()); body.push_back(0);

    writeZone(zone,body,24,72+asset.size()+1+16+source.size()+1,output,stored);
}
void build(const std::string& zone, const std::string& asset, const fs::path& input, const fs::path& output) {
    if (!validName(zone,false) || zone.size()>63 || !validName(asset,true) || asset.size()>255)
        throw std::runtime_error("Invalid zone name or asset path");
    if (fs::file_size(input)>512*1024) throw std::runtime_error("Prototype supports rawfiles up to 512 KiB");
    std::ifstream in(input, std::ios::binary);
    if (!in) throw std::runtime_error("Cannot open input");
    Bytes source((std::istreambuf_iterator<char>(in)), {});
    if (in.bad() || source.size()!=fs::file_size(input)) throw std::runtime_error("Input read failed");

    writeRawfile(zone, asset, source, output, false);
}

void buildWeapon(const std::string& base,const std::string& name,const std::string& clip,const fs::path& out) {
    if (!validName(base,false) || !validName(name,false) || name==base || base.size()>63 || name.size()>63)
        throw std::runtime_error("Use distinct weapon IDs of 1 to 63 lowercase letters, digits or underscores");
    if (clip.empty() || clip.size()>3 || clip.find_first_not_of("0123456789")!=clip.npos || std::stoi(clip)<1)
        throw std::runtime_error("Magazine size must be 1 to 999");
    // T7MP stock-derived weapon. The DLL resolves the base weapon on the August 21
    // registration path. Only identities and the override are stored on disk.
    constexpr size_t completeSize=0x300, record=72;
    Bytes body(record+completeSize);
    put(body,0,1,4); put(body,8,nextPointer,8);
    put(body,32,1,4); put(body,40,nextPointer,8);
    put(body,56,23,4); put(body,64,nextPointer,8);
    put(body,record,nextPointer,8); put(body,record+8,nextPointer,8);
    put(body,record+0x194,std::stoi(clip),4);
    body.insert(body.end(),name.begin(),name.end()); body.push_back(0);
    body.insert(body.end(),base.begin(),base.end()); body.push_back(0);
    writeZone("t7mp_weapon",body,completeSize,72+name.size()+base.size()+2,out,true);
}
int main(int argc,char** argv) {
    try {
        if (argc==3 && std::string(argv[1])=="inspect") inspect(argv[2]);
        else if (argc==6 && std::string(argv[1])=="build-weapon")
            buildWeapon(argv[2],argv[3],argv[4],argv[5]);
        else if (argc==6 && std::string(argv[1])=="build-rawfile")
            build(argv[2],argv[3],argv[4],argv[5]);
        else { std::cerr<<"t7-zonetool build-weapon BASE NEW_ID MAG_SIZE OUTPUT.ff\nt7-zonetool inspect FILE.ff\nt7-zonetool build-rawfile ZONE ASSET_PATH INPUT OUTPUT.ff\n"; return 2; }
    } catch (const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
