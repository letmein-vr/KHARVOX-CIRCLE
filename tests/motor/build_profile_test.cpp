#include "build_profiles.generated.hpp"
#include "laser_hooks.hpp"
#include <cstdio>
#include <cstring>
using namespace circle::motor::build;
int main(){
    unsigned errors{};
    auto check=[&](bool ok,const char* message){if(!ok){std::fprintf(stderr,"FAIL: %s\n",message);++errors;}};
    for(const auto& p:profiles){
        check(identify(p.identity,p.sha256,p.timestamp,p.imageSize)==&p||
            (identify(p.identity,p.sha256,p.timestamp,p.imageSize)&&identify(p.identity,p.sha256,p.timestamp,p.imageSize)->id==p.id),"exact build identified");
        check(!identify(p.identity,p.sha256,p.timestamp+1,p.imageSize),"PE timestamp mismatch rejected");
        check(!identify(p.identity,p.sha256,p.timestamp,p.imageSize+1),"PE size mismatch rejected");
        char changed[65]{};std::memcpy(changed,p.sha256,64);changed[0]=changed[0]=='0'?'1':'0';
        check(!identify(p.identity,changed,p.timestamp,p.imageSize),"same version/PE with different content rejected");
        check(!identify(p.identity==Identity::File?Identity::CodeImage:Identity::File,p.sha256,p.timestamp,p.imageSize),"file and code-image identities never stand in for each other");
    }
    for(const auto& a:addresses){
        check(resolve(Id::Gog170,a.reference)==a.reference,"GOG address preserved");
        check(resolve(Id::Steam170,a.reference)==a.steam,"Steam uses its explicit address");
        check(resolve(Id::GamePass170,a.reference)==a.gamepass,"Game Pass uses its explicit address");
        if(a.code)check(a.gamepass!=0,"every hook site has a Game Pass address");
        check(resolve(Id::Unknown,a.reference)==0,"unknown store/build cannot inherit a known address");
    }
    check(resolve(Id::Steam170,0xdeadbeef)==0,"unregistered point rejected");
    const Address* cast=nullptr;for(const auto& a:addresses)if(a.reference==0xa490c0)cast=&a;
    check(cast&&cast->code&&cast->steam==0xa49410,"laser collision function registered for both stores");
    check(cast&&cast->gamepass==0xa455f0,"laser collision function registered for Game Pass");
    if(cast){
        using namespace circle::motor::laser;
        check(!std::memcmp(cast->gogBytes,castPrologue,sizeof(castPrologue)),"laser cast signature matches canonical GOG prologue including REX prefix");
        check(!std::memcmp(cast->steamBytes,castPrologue,sizeof(castPrologue)),"laser cast signature matches Steam prologue");
        check(!std::memcmp(cast->gamepassBytes,castPrologue,sizeof(castPrologue)),"laser cast signature matches Game Pass prologue");
    }
    check(!supported(),"test executable cannot enable game writes");
    return errors?1:0;
}
