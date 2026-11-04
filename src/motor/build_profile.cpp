#include "build_profile.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstring>
#include <vector>
#include <algorithm>
#include "build_profiles.generated.hpp"

namespace circle::motor::build {
namespace {
char g_checked[65]{};
// SHA-256 over the chunks `next` yields (it returns false when done or on failure via `failed`).
template<class Next> bool sha256Hex(Next next,char (&hex)[65]){
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};DWORD objectSize{},got{};
    bool ok=BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0;
    if(ok)ok=BCryptGetProperty(alg,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&objectSize),sizeof(objectSize),&got,0)>=0;
    std::vector<unsigned char> object(objectSize);
    if(ok)ok=BCryptCreateHash(alg,&hash,object.data(),objectSize,nullptr,0,0)>=0;
    const unsigned char* data{};ULONG size{};bool failed=false;
    while(ok&&next(data,size,failed))ok=BCryptHashData(hash,const_cast<PUCHAR>(data),size,0)>=0;
    ok=ok&&!failed;
    std::array<unsigned char,32> digest{};
    if(ok)ok=BCryptFinishHash(hash,digest.data(),DWORD(digest.size()),0)>=0;
    if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);
    if(!ok)return false;
    constexpr char alphabet[]="0123456789abcdef";
    for(unsigned i=0;i<32;++i){hex[i*2]=alphabet[digest[i]>>4];hex[i*2+1]=alphabet[digest[i]&15];}
    hex[64]=0;return true;
}
bool fileSha256(char (&hex)[65]){
    wchar_t path[32768]{};if(!GetModuleFileNameW(nullptr,path,32768))return false;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    std::vector<unsigned char> buffer(1024*1024);
    const bool ok=sha256Hex([&](const unsigned char*& data,ULONG& size,bool& failed){
        DWORD n{};if(!ReadFile(file,buffer.data(),DWORD(buffer.size()),&n,nullptr)){failed=true;return false;}
        data=buffer.data();size=n;return n!=0;},hex);
    CloseHandle(file);return ok;
}
// The mapped .text section, with each base-relocated pointer inside it zeroed so the value does not
// depend on the load address. Matches tools/gamepass/motor-image-identity.ps1.
bool codeImageSha256(const unsigned char* base,const IMAGE_NT_HEADERS64* pe,char (&hex)[65]){
    const IMAGE_SECTION_HEADER* text=nullptr;
    const auto sections=IMAGE_FIRST_SECTION(pe);
    for(unsigned i=0;i<pe->FileHeader.NumberOfSections;++i)if(!std::memcmp(sections[i].Name,".text\0\0\0",8))text=&sections[i];
    const auto imageSize=pe->OptionalHeader.SizeOfImage;
    if(!text||text->VirtualAddress>=imageSize||text->Misc.VirtualSize>imageSize-text->VirtualAddress)return false;
    std::vector<unsigned char> code(base+text->VirtualAddress,base+text->VirtualAddress+text->Misc.VirtualSize);
    const auto& dir=pe->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if(dir.VirtualAddress<imageSize&&dir.Size<=imageSize-dir.VirtualAddress){
        for(DWORD at=dir.VirtualAddress;at+sizeof(IMAGE_BASE_RELOCATION)<=dir.VirtualAddress+dir.Size;){
            const auto block=reinterpret_cast<const IMAGE_BASE_RELOCATION*>(base+at);
            if(block->SizeOfBlock<sizeof(IMAGE_BASE_RELOCATION)||block->SizeOfBlock>dir.VirtualAddress+dir.Size-at)break;
            const auto entries=reinterpret_cast<const WORD*>(block+1);
            for(DWORD i=0;i<(block->SizeOfBlock-sizeof(IMAGE_BASE_RELOCATION))/sizeof(WORD);++i){
                const unsigned type=entries[i]>>12,width=type==IMAGE_REL_BASED_DIR64?8:type==IMAGE_REL_BASED_HIGHLOW?4:0;
                const long long target=static_cast<long long>(block->VirtualAddress)+(entries[i]&0xfff)-text->VirtualAddress;
                for(unsigned k=0;k<width;++k)if(target+k>=0&&target+k<static_cast<long long>(code.size()))code[size_t(target+k)]=0;
            }
            at+=block->SizeOfBlock;
        }
    }
    bool done=false;
    return sha256Hex([&](const unsigned char*& data,ULONG& size,bool&){
        if(done)return false;done=true;data=code.data();size=ULONG(code.size());return true;},hex);
}
}
const Profile* identify(Identity kind,const char* hash,uint32_t stamp,uint32_t size){
    if(!hash)return nullptr;
    for(const auto& p:profiles)if(p.identity==kind&&p.timestamp==stamp&&p.imageSize==size&&!_stricmp(hash,p.sha256))return &p;
    return nullptr;
}
const Profile* current(){
    static const Profile* profile=[]()->const Profile*{
        const auto base=reinterpret_cast<const unsigned char*>(GetModuleHandleW(nullptr));
        const auto dos=reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if(!base||dos->e_magic!=IMAGE_DOS_SIGNATURE||dos->e_lfanew<=0||dos->e_lfanew>0x100000)return nullptr;
        const auto pe=reinterpret_cast<const IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
        if(pe->Signature!=IMAGE_NT_SIGNATURE||pe->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64)return nullptr;
        const auto stamp=pe->FileHeader.TimeDateStamp,size=pe->OptionalHeader.SizeOfImage;
        bool file=false,image=false;
        for(const auto& p:profiles)if(p.timestamp==stamp&&p.imageSize==size)(p.identity==Identity::File?file:image)=true;
        char hex[65]{};
        if(file&&fileSha256(hex)){std::memcpy(g_checked,hex,sizeof(hex));if(auto p=identify(Identity::File,hex,stamp,size))return p;}
        // Hashed only from the image the loader mapped: the store file itself cannot be read.
        if(image&&codeImageSha256(base,pe,hex)){std::memcpy(g_checked,hex,sizeof(hex));if(auto p=identify(Identity::CodeImage,hex,stamp,size))return p;}
        return nullptr;
    }();
    return profile;
}
bool supported(){return current()!=nullptr;}
const char* checkedHash(){current();return g_checked;}
const Address* entry(uintptr_t canonical){
    const auto i=std::lower_bound(std::begin(addresses),std::end(addresses),canonical,[](const Address& a,uintptr_t r){return a.reference<r;});
    return i!=std::end(addresses)&&i->reference==canonical?i:nullptr;
}
static uintptr_t actualOf(Id id,const Address& a){return id==Id::Gog170?a.reference:id==Id::Steam170?a.steam:id==Id::GamePass170?a.gamepass:0;}
static const unsigned char* bytesOf(Id id,const Address& a){return id==Id::Gog170?a.gogBytes:id==Id::Steam170?a.steamBytes:a.gamepassBytes;}
uintptr_t resolve(Id id,uintptr_t canonical){
    const auto a=entry(canonical);return a?actualOf(id,*a):0;
}
uintptr_t rva(uintptr_t canonical){const auto p=current();return p?resolve(p->id,canonical):0;}
uintptr_t canonical(uintptr_t actual){
    const auto p=current();if(!p||!actual)return 0;
    for(const auto& a:addresses)if(actualOf(p->id,a)==actual)return a.reference;
    // Joint selectors report the return address immediately after a checked call.
    for(const auto& a:addresses)if(a.code&&a.gogBytes[0]==0xe8&&actualOf(p->id,a)&&actualOf(p->id,a)+5==actual)return a.reference+5;
    return 0;
}
bool matches(uintptr_t canonical,const void* referenceBytes,size_t count){
    const auto p=current();const auto a=entry(canonical);
    if(!p||!a||!a->code||!referenceBytes||count>32||std::memcmp(a->gogBytes,referenceBytes,count))return false;
    const auto offset=actualOf(p->id,*a);if(!offset)return false;
    std::array<unsigned char,32> actual{};SIZE_T got{};
    const auto at=reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr))+offset;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(at),actual.data(),count,&got)&&got==count&&
        !std::memcmp(actual.data(),bytesOf(p->id,*a),count);
}
}
