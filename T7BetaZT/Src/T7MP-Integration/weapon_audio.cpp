#include "weapon_audio.hpp"
#include "dvar.h"
#include <Windows.h>
#include <mmsystem.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <vector>
#include <memory>
#include <map>
#include <algorithm>
#include <cstring>
#include <stdexcept>
namespace WeaponAudio {
namespace {
struct Clip {WAVEFORMATEX format{};std::vector<char> pcm;};
struct Voice {HWAVEOUT output{};WAVEHDR header{};std::shared_ptr<Clip> clip;};
struct Cue {ULONGLONG time;std::string sound;};
std::map<std::string,std::shared_ptr<Clip>> clips;
std::vector<std::unique_ptr<Voice>> voices;
std::vector<Cue> cues;
std::string previous, previousShot;
bool loaded{};
std::shared_ptr<Clip> load(const std::filesystem::path& path) {
 std::ifstream input(path,std::ios::binary);
 std::vector<char> data((std::istreambuf_iterator<char>(input)),{});
 if(data.size()<44 || data.size()>2*1024*1024 || std::memcmp(data.data(),"RIFF",4) || std::memcmp(data.data()+8,"WAVE",4))
     throw std::runtime_error("Invalid sound file");
 auto result=std::make_shared<Clip>();bool fmt=false,pcm=false;
 for(size_t p=12;p+8<=data.size();) {
  uint32_t n{};std::memcpy(&n,data.data()+p+4,4);
  if(n>data.size()-p-8)throw std::runtime_error("Invalid WAV chunk");
  if(!std::memcmp(data.data()+p,"fmt ",4) && n>=16) {
   std::memcpy(&result->format,data.data()+p+8,16);fmt=true;
  }
  if(!std::memcmp(data.data()+p,"data",4)) {result->pcm.assign(data.begin()+p+8,data.begin()+p+8+n);pcm=true;}
  p+=8+n+(n&1);
 }
 auto& f=result->format;
 if(!fmt || !pcm || result->pcm.empty() || f.wFormatTag!=WAVE_FORMAT_PCM || f.wBitsPerSample!=16 ||
    (f.nChannels!=1 && f.nChannels!=2) || !f.nSamplesPerSec ||
    f.nBlockAlign!=f.nChannels*2 || f.nAvgBytesPerSec!=f.nSamplesPerSec*f.nBlockAlign ||
    result->pcm.size()%f.nBlockAlign)throw std::runtime_error("Unsupported PCM sound");
 return result;
}
// A port supplies one first-person sound at a time. Reclaim its wave header
// before releasing the backing clip or opening the next voice.
void stopVoices() {
 for(auto i=voices.begin();i!=voices.end();) {
  auto& v=**i;
  if(waveOutReset(v.output)==MMSYSERR_NOERROR &&
     waveOutUnprepareHeader(v.output,&v.header,sizeof(WAVEHDR))==MMSYSERR_NOERROR) {
   waveOutClose(v.output);
   i=voices.erase(i);
  } else ++i;
 }
}
void play(const std::string& name) {
 auto found=clips.find(name);if(found==clips.end())return;
 stopVoices();
 if(!voices.empty())return;
 auto voice=std::make_unique<Voice>();voice->clip=found->second;
 if(waveOutOpen(&voice->output,WAVE_MAPPER,&voice->clip->format,0,0,CALLBACK_NULL)!=MMSYSERR_NOERROR)return;
 voice->header.lpData=voice->clip->pcm.data();voice->header.dwBufferLength=static_cast<DWORD>(voice->clip->pcm.size());
 if(waveOutPrepareHeader(voice->output,&voice->header,sizeof(WAVEHDR))!=MMSYSERR_NOERROR){waveOutClose(voice->output);return;}
 if(waveOutWrite(voice->output,&voice->header,sizeof(WAVEHDR))!=MMSYSERR_NOERROR){
  waveOutUnprepareHeader(voice->output,&voice->header,sizeof(WAVEHDR));waveOutClose(voice->output);return;
 }
 voices.push_back(std::move(voice));
}
}
void Pump(bool active) {
 if(!active) {
  cues.clear();previous.clear();previousShot.clear();
  stopVoices();return;
 }
 try {
  if(!loaded) {
   auto path=Mod::Root()/"assets/models/msmc/sounds";
   auto loadClip=[](const std::string& key,const std::filesystem::path& file) {
    try { clips[key]=load(file); }
    catch(const std::exception& e){Mod::QueueLine("[Weapon audio] "+file.filename().string()+": "+e.what());}
   };
   for(auto name:{"shot","mag_out","mag_in","bolt_back","bolt_forward"})
    loadClip(name,path/(std::string("wpn_t6_msmc_")+name+".wav"));
   auto famas=Mod::Root()/"assets/models/famas/sounds";
   for(auto name:{"fire","clipout","clipin","chamber"})
    loadClip(std::string("famas_")+name,famas/(std::string("famas_")+name+".wav"));
   loaded=true;
  }
  for(auto i=voices.begin();i!=voices.end();) {
   auto& v=**i;
   if(v.header.dwFlags&WHDR_DONE){waveOutUnprepareHeader(v.output,&v.header,sizeof(WAVEHDR));waveOutClose(v.output);i=voices.erase(i);}
   else ++i;
  }
  const char* rawShot=Dvar::ValueToString("t7mp_port_shot");
  std::string shot=rawShot?rawShot:"";
  if(shot!=previousShot) {
   previousShot=shot;
   const auto type=shot.substr(0,shot.find(':'));
   if(type=="fire")play("shot");
   else if(type=="famas_fire")play("famas_fire");
  }
  const char* value=Dvar::ValueToString("t7mp_msmc_audio");
  std::string message=value?value:"";
  auto now=GetTickCount64();
  if(!message.empty() && message!=previous) {
   previous=message;auto type=message.substr(0,message.find(':'));
   // Older scripts use this channel for shots. A matching serial on both
   // channels represents the same shot during a script refresh.
   if(message!=shot && type=="fire")play("shot");
   if(message!=shot && type=="famas_fire")play("famas_fire");
   if(type=="famas_reload" || type=="famas_reload_empty") {
    bool empty=type=="famas_reload_empty";
    cues.clear();
    cues.push_back({now+(empty?567u:600u),"famas_clipout"});
    cues.push_back({now+(empty?1900u:1867u),"famas_clipin"});
    if(empty)cues.push_back({now+2500,"famas_chamber"});
   }
   if(type=="stop")cues.clear();
   if(type=="reload" || type=="reload_empty") {
    cues.clear();cues.push_back({now+500,"mag_out"});cues.push_back({now+2100,"mag_in"});
    if(type=="reload_empty"){cues.push_back({now+2667,"bolt_back"});cues.push_back({now+2800,"bolt_forward"});}
   }
  }
  for(auto i=cues.begin();i!=cues.end();) {
   if(i->time<=now){play(i->sound);i=cues.erase(i);}else ++i;
  }
 }catch(const std::exception& e){Mod::QueueLine(std::string("[MSMC audio] ")+e.what());loaded=true;}
}
}
