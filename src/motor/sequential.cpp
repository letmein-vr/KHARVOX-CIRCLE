#include "motor/diagnostics.hpp"
#include "motor/symmetric_optics.hpp"
#include "motor/runtime_optics_crop.hpp"
#include "vkcore/xr_runtime.hpp"
#include "motor/packed_projection.hpp"
#include "motor/build_profile.hpp"
#include "motor/comfort.hpp"
#include "vkcore/ui_engine.hpp"
#include "motor/sequential.hpp"
#include "motor/mono_vr.hpp"
#include "motor/experimental.hpp"
#include "motor/adapter.hpp"
#include "motor/native_state.hpp"
#include "motor/previous_view.hpp"
#include "motor/hud_projection.hpp"
#include "motor/temporal.hpp"
#include "motor/transparent_sort.hpp"
#include "motor/refraction.hpp"
#include "motor/object_ring.hpp"
#include "motor/joint_pair.hpp"
#include "motor/npc_skin_pair.hpp"
#include "motor/npc_model_policy.hpp"
#include "motor/particle_pair.hpp"
#include "motor/secondary_animation.hpp"
#include "motor/secondary_cache.hpp"
#include "motor/model_pair.hpp"
#include "motor/world_model_pair.hpp"
#include "motor/hand_hair.hpp"
#include "motor/hands_only.hpp"
#include "motor/hand_runtime.hpp"
#include "motor/firearm_policy.hpp"
#include "motor/hair_projection.hpp"
#include "motor/hair_pair.hpp"
#include "motor/effect_history.hpp"
#include "motor/scatter.hpp"
#include "motor/cinema_policy.hpp"
#include "motor/interaction_policy.hpp"
#include "motor/interaction_runtime.hpp"
#include "motor/cinema_timing.hpp"
#include "motor/world_gui_policy.hpp"
#include <intrin.h>
#include "motor/locomotion_probe.hpp"
#include "motor/locomotion_runtime.hpp"
#include <safetyhook.hpp>
#include "vkcore/log.hpp"
#include "vkcore/seq_hooks.hpp"
#include "vkcore/seq_locate.hpp"
#include "xr_math/stereo_view.hpp"
#include "stereo_seq/bin_tiles.hpp"
#include <array>
#include <span>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <cmath>
#include <share.h>
#include <unordered_map>

namespace circle::motor::sequential {
namespace {
std::uintptr_t base=0;
std::atomic<unsigned> events[6]{};
std::atomic<bool> adapterInstalled{false},based{false};
std::atomic<std::uint64_t> simulationEntries{0};
std::atomic<std::uintptr_t> activeInfo{0};
std::atomic<bool> probeArmed{false};
std::mutex proofMutex;
struct Proof {std::uintptr_t info=0,source=0;std::uint64_t simulation=0,token=0,cameraCopies=0;std::array<unsigned char,48> camera{};bool started=false,stereo=false;} proof;
bool read(std::uintptr_t p,void* dst,std::size_t n) {
    SIZE_T done=0;return p && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<void*>(p),dst,n,&done) && done==n;
}
template<class T> T field(std::uintptr_t p) {T v{};read(p,&v,sizeof(v));return v;}
std::atomic<std::uintptr_t> menuObject{0};
// Unknown stays closed. Only the fingerprinted native update/destructor changes this state.
std::atomic<int> menuState{-1}; // -1 unknown, 0 inactive/destroyed, 1 active
std::atomic<bool> stopped{false},inGameplay{false};
std::atomic<bool> inCinema{false};
bool stereoCinemaEnabled(){
    static const bool enabled=[](){
        wchar_t value[8]{};
        const bool on=GetEnvironmentVariableW(L"CIRCLE_STEREO_CINEMATICS",value,8)==1 && value[0]==L'1';
        circlevr::vkcore::logf("motor-cinema-policy: %s; item inspection always 2D",on?"experimental stereo":"2D shared image");
        return on;
    }();
    return enabled;
}
std::atomic<std::uintptr_t> projectedNativeView{0};
TransparentSortScope transparentSortScope; // protected by proofMutex
std::mutex controlMutex;
struct Fps {std::uintptr_t component=0,view=0;ULONGLONG ticks=0;std::array<float,3> origin{};float blend{};bool active=false,allowed=false;} fps;
std::array<std::array<unsigned char,0x9a0>,2> privateEyes;
std::array<experimental::LiveEye,2> currentEyes;
struct SavedPair {std::uint64_t token=0;std::array<experimental::LiveEye,2> eyes;};
std::array<SavedPair,32> savedPairs;unsigned savedIndex=0;
void scatterCurrentVolume(safetyhook::Context& r){
    // C42CCA stores the native current-volume blend weight. C42CB2 chooses
    // r_lightScatteringTAABlendRatio when TAA is on; C42CC6 chooses 1 when off.
    // Alter only this primary stereo pass's operand, never the saved cvar.
    const auto context=field<uintptr_t>(r.rsi+0x88);
    const auto view=field<uintptr_t>(r.rsi+0x20);
    if(!context||!view||!r.rdi||field<uintptr_t>(context+8)!=r.rdi)return;
    const auto counter=field<unsigned>(context+0x10);
    const auto tag=circlevr::vkcore::seqTagForBackendFrame(counter+1u);
    const auto eye=scatterStereoEye(adapterInstalled,stopped,
        field<int>(view+0x18f34)==0,counter,tag);
    if(!eye)return;
    // Preserve Motor's native temporal weight only after this exact pass has
    // been assigned a complete, sized per-eye volume pair and filter state.
    if(scatter::filtered(r.rdi,counter))return;
    const float before=r.xmm0.f32[0];
    r.xmm0.f32[0]=1.f;
    static std::atomic<unsigned> counts[2]{};const auto n=++counts[*eye];
    if(n<=4||n%1800==0)circlevr::vkcore::logf(
        "motor-scatter-history: eye %u backend %u token %llu blend %.6f -> 1; current volume retained, opposite-eye history excluded; count %u",
        *eye,counter,tag->tick,before,n);
}
#include "motor/world_model_pair.inl"
#include "motor/temporal_models.inl"
#include "motor/npc_pair_audit.inl"
#include "motor/model_pair_audit.inl"
#include "motor/particle_pair.inl"
#include "motor/secondary_cache_native.inl"
void secondaryAnimationSurface(safetyhook::Context& r){
    const bool owned=adapterInstalled&&!stopped&&inGameplay;
    const auto before=r.rflags;
    r.rflags=secondaryAnimationFlags(before,owned,diagnostics::nativeSecondaryAnimation());
    if(before!=r.rflags){
        static std::atomic<unsigned> changed{};const auto n=++changed;
        if(n<=4||n%60000==0)circlevr::vkcore::logf("motor-secondary-animation: secondary surface deformation suppressed in paired VR; native skeleton retained; count %u",n);
    }
}
using PickJoints=void(*)(void*,int*,int*);
PickJoints nativePickJoints=nullptr;
std::mutex jointPairMutex;
std::array<JointPairRecord,512> jointPairs{};
unsigned jointPairNext=0;
void pairedJoints(void* source,int* current,int* previous){
    const auto p=reinterpret_cast<uintptr_t>(source);
    const auto caller=build::canonical(reinterpret_cast<uintptr_t>(_ReturnAddress())-base);
    const bool scoped=jointCommitCaller(caller)&&adapterInstalled&&!stopped&&inGameplay;
    if(!scoped){nativePickJoints(source,current,previous);return;}
    const auto token=field<uint64_t>(base+build::rva(0x4c523a8));
    const auto frame=field<unsigned>(base+build::rva(0x4a71950));
    const auto eye=circlevr::vkcore::seqChainEye();
    if(eye!=circlevr::stereo_seq::Eye::Left&&eye!=circlevr::stereo_seq::Eye::Right){nativePickJoints(source,current,previous);return;}
    // Serialize selection and publication: two consumers of the same source
    // must not both advance its native allocation/history ring.
    std::lock_guard lock(jointPairMutex);
    const auto slot=field<int>(p+0x310);
    std::array<unsigned char,0x70> descriptor{};
    if(slot<0||slot>=4||!token||!read(p+slot*0x70,descriptor.data(),descriptor.size())){nativePickJoints(source,current,previous);return;}
    const bool right=eye==circlevr::stereo_seq::Eye::Right;
    const auto trace=[&](bool reused){
        gpu_audit::joints(token,frame,right?1:0,p,*current,*previous,descriptor.data(),unsigned(descriptor.size()),reused,slot,caller);
        if(!diagnostics::npcBufferEvent())return;
        unsigned hash=2166136261u;for(auto v:descriptor)hash=(hash^v)*16777619u;
        circlevr::vkcore::logf("motor-buffer-joints: source %p token %llu frame %u eye %u caller %llx slot %d descriptorHash %08x current %d previous %d reused %d",
            source,token,frame,right?1:0,caller,slot,hash,*current,*previous,reused);
    };
    {
        for(const auto& saved:jointPairs)if(saved.sameRender(p,token,frame,slot,descriptor)||(right&&saved.matches(p,token,frame,slot,descriptor))){
            *current=saved.current;*previous=saved.previous;
            trace(true);
            if(saved.render==frame){
                static std::atomic<unsigned> duplicates{};const auto count=++duplicates;
                if(count<=12||count%600==0)circlevr::vkcore::logf("motor-joint-repeat: eye %u source %p token %llu render %u current %d previous %d count %u caller %llx",right?1:0,source,token,frame,*current,*previous,count,caller);
                return;
            }
            static std::atomic<unsigned> reused{};const auto n=++reused;
            if(n<=16||n%1200==0)circlevr::vkcore::logf("motor-joint-pair: reuse right source %p token %llu render %u source slot %d draw %d previous %d count %u caller %llx",source,token,frame,slot,*current,*previous,n,caller);
            return;
        }
    }
    if(right&&diagnostics::enabled()&&frame%120<=3){
        const JointPairRecord* prior=nullptr;
        for(const auto& saved:jointPairs)if(saved.source==p){prior=&saved;break;}
        circlevr::vkcore::logf("motor-joint-pair-miss: source %p token %llu frame %u slot %d caller %llx cached %d sameToken %d frameDelta %u oldSlot %d sameDescriptor %d",
            source,token,frame,slot,caller,prior!=nullptr,prior&&prior->token==token,
            prior?frame-prior->render:0,prior?prior->slot:-1,prior&&prior->descriptor==descriptor);
    }
    nativePickJoints(source,current,previous);
    if(read(p+slot*0x70,descriptor.data(),descriptor.size())){
        trace(false);
        // Only retain the latest result for a given native source object.
        auto* save=&jointPairs[jointPairNext++%jointPairs.size()];
        for(auto& item:jointPairs)if(item.source==p){save=&item;break;}
        *save={p,token,frame,slot,*current,*previous,descriptor};
    }
}
std::mutex objectRingMutex;
circle::motor::ObjectRing objectRing;
struct ObjectAuditContext {uintptr_t stack{},manager{};unsigned frame{},slot{};int eye{-1};uint64_t token{};};
thread_local std::array<ObjectAuditContext,5> objectAuditContexts;
void objectRingPick(safetyhook::Context& r,bool previous,unsigned site){
    if(!adapterInstalled)return;
    // Capture this job's original counter before remapping. The later upload
    // must not identify its eye by re-reading a global advanced by another job.
    const auto frame=static_cast<unsigned>(r.rcx);
    const auto tag=circlevr::vkcore::seqTagForBackendFrame(frame+1);
    auto& context=objectAuditContexts[site];
    context={r.rsp,r.rdi,frame,frame%3,tag?circlevr::stereo_seq::eyeIndex(tag->eye):-1,tag?tag->tick:0};
    std::lock_guard lock(objectRingMutex);
    if(!temporal::active()){
        objectRing={};
        if(diagnostics::npcBufferEvent())circlevr::vkcore::logf("motor-buffer-ring: site %u frame %u active 0",site,unsigned(r.rcx));
        return;
    }
    // ECX is the native counter already captured by this job. Never re-read
    // the global backend pointer to decide ownership while jobs are running.
    ObjectRing::Observation observation;
    const auto p=objectRing.picks(tag?tag->eye:circlevr::stereo_seq::Eye::Mono,tag?tag->tick:0,frame,&observation);
    if(diagnostics::npcBufferEvent())
        circlevr::vkcore::logf("motor-buffer-ring: site %u frame %u active 1 eye %d token %llu first %d ordered %d tagMismatch %d current %u previous %u",
            site,frame,tag?circlevr::stereo_seq::eyeIndex(tag->eye):-1,tag?tag->tick:0,observation.first,observation.inOrder,observation.tagMismatch,p.current%3,(p.previous+2)%3);
    r.rcx=previous?p.previous:p.current;
    context.slot=previous?(p.previous+2)%3:p.current%3;
    static unsigned counts[2]{};const unsigned eye=tag&&tag->eye==circlevr::stereo_seq::Eye::Right?1:0;
    if(!previous&&(++counts[eye]<=8||counts[eye]%480==0))circlevr::vkcore::logf("motor-object-ring: eye %u frame %u token %llu current slot %u previous slot %u remapped %d",eye,frame,tag?tag->tick:0,p.current%3,(p.previous+2)%3,p.remapped);
}
void objectCurrent(safetyhook::Context& r){objectRingPick(r,false,0);}
void objectCurrentMatrices(safetyhook::Context& r){objectRingPick(r,false,1);}
void objectPrevious(safetyhook::Context& r){objectRingPick(r,true,2);}
void objectPreviousMatrices(safetyhook::Context& r){objectRingPick(r,true,3);}
void objectUploadPick(safetyhook::Context& r){objectRingPick(r,false,4);}
// Audit the native object ring without changing its ownership or rotation.
void objectUploadAudit(safetyhook::Context& r){
    if(!diagnostics::enabled()&&!diagnostics::npcBufferActive()&&!gpu_audit::recording())return;
    static std::atomic<unsigned> count{0};const auto n=++count;
    const auto context=objectAuditContexts[4];
    if(context.stack!=r.rsp||context.manager!=r.rdi)return;
    const auto frame=context.frame;
    const auto tag=circlevr::vkcore::seqTagForBackendFrame(frame+1);
    if(diagnostics::npcBufferEvent())circlevr::vkcore::logf("motor-buffer-resources: frame %u eye %d token %llu manager %p slot %u joints %p matrices %p nativeUpload %d jointCount %d",
        frame,context.eye,context.token,(void*)r.rdi,context.slot,(void*)r.r15,(void*)r.r13,
        field<uint64_t>(r.rdi+0x12db0)<r.rax&&int(r.rbx)>=0,int(r.rbx)+1);
    auditNpcUpload(r.rdi,frame,context.eye,context.token);
    playerModelUpload(r.rdi,frame,tag?circlevr::stereo_seq::eyeIndex(tag->eye):-1,tag?tag->tick:0);
    if(n>32&&n%240>3)return;
    std::array<unsigned char,256> sample{};const auto source=field<std::uintptr_t>(r.rdi+0xc0);
    unsigned hash=2166136261u;if(read(source,sample.data(),sample.size()))for(auto v:sample)hash=(hash^v)*16777619u;else hash=0;
    circlevr::vkcore::logf("motor-object-upload-audit: frame %u eye %d tick %llu manager %p offsets %p matrices %p source %p sample %08x last %llu current %llu count %u",frame,tag?circlevr::stereo_seq::eyeIndex(tag->eye):-1,tag?tag->tick:0,(void*)r.rdi,(void*)r.r15,(void*)r.r13,(void*)source,hash,field<uint64_t>(r.rdi+0x12db0),r.rax,n);
}
void objectBindAudit(safetyhook::Context& r){
    if(diagnostics::npcBufferActive()){
        const auto& c=objectAuditContexts[0];
        if(c.stack==r.rsp&&c.manager==r.rdi&&r.rax<=2&&diagnostics::npcBufferEvent())
            circlevr::vkcore::logf("motor-buffer-bind: frame %u eye %d token %llu manager %p slot %llu joints %p matrices %p",
                c.frame,c.eye,c.token,(void*)r.rdi,r.rax,
                (void*)field<uintptr_t>(r.rdi+0x45f18+r.rax*8),(void*)field<uintptr_t>(r.rdi+0x45f30+r.rax*8));
    }
    if(!diagnostics::enabled())return;
    static std::atomic<unsigned> count{0};const auto n=++count;
    if(n>32&&n%240>3)return;
    const auto backend=field<std::uintptr_t>(base+build::rva(0x4a729b8));
    const auto frame=backend?field<unsigned>(backend+0x1b8):field<unsigned>(base+build::rva(0x4a71950));
    const auto tag=circlevr::vkcore::seqTagForBackendFrame(frame+1);
    if(r.rax>2)return;
    circlevr::vkcore::logf("motor-object-bind-audit: frame %u eye %d tick %llu slot %llu manager %p offsets %p matrices %p count %u",frame,tag?circlevr::stereo_seq::eyeIndex(tag->eye):-1,tag?tag->tick:0,r.rax,(void*)r.rdi,(void*)field<uintptr_t>(r.rdi+0x45f18+r.rax*8),(void*)field<uintptr_t>(r.rdi+0x45f30+r.rax*8),n);
}
// Retain CPU GUI batches until the native builder has consumed them for both
// eyes. Rebuilding through Motor allocates valid per-render descriptors; replaying
// the old backend surface word alone is invalid (r06 regression).
// Both native sites have already tested bit 32 (per-frame GUI geometry).
// The first filters world surfaces, the second draws the surviving GUI. Screen
// GUI draws with no world bypass the native comparison and remain untouched.
void worldGuiStamp(safetyhook::Context& r,std::uintptr_t world,unsigned site){
    if(!adapterInstalled||stopped||!world)return;
    const auto tag=circlevr::vkcore::seqTagInFlight();
    if(!tag||!circlevr::stereo_seq::drawsEyeView(*tag))return;
    const auto current=field<std::uint64_t>(world+0x12db0);
    const auto committed=static_cast<std::int64_t>(r.rax);
    if(!reuseWorldGuiStamp(tag->eye==circlevr::stereo_seq::Eye::Right,committed,current))return;
    // Only the comparison register changes, not the GUI stamp/world counter.
    r.rax=current;
    static std::atomic<unsigned> counts[2]{};const auto n=++counts[site];
    if(n<=8||n%3600==0)circlevr::vkcore::logf("motor-world-gui: site %u right render %u tick %llu stamp %lld world %llu reused %u",site,tag->renderFrame,tag->tick,committed,current,n);
}
void worldGuiDraw(safetyhook::Context& r){worldGuiStamp(r,r.r8,0);}
void worldGuiVisibility(safetyhook::Context& r){worldGuiStamp(r,r.r14,1);}
// Slug glyph payload is consumed/reset at each native backend begin, independently
// of GUI geometry. Re-upload the same simulation payload into R's native buffer.
std::mutex glyphMutex;
struct GlyphPair {uint64_t token{};unsigned render{},bytes{};std::array<unsigned char,0x4000*48> data{};} glyphPair;
thread_local std::array<unsigned char,0x4000*48> glyphReplay;
bool glyphPairMatches(uint64_t token,unsigned render){return reuseGlyphPayload(glyphPair.token,token,glyphPair.render,render,glyphPair.bytes);}
void glyphCount(safetyhook::Context& r){
    if(!adapterInstalled||stopped||!inGameplay||circlevr::vkcore::seqChainEye()!=circlevr::stereo_seq::Eye::Right)return;
    const auto token=field<uint64_t>(base+build::rva(0x4c523a8));
    const auto render=field<unsigned>(base+build::rva(0x4a71950));
    std::lock_guard lock(glyphMutex);
    if(glyphPairMatches(token,render))r.rdx=glyphPair.bytes/48;
}
void glyphUpload(safetyhook::Context& r){
    if(!adapterInstalled||stopped||!inGameplay)return;
    const auto eye=circlevr::vkcore::seqChainEye();
    const auto token=field<uint64_t>(base+build::rva(0x4c523a8));
    const auto render=field<unsigned>(base+build::rva(0x4a71950));
    std::lock_guard lock(glyphMutex);
    if(eye==circlevr::stereo_seq::Eye::Left){
        glyphPair.bytes=0;
        if(r.r8&&r.r8<=glyphPair.data.size()&&r.r8%48==0&&read(r.rdx,glyphPair.data.data(),r.r8)){
            glyphPair.bytes=unsigned(r.r8);glyphPair.token=token;glyphPair.render=render;
        }
    }else if(eye==circlevr::stereo_seq::Eye::Right&&glyphPairMatches(token,render)){
        const auto nativeBytes=r.r8;
        std::memcpy(glyphReplay.data(),glyphPair.data.data(),glyphPair.bytes);
        r.rdx=reinterpret_cast<uintptr_t>(glyphReplay.data());r.r8=glyphPair.bytes;
        static unsigned count{};if(++count<=8||count%1200==0)circlevr::vkcore::logf("motor-glyph-pair: right render %u token %llu native bytes %llu paired bytes %u count %u",render,token,nativeBytes,glyphPair.bytes,count);
    }
}
struct GuiBatch {std::uintptr_t model=0,surfaces=0,splits=0;std::uint64_t token=0;unsigned render=0;int count=0,splitCount=0,scratch=0,generation=0;std::uintptr_t context=0;int buffer=-1;std::array<unsigned char,0x380> bindings{};bool bindingsValid=false;};
std::mutex guiMutex;
std::array<GuiBatch,128> guiBatches{};unsigned guiIndex=0;
std::atomic<uint64_t> binocularHudTick{};
// Read only the surfaces actually submitted by the native screen GUI. The
// existing 88DF63 hook runs before the native surface count is cleared.
// 88D970..88DA3A resolves pool[index*stride] -> idMaterial2; idResource.name
// is reflected at +8. No new native hooks or per-frame heap allocations.
bool hasBinocularHud(std::uintptr_t model,std::uintptr_t pool,unsigned stride){
    __try {
        if(!pool||stride<0x90||stride>4096||
           *reinterpret_cast<std::uintptr_t*>(model+0x108)||
           *reinterpret_cast<int*>(model+0x62c)>0)return false;
        const int count=*reinterpret_cast<int*>(model+0x4a0);
        const auto* list=*reinterpret_cast<const short**>(model+0x4a8);
        if(!list||count<=0||count>4096)return false;
        for(int i=0;i<count;++i){
            if(list[i]<0)continue;
            const auto surface=pool+static_cast<unsigned>(list[i])*stride;
            if(*reinterpret_cast<int*>(surface+0x14)<=0)continue;
            const auto material=*reinterpret_cast<std::uintptr_t*>(surface);
            if(!material)continue;
            const auto* name=*reinterpret_cast<const char**>(material+8);
            if(!name)continue;
            char text[160]{};unsigned n=0;
            for(;n<sizeof(text)-1&&name[n];++n)text[n]=name[n];
            if(n==sizeof(text)-1)continue;
            if(binocularHudMaterial(std::string_view(text,n)))return true;
        }
    } __except(GetExceptionCode()==EXCEPTION_ACCESS_VIOLATION?EXCEPTION_EXECUTE_HANDLER:EXCEPTION_CONTINUE_SEARCH) {return false;}
    return false;
}
void guiAudit(std::uintptr_t model,const char* phase){
    if(!diagnostics::enabled())return;
    static std::atomic<unsigned> counts[2]{};const unsigned side=phase[0]=='L'?0:1;const auto n=++counts[side];
    if(n>12&&n%1800)return;
    const auto context=field<std::uintptr_t>(field<std::uintptr_t>(base+build::rva(0x4a729b8))+0x450);
    const auto list=field<std::uintptr_t>(model+0x4a8),pool=field<std::uintptr_t>(base+build::rva(0x3f061b8));
    const auto stride=field<unsigned>(base+build::rva(0x3f061c0));
    const auto scratch=field<std::uintptr_t>(model+0x5e8);
    circlevr::vkcore::logf("motor-gui-audit: %s model %p render %u generation %d buffer %d poolStride %u surfaces %d scratch %p vertices %d indices %d",phase,reinterpret_cast<void*>(model),field<unsigned>(base+build::rva(0x4a71950)),field<int>(context+0x668),field<int>(context+0x5d0),stride,field<int>(model+0x4a0),reinterpret_cast<void*>(scratch),field<int>(scratch+8),field<int>(scratch+12));
    if(n>12)return;
    const auto dump=[&](const wchar_t* name,std::uintptr_t p,std::size_t size){
        std::array<unsigned char,4096> data{};if(size>data.size()||!read(p,data.data(),size))return;
        const auto path=circlevr::vkcore::logDirectory()+L"\\gui-"+std::to_wstring(side)+L"-"+std::to_wstring(n)+L"-"+name+L".bin";
        if(FILE* f=circlevr::vkcore::openLogFileShared(path.c_str(),L"wb",_SH_DENYWR)){fwrite(data.data(),1,size,f);fclose(f);}
    };
    dump(L"model",model,0x630);dump(L"context",context,0x680);
    const int count=field<int>(model+0x4a0);
    for(int i=0;i<count&&i<4&&stride>=0x90&&stride<=4096;++i){
        const auto index=field<short>(list+i*2);if(index<0)continue;
        const auto name=L"surface"+std::to_wstring(i);dump(name.c_str(),pool+index*stride,stride);
    }
}
thread_local std::uintptr_t guiReplayContext=0;
thread_local int guiReplayBuffer=-1;
thread_local std::array<unsigned char,0x380> guiReplayBindings{};
thread_local bool guiReplayBindingsValid=false;
struct HudFrame {unsigned render=0;int eye=-1;std::uint64_t token=0;std::array<float,4> viewport{};};
std::mutex hudMutex;
std::array<HudFrame,64> hudFrames{};
struct HudDrawState {std::uintptr_t context=0;std::array<int,8> rectangles{};};
thread_local HudDrawState hudDrawState;
void hudDrawEnd(safetyhook::Context&){
    if(!hudDrawState.context)return;
    std::memcpy(reinterpret_cast<void*>(hudDrawState.context+0x498),hudDrawState.rectangles.data(),sizeof(hudDrawState.rectangles));
    *reinterpret_cast<unsigned short*>(hudDrawState.context+0x2a8)=0x0101;
    hudDrawState={};
}
void guiBuffer(safetyhook::Context& r){
    // Only change the register used for this buffer binding. Never change the
    // engine's shared buffer rotation or replay backend descriptor words.
    if(guiReplayBuffer>=0&&r.r14==guiReplayContext)r.rax=static_cast<unsigned>(guiReplayBuffer);
}
void guiBinding(safetyhook::Context& r){
    if(!guiReplayBindingsValid||r.r14!=guiReplayContext||r.rdx<guiReplayContext)return;
    const auto offset=r.rdx-guiReplayContext;
    if(offset>=guiReplayBindings.size()||offset%0x70)return;
    const auto* saved=guiReplayBindings.data()+offset;
    static std::atomic<unsigned> changes{0};
    if(std::memcmp(saved,reinterpret_cast<void*>(r.rdx),0x70)!=0){
        const auto n=++changes;
        if(n<=20||n%1800==0)circlevr::vkcore::logf("motor-gui-binding: right source changed slot %llu baseOffset %u -> %u resource %p -> %p count %u",offset/0x70,field<unsigned>(r.rdx+8),field<unsigned>(reinterpret_cast<std::uintptr_t>(saved)+8),reinterpret_cast<void*>(field<std::uintptr_t>(r.rdx+0x18)),reinterpret_cast<void*>(field<std::uintptr_t>(reinterpret_cast<std::uintptr_t>(saved)+0x18)),n);
    }
    // B955B0 copies a resource slice (handle AND base offset), not just the
    // ring index. The backing allocation remains owned by the native frame.
    r.rdx=reinterpret_cast<std::uintptr_t>(saved);
}
void hudDraw(safetyhook::Context& r){
    hudDrawEnd(r);
    if(!adapterInstalled||stopped||!r.rbp)return;
    const auto tag=circlevr::vkcore::seqTagInFlight();
    if(!tag||!circlevr::stereo_seq::drawsEyeView(*tag))return;
    HudFrame frame;
    {std::lock_guard lock(hudMutex);frame=hudFrames[tag->renderFrame%hudFrames.size()];}
    if(frame.render!=tag->renderFrame||frame.token!=tag->tick||frame.eye!=circlevr::stereo_seq::eyeIndex(tag->eye))return;
    std::array<int,8> saved{};
    if(!read(r.rbx+0x498,saved.data(),sizeof(saved)))return;
    // DADFF0 uploads 4A8..4B4 as VkViewport and 498..4A4 as
    // VkRect2D. Transform both, then restore after this GUI draw only.
    const std::array<int,4> native{saved[4],saved[5],saved[6],saved[7]};
    if(native[2]<64||native[3]<64)return;
    const auto& v=frame.viewport;
    const std::array<int,4> rect{native[0]+int(std::lround(v[0]*native[2])),native[1]+int(std::lround(v[1]*native[3])),int(std::lround(v[2]*native[2])),int(std::lround(v[3]*native[3]))};
    if(rect[0]<0||rect[1]<0||rect[2]<32||rect[3]<32)return;
    const int sx=rect[0]+int(std::lround((saved[0]-native[0])*v[2]));
    const int sy=rect[1]+int(std::lround((saved[1]-native[1])*v[3]));
    const std::array<int,4> clip{sx,sy,int(std::lround(saved[2]*v[2])),int(std::lround(saved[3]*v[3]))};
    if(sx<0||sy<0||clip[2]<=0||clip[3]<=0)return;
    hudDrawState={r.rbx,saved};
    std::memcpy(reinterpret_cast<void*>(r.rbx+0x4a8),rect.data(),sizeof(rect));
    std::memcpy(reinterpret_cast<void*>(r.rbx+0x498),clip.data(),sizeof(clip));
    *reinterpret_cast<unsigned short*>(r.rbx+0x2a8)=0x0101;
    static std::atomic<unsigned> counts[2]{};const auto n=++counts[frame.eye];
    if(n<=4||n%3600==0)circlevr::vkcore::logf("motor-hud: eye %d render %u viewport %d %d %d %d -> %d %d %d %d count %u",frame.eye,frame.render,native[0],native[1],native[2],native[3],rect[0],rect[1],rect[2],rect[3],n);
}
void guiConsumed(safetyhook::Context& r){
    guiReplayContext=0;guiReplayBuffer=-1;guiReplayBindingsValid=false;
    if(!adapterInstalled||stopped||!inGameplay||circlevr::vkcore::seqChainEye()!=circlevr::stereo_seq::Eye::Left||field<std::uintptr_t>(r.r13)!=base+build::rva(0x3648ab0))return;
    guiAudit(r.r13,"L-consumed");
    if(hasBinocularHud(r.r13,field<std::uintptr_t>(base+build::rva(0x3f061b8)),field<unsigned>(base+build::rva(0x3f061c0)))){
        const auto now=GetTickCount64();const auto previous=binocularHudTick.exchange(now);
        if(!previous||now-previous>=250)circlevr::vkcore::logf("motor-binocular-hud: native screen material submitted; direct head-follow active");
    }
    const auto m=r.r13;
    GuiBatch v{m,field<std::uintptr_t>(m+0x4a8),field<std::uintptr_t>(m+0x4d0),field<std::uint64_t>(base+build::rva(0x4c523a8)),field<unsigned>(base+build::rva(0x4a71950)),field<int>(m+0x4a0),field<int>(m+0x4d8),field<int>(m+0x528),field<int>(m+0x5e0)};
    v.context=field<std::uintptr_t>(field<std::uintptr_t>(base+build::rva(0x4a729b8))+0x450);
    v.buffer=field<int>(v.context+0x5d0);
    v.bindingsValid=v.context&&read(v.context,v.bindings.data(),v.bindings.size());
    if(v.count<=0||v.count>4096||v.splitCount<=0||v.splitCount>4096)return;
    std::lock_guard lock(guiMutex);guiBatches[(guiIndex++)%guiBatches.size()]=v;
}
void guiBuild(safetyhook::Context& r){
    guiReplayContext=0;guiReplayBuffer=-1;guiReplayBindingsValid=false;
    if(!adapterInstalled||stopped||!inGameplay||circlevr::vkcore::seqChainEye()!=circlevr::stereo_seq::Eye::Right||field<std::uintptr_t>(r.rcx)!=base+build::rva(0x3648ab0))return;
    // Only screen GUI is supplied by the common BOTH-eye HUD layer. A GUI
    // rendered into an object texture (e.g. map markings) still needs its native
    // right-eye build, even though its render-model world pointer can be null.
    if(!replayGuiBatch(circlevr::vkcore::ui_engine::skipComposite(),field<std::uintptr_t>(r.rcx+0x108)!=0,field<int>(r.rcx+0x62c)))return;
    const auto m=r.rcx;const auto render=field<unsigned>(base+build::rva(0x4a71950));const auto token=field<std::uint64_t>(base+build::rva(0x4c523a8));
    std::lock_guard lock(guiMutex);
    for(unsigned age=0;age<guiBatches.size()&&age<guiIndex;++age){
        auto& v=guiBatches[(guiIndex-1-age)%guiBatches.size()];
        if(v.model!=m||v.token!=token||v.render+1!=render)continue;
        if(field<int>(m+0x4a0)!=0||field<int>(m+0x4d8)!=0||field<std::uintptr_t>(m+0x4a8)!=v.surfaces||field<std::uintptr_t>(m+0x4d0)!=v.splits||field<int>(m+0x5e0)!=v.generation||field<int>(m+0x4b0)<v.count||field<int>(m+0x4dc)<v.splitCount)return;
        std::memcpy(reinterpret_cast<void*>(m+0x4a0),&v.count,4);
        std::memcpy(reinterpret_cast<void*>(m+0x4d8),&v.splitCount,4);
        std::memcpy(reinterpret_cast<void*>(m+0x528),&v.scratch,4);
        // The saved splits already include the last open range. Do not append
        // that range again when rebuilding the right eye.
        const int closed=-1;std::memcpy(reinterpret_cast<void*>(m+0x52c),&closed,4);
        if(v.context&&v.buffer>=0&&v.buffer<3&&v.bindingsValid){guiReplayContext=v.context;guiReplayBuffer=v.buffer;guiReplayBindings=v.bindings;guiReplayBindingsValid=true;}
        guiAudit(m,"R-rebuild");
        v.model=0;
        static unsigned builds=0;if(++builds<=16||builds%600==0)circlevr::vkcore::logf("motor-stereo-gui: native right rebuild render %u batches %d splits %d generation %d count %u renderTexture %d commonHud %d",render,v.count,v.splitCount,v.generation,builds,field<int>(m+0x62c),circlevr::vkcore::ui_engine::skipComposite());
        return;
    }
}
struct Copy {alignas(16) std::array<unsigned char,0x9a0> bytes;std::array<unsigned char,48> originalCamera;std::array<float,4> fov{};std::uintptr_t original=0,dest=0;int kind=0,eye=0;unsigned builders=0;bool active=false,committed=false,latching=false;};
thread_local Copy privateCopy;
std::mutex previousMutex;
circlevr::stereo_seq::PrevMatrixBook previousViews(previousViewRanges());
bool immersive(){static const bool on=[] {wchar_t v[16]{};return GetEnvironmentVariableW(L"CIRCLE_VR_PROJECTION",v,16)==0 || wcscmp(v,L"phase5")!=0;}();return on;}
void menu(safetyhook::Context& r){
    if(field<std::uintptr_t>(r.rbx)!=base+build::rva(0x37dd838))return;
    menuObject=r.rbx;
    menuState=(field<unsigned char>(r.rbx+0xa5) || field<unsigned char>(r.rbx+0xa7))?1:0;
}
void menuDestroyed(safetyhook::Context& r){
    if(r.rcx!=menuObject || field<std::uintptr_t>(r.rcx)!=base+build::rva(0x37dd838))return;
    menuObject=0;menuState=0;
    circlevr::vkcore::logf("motor-seq-gate: native main-menu destructor observed for %p",reinterpret_cast<void*>(r.rcx));
}
void fpsComplete(safetyhook::Context& r){
    Fps next;next.component=r.rbx;next.view=r.rdi;next.ticks=GetTickCount64();
    next.active=field<unsigned char>(r.rbx+0x5c)==1&&field<unsigned char>(r.rbx+0x5d)==1;next.blend=field<float>(r.rbx+0x10);
    next.allowed=field<unsigned char>(r.rbx+0x5c)==1 && field<unsigned char>(r.rbx+0x5d)==1 && !field<unsigned char>(r.rbx+0x5e) && field<float>(r.rbx+0x10)>=0.999f;
    if(!read(r.rdi+0x70,next.origin.data(),12))next.allowed=false;
    std::lock_guard lock(controlMutex);fps=next;
}
bool gameplay(std::uintptr_t source){
    Fps f;{std::lock_guard lock(controlMutex);f=fps;}
    const auto m=menuObject.load();std::array<float,3> origin{};
    const bool sourceValid=read(source+0xa8,origin.data(),12);
    const bool nativeReady=sourceValid&&f.allowed&&locomotion::traversalCameraMatches(
        {origin[0],origin[1],origin[2]},{f.origin[0],f.origin[1],f.origin[2]});
    std::array<float,12> ledge{};const bool climbing=ledgeCamera(ledge,nativeReady,true);
    bool yes=!stopped && menuState==0 && (!m || (field<std::uintptr_t>(m)==base+build::rva(0x37dd838) && !field<unsigned char>(m+0xa5) && !field<unsigned char>(m+0xa7))) &&
        field<int>(base+build::rva(0x4c6c514))==2 && !locomotion::flatInteractionActive() && (f.allowed||climbing) && GetTickCount64()-f.ticks<150 && sourceValid;
    for(unsigned i=0;i<3;++i)yes=yes && std::isfinite(origin[i]) && (climbing||std::fabs(origin[i]-f.origin[i])<0.05f);
    static ULONGLONG stableSince=0,lastLog=0;const auto now=GetTickCount64();if(!yes)stableSince=0;else if(!stableSince)stableSince=now;
    if(now-lastLog>1000){lastLog=now;circlevr::vkcore::logf("motor-seq-gate: menu %p state %d FPS %p allowed %d age %llu source %.4f %.4f %.4f fps %.4f %.4f %.4f candidate %d",reinterpret_cast<void*>(m),menuState.load(),reinterpret_cast<void*>(f.component),f.allowed?1:0,now-f.ticks,origin[0],origin[1],origin[2],f.origin[0],f.origin[1],f.origin[2],yes?1:0);}
    return yes && now-stableSince>=200;
}
void redirect(safetyhook::Context& r,int kind,int eye){
    auto& c=privateCopy;c={};c.kind=kind;c.eye=eye;c.original=kind==3?r.r15:r.rbx;c.dest=r.rcx;
    // Auxiliary and primary views share a camera but have different native flags/pointers.
    // Preserve this call's complete source; only replace the validated camera fields.
    if(!read(c.original+0x30,c.bytes.data(),c.bytes.size()))return;
    std::memcpy(c.originalCamera.data(),c.bytes.data()+0xa8,48);
    if(std::memcmp(c.originalCamera.data(),proof.camera.data(),48))return;
    std::memcpy(c.bytes.data()+0xa8,privateEyes[eye].data()+0xa8,48);
    if(immersive()) {
        const auto f=currentEyes[eye].cinema?currentEyes[eye].cinemaFov:
            interaction::photoRenderFov(currentEyes[eye].rig.fovs[eye],currentEyes[eye].photoZoomScale);
        circlevr::xr_math::EngineMatrix depth{};depth[10]=-1;depth[11]=-0.06f;depth[14]=-1;
        const auto projection=circlevr::xr_math::engineProjection({f[0],f[1],f[2],f[3]},depth);
        if(!projection)return;
        // The explicit path collapses Motor's forward/reverse depth projections and
        // even copies P into some inverse slots. Keep its native builders instead;
        // scoped hooks below provide the exact optical planes to each builder.
        c.fov=f;c.bytes[0x8c]=0;
        const float fovX=2*std::fmax(std::fabs(f[0]),std::fabs(f[1]))*57.2957795f,
                    fovY=2*std::fmax(std::fabs(f[2]),std::fabs(f[3]))*57.2957795f;
        std::memcpy(c.bytes.data()+0x64,&fovX,4);std::memcpy(c.bytes.data()+0x68,&fovY,4);
        currentEyes[eye].immersive=true;
    }
    temporal::prepareEye(c.bytes.data(),eye,proof.token);
    c.active=true;const auto replacement=reinterpret_cast<std::uintptr_t>(c.bytes.data())-0x30;
    if(kind==3)r.r15=replacement;else r.rbx=replacement;
}
void copied(safetyhook::Context& r){
    auto& c=privateCopy;if(!c.active)return;
    std::array<unsigned char,48> original{},dest{};
    const bool valid=read(c.original+0x30+0xa8,original.data(),48) && original==c.originalCamera && read(c.dest+0xa8,dest.data(),48) && !std::memcmp(dest.data(),c.bytes.data()+0xa8,48);
    if(c.kind==3)r.r15=c.original;else r.rbx=c.original;
    r.rdx=c.original+0x30;c.active=false;
    if(!valid){stopped=true;inGameplay=false;circlevr::vkcore::logf("motor-seq: private camera copy mismatch; cinema latched");return;}
    c.committed=true;
    if(c.kind==3 && !immersive()){std::lock_guard lock(proofMutex);currentEyes[c.eye].sequence=field<unsigned>(base+build::rva(0x4a71950));circlevr::vkcore::seqMarkEyeView(c.eye?circlevr::stereo_seq::Eye::Right:circlevr::stereo_seq::Eye::Left,proof.token);}
}
void latchBegin(safetyhook::Context& r){
    auto& c=privateCopy;
    c.latching=adapterInstalled && !stopped && immersive() && c.committed && r.rcx==c.dest;
    c.builders=0;
}
void latchEnd(safetyhook::Context&){privateCopy.latching=false;}
void previousStored(safetyhook::Context& r){
    // C4A397 just called CC8820. RDI is the same idRenderView validated by
    // the auxiliary private-copy hook, before CC8C30 rebuilds current optics.
    const auto& c=privateCopy;
    std::lock_guard lock(previousMutex);
    int locked=1;
    read(field<std::uintptr_t>(base+build::rva(0x44ccf10))+8,&locked,sizeof(locked));
    if(!adapterInstalled || stopped || !inGameplay || !immersive() || !temporal::active() ||
       c.kind!=4 || !c.committed || c.dest!=r.rdi ||
       locked!=0){
        previousViews.clear();return;
    }
    const auto frame=field<unsigned>(base+build::rva(0x4a71950));
    const float before=field<float>(r.rdi+0x18818);
    const bool changed=previousViews.afterStore(reinterpret_cast<std::byte*>(r.rdi),
        c.eye?circlevr::stereo_seq::Eye::Right:circlevr::stereo_seq::Eye::Left,frame);
    const float after=field<float>(r.rdi+0x18818);
    const auto stats=previousViews.stats();
    if(stats.stores<=12 || stats.stores%1200==0)
        circlevr::vkcore::logf("motor-prev-view: eye %d frame %u view %p rewritten %d previous P02 %.7f -> %.7f stores %llu rewrites %llu",c.eye,frame,reinterpret_cast<void*>(r.rdi),changed?1:0,before,after,stats.stores,stats.rewrites);
    if(changed){
        const auto& f=c.fov;
        const float expected=(std::tan(f[1])+std::tan(f[0]))/(std::tan(f[1])-std::tan(f[0]));
        // Optical-centre evidence only: FOV may legitimately change between
        // frames. A mismatch is logged, not hidden by rewriting old optics.
        static unsigned snapshots[2]{};
        if(diagnostics::enabled()&&snapshots[c.eye]<2){
            std::array<unsigned char,0x7a0> bytes{};
            if(read(r.rdi+0x18790,bytes.data(),bytes.size())){
                const auto name=circlevr::vkcore::logDirectory()+L"\\motor-previous-eye"+std::to_wstring(c.eye)+L"-"+std::to_wstring(++snapshots[c.eye])+L".bin";
                FILE* file=nullptr;if(!circlevr::vkcore::openLogFile(&file,name.c_str(),L"wb") && file){fwrite(bytes.data(),1,bytes.size(),file);fclose(file);}
            }
            circlevr::vkcore::logf("motor-prev-view: eye %d restored P02 %.7f current XR P02 %.7f",c.eye,after,expected);
        }
    }
}
void opticalPlanes(safetyhook::Context& r){
    auto& c=privateCopy;if(!c.latching)return;
    // Both FOV builders have a verified 0x88-byte stack frame. Restrict this
    // adapter to CC8C30's eight direct calls, never unrelated/shadow projections.
    constexpr std::uintptr_t callers[]={0xcc8dd8,0xcc8fb3,0xcc901d,0xcc91e5,0xcc9389,0xcc9633,0xcc9b8f,0xcc9bd6};
    const auto caller=build::canonical(field<std::uintptr_t>(r.rsp+0x88)-base);
    unsigned index=0;while(index<8 && callers[index]!=caller)++index;
    if(index==8)return;
    const float nearPlane=field<float>(r.rsp+0x20);
    if(!std::isfinite(nearPlane) || nearPlane<=0){stopped=true;inGameplay=false;return;}
    r.xmm0.f32[0]=nearPlane*std::tan(c.fov[0]); // left
    r.xmm1.f32[0]=nearPlane*std::tan(c.fov[1]); // right
    r.xmm2.f32[0]=nearPlane*std::tan(c.fov[3]); // bottom
    r.xmm3.f32[0]=nearPlane*std::tan(c.fov[2]); // top
    c.builders|=1u<<index;
}
void inverseOpticalPlanes(safetyhook::Context& r){
    auto& c=privateCopy;if(!c.latching)return;
    const auto caller=build::canonical(field<std::uintptr_t>(r.rsp+0x88)-base);
    const unsigned bit=caller==0xcc8fb3?0x100:caller==0xcc9633?0x200:caller==0xcc9bd6?0x400:0;
    if(!bit)return;
    // Motor 5060E0 only implements a symmetric inverse: its [0][3]/[1][3]
    // remain zero even for asymmetric planes. Preserve native inverse depth,
    // adding the two analytically determined optical-centre components.
    auto* output=reinterpret_cast<float*>(field<std::uintptr_t>(r.rsp+0x30));
    if(!output)return;
    output[3]=(std::tan(c.fov[1])+std::tan(c.fov[0]))*0.5f;
    output[7]=(std::tan(c.fov[2])+std::tan(c.fov[3]))*0.5f;
    c.builders|=bit;
}
void fullFrameSettings(safetyhook::Context& r){
    if(!allowsRenderSize())return;
    Fps f;{std::lock_guard lock(controlMutex);f=fps;}
    const bool film=fixedCinemaSurface(field<int>(base+build::rva(0x4c6c514))==2);
    std::array<float,12> ledge{};
    if(menuActive()||menuState!=0||(!film&&!ledgeCamera(ledge)&&(!f.allowed||GetTickCount64()-f.ticks>=150)))return;
    // EB2AE0 builds the native frame's render/upscale sizes and framingType.
    // Retail strips r_letterBoxTypeOverride from the console. Override the
    // already selected value, leaving the cvar and saved user settings alone.
    static std::atomic<bool> logged{false};
    if(!logged.exchange(true))circlevr::vkcore::logf("motor-vr-framing: native frame settings %u -> 0 (full eye surface)",static_cast<unsigned>(r.rbp));
    r.rbp=0;
}
void fullFrameOutput(safetyhook::Context& r){
    if(!allowsRenderSize())return;
    Fps f;{std::lock_guard lock(controlMutex);f=fps;}
    const bool film=fixedCinemaSurface(field<int>(base+build::rva(0x4c6c514))==2);
    std::array<float,12> ledge{};
    if(menuActive()||menuState!=0||(!film&&!ledgeCamera(ledge)&&(!f.allowed||GetTickCount64()-f.ticks>=150)))return;
    // B879A0 separately chooses the final composite's letterbox rectangle.
    r.r9=0;
}
void contactShadowDiagnostic(safetyhook::Context& r){
    // C25360's native parameter setup, immediately before C0FB20 receives
    // useContactShadows (global 44CA8F8) in XMM2. A per-run request permits
    // a reversible A/B comparison without changing saved cvars or RT features.
    if(!requested() || !adapterInstalled || !immersive())return;
    static std::atomic<ULONGLONG> lastPoll{0};
    static std::atomic<bool> requestedOff{false};
    auto previous=lastPoll.load();const auto now=GetTickCount64();
    if(now-previous>=500 && lastPoll.compare_exchange_strong(previous,now)){
        const auto path=circlevr::vkcore::logDirectory()+L"\\motor-contact-shadows-off.request";
        requestedOff=GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES;
    }
    const bool off=requestedOff && inGameplay && !stopped;
    const float native=r.xmm2.f32[0];
    if(off)r.xmm2.f32[0]=0.0f;
    static std::atomic<int> lastMode{-1};
    if(lastMode.exchange(off?1:0)!=(off?1:0))
        circlevr::vkcore::logf("motor-contact-shadow-diagnostic: %s; native useContactShadows %.3f -> %.3f; mandatory RT unchanged",off?"OFF comparison":"native",native,r.xmm2.f32[0]);
}
void binTile(safetyhook::Context& r,unsigned component){
    // Motor CEF810 builds light/decal bins with a symmetric FOV even when
    // CC8C30 has latched asymmetric optics. R14 is this job's idRenderView;
    // only the primary view validated by latched() may use the VR frustum.
    if(!inGameplay || stopped || !immersive() || r.r14!=projectedNativeView.load())return;
    circlevr::stereo_seq::Matrix4 projection{};
    if(!read(r.r14+0x18790,projection.data(),sizeof(projection)))return;
    const int width=field<int>(r.r14+0x18f24),height=field<int>(r.r14+0x18f28);
    const auto params=circlevr::stereo_seq::binTileParams(projection,width,height);
    if(!params)return;
    const float values[]={params->width,params->height,params->left,params->top};
    static std::atomic<unsigned> counts[4]{};
    const auto n=++counts[component];
    if(n<=4 || n%3600==0)circlevr::vkcore::logf("motor-vr-bins: component %u view %p size %dx%d projection %.6f %.6f %.6f %.6f native %.7f -> eye %.7f",component,reinterpret_cast<void*>(r.r14),width,height,projection[0],projection[2],projection[5],projection[6],r.xmm2.f32[0],values[component]);
    // Keep the native setter, context, parameter object, and resource lifetime.
    r.xmm2.f32[0]=values[component];
}
void binWidth(safetyhook::Context& r){binTile(r,0);}
void binHeight(safetyhook::Context& r){binTile(r,1);}
void binLeft(safetyhook::Context& r){binTile(r,2);}
void binTop(safetyhook::Context& r){binTile(r,3);}
void transparentDepthSort(safetyhook::Context& r){
    // C1E8C6 (Steam C35AE6) stores the native skip-depth-sort decision in
    // this view's render options. RDI/R8 still hold the original idRenderView.
    // Change AL for this store only; the next instruction reloads RAX.
    // A/B/A on Himalaya ice: native sorting pops, bypass is stable, restore
    // brings popping back. Keep its stable order; refraction.cpp independently
    // recovers physical view depth instead of interpreting zeroed key bits.
    if(!adapterInstalled||stopped||!immersive()||!inGameplay||inCinema||
       monoVrEnabled()||menuActive()||!diagnostics::transparentSortFallback()||
       !r.rbx||!r.rdi||r.r8!=r.rdi)return;
    int viewIndex=-1;
    if(!read(r.rdi+0x18f34,&viewIndex,sizeof(viewIndex)))return;
    const unsigned eye=circlevr::vkcore::seqChainEye()==circlevr::stereo_seq::Eye::Right?1u:0u;
    const auto frame=field<unsigned>(base+build::rva(0x4a71950));
    std::uint64_t token{};
    {
        std::lock_guard lock(proofMutex);
        if(!proof.stereo||!transparentSortScope.matches(r.rdi,proof.token,frame,eye,viewIndex))return;
        token=proof.token;
    }
    const unsigned native=unsigned(r.rax&0xffu);
    // Preserve this validated view's ownership for the independent physical
    // refraction-depth path; the packed key remains an ordering key only.
    refraction::noteStableSort(r.rbx+0x4d4250,r.rdi,frame,token,eye);
    r.rax=(r.rax&~std::uintptr_t(0xffu))|1u;
    static std::atomic<unsigned> counts[2]{};const auto count=++counts[eye];
    if(count<=4||count%2400==0)circlevr::vkcore::logf(
        "motor-transparent-sort: eye %u frame %u token %llu view %p nativeSkip %u -> 1; validated VR view fallback, cvar unchanged; count %u",
        eye,frame,token,reinterpret_cast<void*>(r.rdi),native,count);
}
void packedReflectionInverse(safetyhook::Context& r){
    // C45669: before Motor's SetVec4(packedInverseProjection0). RBX is
    // this pass's view; stack +20/+28 are the two scalar tail arguments.
    // Restrict to our validated primary projection, leaving secondary views,
    // native depth packing and shader/resource lifetime unchanged.
    if(!adapterInstalled||stopped||!immersive()||!inGameplay||
       r.rbx!=projectedNativeView.load()||field<uintptr_t>(r.rsi+0x20)!=r.rbx)return;
    std::array<float,16> inverse{};
    if(!read(r.rbx+0x187d0,inverse.data(),sizeof(inverse)))return;
    const auto corrected=packedInverseProjectionXY(inverse);if(!corrected)return;
    const float beforeZ=field<float>(r.rsp+0x20),beforeW=field<float>(r.rsp+0x28);
    // Verify the exact native ABI/values before modifying any operands.
    if(std::abs(r.xmm2.f32[0]-(*corrected)[0])>1.e-5f||
       std::abs(r.xmm3.f32[0]-(*corrected)[1])>1.e-5f||
       !std::isfinite(beforeZ)||!std::isfinite(beforeW)||
       std::abs(beforeZ+inverse[0])>1.e-5f||std::abs(beforeW-inverse[5])>1.e-5f)return;
    const bool enabled=diagnostics::packedReflectionInverse();
    if(enabled){
        memcpy(reinterpret_cast<void*>(r.rsp+0x20),&(*corrected)[2],sizeof(float));
        memcpy(reinterpret_cast<void*>(r.rsp+0x28),&(*corrected)[3],sizeof(float));
    }
    const auto eye=circlevr::vkcore::seqChainEye();
    const unsigned e=eye==circlevr::stereo_seq::Eye::Right?1:eye==circlevr::stereo_seq::Eye::Left?0:2;
    static std::atomic<unsigned> counts[3]{};const auto n=++counts[e];
    if(n<=8||n%300==0)circlevr::vkcore::logf(
        "motor-packed-reflection: eye %u view %p enabled %d inverse centre %.7f %.7f native %.7f %.7f corrected %.7f %.7f count %u",
        e,reinterpret_cast<void*>(r.rbx),enabled,inverse[3],inverse[7],beforeZ,beforeW,(*corrected)[2],(*corrected)[3],n);
}
#include "hair_pair_runtime.inl"
void hairVisibilityProjection(safetyhook::Context& r){
    // 89A727: COMISS XMM1,[RAX+C] tests projected bbox area against
    // r_hairMinVisibleArea; RDI is this hair's per-view data (+178=view).
    // All preceding clipping/bounds rejection and following tile allocation
    // remain native. XMM1 is dead after the comparison (reloaded at 89A755).
    if(!adapterInstalled||stopped||!immersive()||
       field<uintptr_t>(r.rdi+0x178)!=projectedNativeView.load()||
       r.rax!=field<uintptr_t>(base+build::rva(0x44591f0)))return;
    const float minimum=field<float>(r.rax+0xc),area=r.xmm1.f32[0];
    const auto expanded=hairVisibilityArea(area);
    if(!expanded||!std::isfinite(minimum)||minimum<0||minimum>1)return;
    r.xmm1.f32[0]=*expanded;
    const auto eye=circlevr::vkcore::seqChainEye();
    const unsigned index=eye==circlevr::stereo_seq::Eye::Right?1:eye==circlevr::stereo_seq::Eye::Left?0:2;
    static std::atomic<unsigned> counts[3]{},rescues[3]{};const unsigned n=++counts[index];
    const bool newlyVisible=area<minimum&&*expanded>=minimum;
    const unsigned recovered=newlyVisible?++rescues[index]:rescues[index].load();
    if(n<=4||n%2400==0||(newlyVisible&&recovered<=8))
        circlevr::vkcore::logf("motor-hair-visibility: eye %u area %.9g cutoff %.9g -> %.9g newlyVisible %d recovered %u count %u",
            index,area,minimum,minimum*.25f,newlyVisible?1:0,recovered,n);
}
void hairWidthProjection(safetyhook::Context& r){
    // 89AEA7, before DIVSS XMM3,XMM0. RDI=hair model, RSI=the actual view;
    // XMM4=world strand width; XMM1/2 contain projected point numerators.
    // Only the primary view whose native optical builders we validated may
    // enter this path. Never modify a shared view matrix or a hair resource.
    if(!adapterInstalled||stopped||!immersive()||r.rsi!=projectedNativeView.load()||
       field<uintptr_t>(r.rdi)!=base+build::rva(0x3649f60))return;
    std::array<float,16> p{};
    if(!read(r.rsi+0x18790,p.data(),sizeof(p)))return;
    const auto corrected=hairWidthNumerators(p,r.xmm4.f32[0]);
    if(!corrected)return;
    const float beforeX=r.xmm1.f32[0],beforeY=r.xmm2.f32[0];
    r.xmm1.f32[0]=(*corrected)[0];r.xmm2.f32[0]=(*corrected)[1];
    const auto eye=circlevr::vkcore::seqChainEye();
    const unsigned index=eye==circlevr::stereo_seq::Eye::Right?1:eye==circlevr::stereo_seq::Eye::Left?0:2;
    view_audit::sample(view_audit::Stage::HairSetup,index,
        field<unsigned>(base+build::rva(0x4a71950)),0,
        field<std::uint64_t>(base+build::rva(0x4c523a8)),r.rsi,r.rdi);
    static std::atomic<unsigned> counts[3]{};const unsigned n=++counts[index];
    if(n<=8||n%2400==0){
        char name[160]{};read(field<uintptr_t>(r.rdi+0x90),name,sizeof(name)-1);
        circlevr::vkcore::logf("motor-hair-projection: eye %u view %p width %.9g centre %.7g %.7g numerator %.9g %.9g -> %.9g %.9g count %u asset %s",
            index,reinterpret_cast<void*>(r.rsi),r.xmm4.f32[0],p[2],p[6],beforeX,beforeY,r.xmm1.f32[0],r.xmm2.f32[0],n,name);
    }
}
void latched(safetyhook::Context& r){
    if(!immersive() || !adapterInstalled || stopped)return;
    auto& c=privateCopy;
    if(c.kind!=3 || field<std::uintptr_t>(r.r13+0x40)!=c.dest)return;
    std::lock_guard lock(proofMutex);
    if(!proof.stereo || !currentEyes[c.eye].immersive)return;
    std::array<float,16> actual{};
    bool valid=read(c.dest+0x18790,actual.data(),sizeof(actual)) && c.builders==0x7ff;
    const auto& f=c.fov;
    const auto expected=circlevr::xr_math::engineProjection({f[0],f[1],f[2],f[3]},actual);
    valid=valid && expected.has_value();
    if(expected)for(unsigned i=0;i<16;++i)valid=valid && std::isfinite(actual[i]) && std::fabs(actual[i]-(*expected)[i])<0.0001f;
    std::array<float,16> inverse{};
    valid=valid && read(c.dest+0x187d0,inverse.data(),sizeof(inverse));
    for(unsigned row=0;row<4;++row)for(unsigned col=0;col<4;++col){
        float value=0;for(unsigned k=0;k<4;++k)value+=actual[row*4+k]*inverse[k*4+col];
        valid=valid && std::isfinite(value) && std::fabs(value-(row==col?1.f:0.f))<0.001f;
    }
    currentEyes[c.eye].projection=actual;
    if(valid)projectedNativeView=c.dest;
    static unsigned counts[2]{};const auto n=++counts[c.eye];
    if(diagnostics::enabled()&&n<=2){
        std::array<unsigned char,0x7a0> matrices{};
        if(read(c.dest+0x18790,matrices.data(),matrices.size())){
            const auto path=circlevr::vkcore::logDirectory()+L"\\motor-projections-eye"+std::to_wstring(c.eye)+L"-"+std::to_wstring(n)+L".bin";
            if(FILE* file=circlevr::vkcore::openLogFileShared(path.c_str(),L"wb",_SH_DENYWR)){fwrite(matrices.data(),1,matrices.size(),file);fclose(file);}
        }
    }
    if(n<=4 || n%300==0 || !valid)circlevr::vkcore::logf("motor-vr-projection: eye %d n %u native latch %s builders %02x matrix %.7f %.7f %.7f %.7f depth %.7f %.7f %.7f %.7f",c.eye,n,valid?"MATCH":"MISMATCH",c.builders,actual[0],actual[2],actual[5],actual[6],actual[10],actual[11],actual[14],actual[15]);
    if(!valid){stopped=true;inGameplay=false;return;}
    currentEyes[c.eye].sequence=field<unsigned>(base+build::rva(0x4a71950));
    transparentSortScope.note(c.dest,proof.token,static_cast<unsigned>(currentEyes[c.eye].sequence),
        static_cast<unsigned>(c.eye),!currentEyes[c.eye].cinema&&!monoVrEnabled());
    view_audit::sample(view_audit::Stage::Latch,c.eye,
        static_cast<unsigned>(currentEyes[c.eye].sequence),0,proof.token,c.dest);
    if(gpu_audit::recording()){
        std::array<unsigned char,0x980> viewBytes{};
        if(read(c.dest+0x18790,viewBytes.data(),viewBytes.size()))
            gpu_audit::projection(proof.token,static_cast<unsigned>(currentEyes[c.eye].sequence),c.eye,viewBytes.data(),unsigned(viewBytes.size()));
    }
    {
        std::lock_guard hudLock(hudMutex);
        const auto& e=currentEyes[c.eye];auto& h=hudFrames[e.sequence%hudFrames.size()];h={};
        if(!e.cinema){
            float ipdSquared=0;for(unsigned k=0;k<3;++k){const float d=e.rig.positions[1][k]-e.rig.positions[0][k];ipdSquared+=d*d;}
            const float eyeX=(c.eye?1.f:-1.f)*std::sqrt(ipdSquared)*.5f;
            if(const auto vp=hudViewport(e.rig.fovs[c.eye],eyeX))h={static_cast<unsigned>(e.sequence),c.eye,proof.token,*vp};
        }
    }
    circlevr::vkcore::seqMarkEyeView(c.eye?circlevr::stereo_seq::Eye::Right:circlevr::stereo_seq::Eye::Left,proof.token);
}
void event(unsigned kind,std::uintptr_t packet,std::uintptr_t info=0,std::uintptr_t view=0,std::uintptr_t destination=0) {
    if(!diagnostics::enabled())return;
    const auto n=++events[kind];
    if(n>16 && n%120)return;
    const auto rs=base+build::rva(0x4a717f0);
    if(packet)info=field<std::uintptr_t>(packet+8);
    const auto backend=field<std::uintptr_t>(rs+0x11a8);
    std::array<float,12> camera{};if(view)read(view+0xa8,camera.data(),48);
    circlevr::vkcore::logf("motor-seq-observe: stage %u n %u tid %lu state %d packet %p info %p rs %p vtable %p guard %u frontend %u skip %u backend %p views %d/%d view %p dest %p xyz %.6f %.6f %.6f",
        kind,n,GetCurrentThreadId(),field<int>(base+build::rva(0x4c6c514)),reinterpret_cast<void*>(packet),reinterpret_cast<void*>(info),reinterpret_cast<void*>(rs),
        reinterpret_cast<void*>(field<std::uintptr_t>(rs)),field<unsigned char>(rs+0x158),field<unsigned>(rs+0x160),field<unsigned>(rs+0x164),reinterpret_cast<void*>(backend),
        info?field<int>(info+8):0,info?field<int>(info+0x1588):0,reinterpret_cast<void*>(view),reinterpret_cast<void*>(destination),camera[0],camera[1],camera[2]);
    // Bounded snapshots permit offline pointer/ownership comparison without changing native data.
    const int state=field<int>(base+build::rva(0x4c6c514));
    static std::atomic<unsigned> saved[3]{};
    if(kind==0 && state>=0 && state<3 && saved[state].fetch_add(1)<3) {
        const auto dir=circlevr::vkcore::logDirectory()+L"\\motor-seq";CreateDirectoryW(dir.c_str(),nullptr);
        struct Region {const wchar_t* name;std::uintptr_t address;std::size_t size;};
        const Region regions[]={{L"info",info,0x2b10},{L"backend",backend,0x900},
            {L"views",field<std::uintptr_t>(info),0x1580},{L"aux",field<std::uintptr_t>(info+0x1580),0x1580}};
        for(const auto& region:regions) {
            std::array<unsigned char,0x2b10> bytes{};if(!read(region.address,bytes.data(),region.size))continue;
            wchar_t name[128]{};swprintf_s(name,L"\\s%d-n%u-%s-%llx.bin",state,n,region.name,static_cast<unsigned long long>(region.address));
            FILE* file=nullptr;if(!circlevr::vkcore::openLogFile(&file,(dir+name).c_str(),L"wb") && file){fwrite(bytes.data(),1,region.size,file);fclose(file);}
        }
    }
}
void begin(safetyhook::Context& r){comfort::enforcePostEffects();activeInfo=field<std::uintptr_t>(r.rcx+8);event(0,r.rcx);}
void end(safetyhook::Context& r){event(1,r.rcx);}
void released(safetyhook::Context& r){event(2,r.rdi);}
void world(safetyhook::Context& r){
    // A mono copy must never inherit a committed eye from an earlier frame
    // that reused the same native destination (notably paused backgrounds).
    privateCopy={};
    event(3,0,0,r.r15+0x30,r.rcx);
    if(adapterInstalled && !based)circlevr::vkcore::seqMarkWanted();
    if(!adapterInstalled)return;
    std::lock_guard lock(proofMutex);
    if(circlevr::vkcore::seqChainEye()==circlevr::stereo_seq::Eye::Right) {
        std::array<unsigned char,48> camera{};
        const bool same=proof.started && simulationEntries==proof.simulation && experimental::selectedViewSequence()==proof.cameraCopies && activeInfo==proof.info &&
            field<std::uint64_t>(base+build::rva(0x4c523a8))==proof.token && read(r.r15+0x30+0xa8,camera.data(),camera.size()) && camera==proof.camera;
        if(!proof.stereo || !same)circlevr::vkcore::logf("motor-seq-reentry: RIGHT consumed source %p info %p simulation %llu token %llu; same_state %d; SAME-VIEW ABI PROBE, not stereo",
            reinterpret_cast<void*>(r.r15+0x30),reinterpret_cast<void*>(activeInfo.load()),simulationEntries.load(),field<std::uint64_t>(base+build::rva(0x4c523a8)),same?1:0);
        if(same){if(proof.stereo)redirect(r,3,1);else circlevr::vkcore::seqMarkEyeView(circlevr::stereo_seq::Eye::Right,proof.token);}
        return;
    }
    static ULONGLONG lastPoll=0;const auto now=GetTickCount64();
    if(now-lastPoll>=1000){lastPoll=now;const auto path=circlevr::vkcore::logDirectory()+L"\\probe-render-reentry.request";if(GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES && DeleteFileW(path.c_str()))probeArmed=true;}
    if(proof.stereo && proof.token==field<std::uint64_t>(base+build::rva(0x4c523a8)) && proof.info==activeInfo && !stopped){proof.source=r.r15+0x30;redirect(r,3,0);return;}
    if(!probeArmed)return;
    if(circlevr::vkcore::seqStereoReadiness()!=circlevr::vkcore::SeqReadiness::Ready){circlevr::vkcore::seqMarkWanted();return;}
    proof={};proof.info=activeInfo;proof.source=r.r15+0x30;proof.simulation=simulationEntries;proof.token=field<std::uint64_t>(base+build::rva(0x4c523a8));
    proof.cameraCopies=experimental::selectedViewSequence();
    if(field<int>(proof.info+8)!=1 || field<int>(proof.info+0x1588)!=1 || !proof.token || !read(proof.source+0xa8,proof.camera.data(),proof.camera.size()))return;
    circlevr::vkcore::seqMarkEyeView(circlevr::stereo_seq::Eye::Left,proof.token);
    circlevr::vkcore::logf("motor-seq-reentry: LEFT same-view ABI probe; info %p simulation %llu token %llu render %u",reinterpret_cast<void*>(proof.info),proof.simulation,proof.token,field<unsigned>(base+build::rva(0x4a71950)));
}
void aux(safetyhook::Context& r){
    privateCopy={};
    event(4,0,0,r.rbx+0x30,r.rcx);if(!adapterInstalled || stopped)return;
    std::lock_guard lock(proofMutex);
    if(circlevr::vkcore::seqChainEye()==circlevr::stereo_seq::Eye::Right){if(proof.stereo && proof.started)redirect(r,4,1);return;}
    proof.stereo=false;const auto source=r.rbx+0x30;
    const bool menus=menuActive();
    const bool playing=!menus&&gameplay(source);
    const bool cinema=!menus && !playing && menuState==0 && field<int>(base+build::rva(0x4c6c514))==2;
    const bool inspection=cinema&&locomotion::flatInteractionActive();
    const bool monoCinema=monoCinemaView(cinema,inspection,stereoCinemaEnabled()&&!monoVrEnabled());
    inCinema=cinema;
    cinema_timing::scene(cinema&&!inspection&&!experimental::pauseMenuActive());
    if(!playing&&!cinema){inGameplay=false;projectedNativeView=0;return;}
    if(circlevr::vkcore::seqStereoReadiness()!=circlevr::vkcore::SeqReadiness::Ready){circlevr::vkcore::seqMarkWanted();inGameplay=false;return;}
    proof={};proof.info=activeInfo;proof.source=source;proof.token=field<std::uint64_t>(base+build::rva(0x4c523a8));proof.simulation=simulationEntries;proof.cameraCopies=experimental::selectedViewSequence();
    if(field<int>(proof.info+8)!=1 || field<int>(proof.info+0x1588)!=1 || !read(source+0xa8,proof.camera.data(),48) || !experimental::buildSequentialEyes(source,proof.token,privateEyes,currentEyes,cinema,monoCinema)){inGameplay=false;projectedNativeView=0;return;}
    for(auto& eye:currentEyes)eye.inspection=inspection;
    const bool cropRuntime=!cinema&&!monoVrEnabled()&&nativeRuntimeOptics(circlevr::vkcore::xrRuntimeName());
    if(cropRuntime)for(unsigned e=0;e<2;++e)currentEyes[e].runtimeFov=currentEyes[e].rig.fovs[e];
    const bool symmetric=!cinema&&!monoVrEnabled()&&diagnostics::symmetricOptics().load()&&symmetricStereoOptics(currentEyes);
    for(auto& eye:currentEyes)eye.runtimeFovCrop=cropRuntime&&symmetric;
    static int lastSymmetric=-1;
    if(lastSymmetric!=int(symmetric)){
        lastSymmetric=int(symmetric);
        circlevr::vkcore::logf("motor-stereo-optics: %s; distinct eye poses retained; matching projection submitted to XR",symmetric?"SYMMETRIC":"native/cinema/mono");
    }
    if(monoVrEnabled()){
        if(!centerMonoViews(currentEyes)){inGameplay=false;projectedNativeView=0;return;}
        for(unsigned e=0;e<2;++e)std::memcpy(privateEyes[e].data()+0xa8,currentEyes[e].camera.data(),48);
    }
    proof.stereo=true;inGameplay=true;redirect(r,4,0);
    static int lastPresentation=-1;
    const int presentation=inspection?3:monoCinema?2:(cinema?1:0);
    if(lastPresentation!=presentation){
        lastPresentation=presentation;
        circlevr::vkcore::logf("motor-seq-presentation: %s token %llu authored FOV %.3f %.3f; same-frame native eyes",inspection?"MONO_INSPECTION":monoCinema?"MONO_CINEMA":(cinema?"STEREO_CINEMA":"IMMERSIVE"),proof.token,currentEyes[0].fovX,currentEyes[0].fovY);
    }
}
void sync(safetyhook::Context& r){event(5,0,r.r8);}
void simulation(safetyhook::Context&){++simulationEntries;}
bool allowsTouch(){return requested() && adapterInstalled.load();}
bool reentryValidated(void* info){
    std::lock_guard lock(proofMutex);std::array<unsigned char,48> camera{};
    const bool armed=probeArmed.exchange(false) || (proof.stereo && !stopped);
    const bool valid=armed && proof.info==reinterpret_cast<std::uintptr_t>(info) && simulationEntries==proof.simulation && experimental::selectedViewSequence()==proof.cameraCopies &&
        field<std::uint64_t>(base+build::rva(0x4c523a8))==proof.token && read(proof.source+0xa8,camera.data(),camera.size()) && camera==proof.camera;
    proof.started=valid;
    if(!proof.stereo || !valid)circlevr::vkcore::logf("motor-seq-reentry: preflight %s; sim %llu -> %llu token %llu -> %llu; one bounded native render, unchanged camera",valid?"PASS":"REJECT",proof.simulation,simulationEntries.load(),proof.token,field<std::uint64_t>(base+build::rva(0x4c523a8)));
    return valid;
}
void completed(void* info) {
    std::lock_guard lock(proofMutex);
    const auto copies=experimental::selectedViewSequence();const auto token=field<std::uint64_t>(base+build::rva(0x4c523a8));
    const bool unchanged=proof.started && proof.info==reinterpret_cast<std::uintptr_t>(info) && proof.simulation==simulationEntries && proof.cameraCopies==copies && proof.token==token;
    if(proof.stereo && unchanged){
        auto& saved=savedPairs[(savedIndex++)%savedPairs.size()];saved.token=proof.token;saved.eyes=currentEyes;
        if(diagnostics::enabled()){
        static FILE* csv=nullptr;static bool opened=false;
        if(!opened){opened=true;csv=circlevr::vkcore::openLogFileShared((circlevr::vkcore::logDirectory()+L"\\motor-sequential-pairs.csv").c_str(),L"wb",_SH_DENYWR);if(csv)fprintf(csv,"token,selectedCopy,broadBoundary,leftRender,rightRender,rigQpc,rigDisplayTime,baseX,baseY,baseZ,leftX,leftY,leftZ,rightX,rightY,rightZ\n");}
        if(csv){const auto& l=currentEyes[0];const auto& r=currentEyes[1];fprintf(csv,"%llu,%llu,%llu,%llu,%llu,%lld,%lld,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",proof.token,proof.cameraCopies,proof.simulation,l.sequence,r.sequence,l.rig.locatedQpc,l.rig.displayTime,l.base[0],l.base[1],l.base[2],l.camera[0],l.camera[1],l.camera[2],r.camera[0],r.camera[1],r.camera[2]);if(savedIndex%120==0)fflush(csv);}
        }
    }
    if(proof.stereo && !unchanged){stopped=true;inGameplay=false;}
    static unsigned completedCount=0;
    if(!proof.stereo || !unchanged || ++completedCount<=12 || completedCount%120==0)circlevr::vkcore::logf("motor-seq-reentry: COMPLETE unchanged %d; selected-view copies %llu -> %llu token %llu -> %llu broad-game-boundary entries %llu -> %llu; right render %u, render guard %u",
        unchanged?1:0,proof.cameraCopies,copies,proof.token,token,proof.simulation,simulationEntries.load(),field<unsigned>(base+build::rva(0x4a71950)),field<unsigned char>(base+build::rva(0x4a71948)));
}
}
bool playerCameraOrigin(uint64_t owner,const std::array<float,3>& camera,std::array<float,3>& origin){
 const auto n=locomotion::latestNative();const auto now=GetTickCount64();
 const bool armedPassenger=locomotion::passengerCombatActive(owner);
 if(!adapterInstalled||stopped||!inGameplay||inCinema||!n.valid||n.entity!=owner||now<n.tick||now-n.tick>100||
    (!armedPassenger&&!locomotion::trackedGameplayState(n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace)))return false;
 std::lock_guard lock(modelAuditMutex);
 for(const auto& m:modelAuditEntries){
  if(m.owner!=owner||std::strcmp(m.name,"md6def/characters/standard/indiana_jones/indiana_wear_body.md6"))continue;
  if(!(field<unsigned char>(m.model+0x183)&2)||field<uintptr_t>(m.model+0x88)!=m.committed||
     field<uintptr_t>(m.committed+0x28)!=m.manager||field<unsigned>(m.committed+0x30)!=m.index)continue;
  char asset[160]{},instance[192]{};std::array<float,3> p{};
  if(!read(field<uintptr_t>(m.model+0x90),asset,sizeof(asset)-1)||std::strcmp(asset,m.name)||
     !read(field<uintptr_t>(m.model+0x98),instance,sizeof(instance)-1)||!cockpitPlayerInstance(asset,instance)||
     !read(m.model+0x120,p.data(),sizeof(p)))continue;
  const locomotion::Vec eye{camera[0],camera[1],camera[2]},body{p[0],p[1],p[2]};
  if(armedPassenger?!locomotion::coherentPassengerBody(eye,n.position,body):!locomotion::coherentRenderBody(n.movementMode,eye,n.position,body))continue;
  origin=p;return true;
 }
 return false;
}
bool requested(){static const bool v=[] {wchar_t s[8]{};return isTargetProcess() && GetEnvironmentVariableW(L"CIRCLE_SEQUENTIAL_STEREO",s,8)==1 && s[0]==L'1' && build::supported();}();return v;}
void installSecondaryCacheResources(){
    if(!secondary_cache::requested())return;
    static std::once_flag once;
    std::call_once(once,[]{
        base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        struct Point {uintptr_t at;const char* bytes;unsigned size;void(*callback)(safetyhook::Context&);};
        const Point points[]={
            {0xd45fcd,"\xe8\xee\x88\x05\x00",5,secondaryPayloadTemplate},
            {0xd4606b,"\xe8\x50\x88\x05\x00",5,secondaryRecordTemplate},
            {0xd460e2,"\x48\x8b\x05\x17\x14\x7b\x03",7,secondaryCacheCreate}};
        for(auto& p:points)if(!build::matches(p.at,p.bytes,p.size)){
            secondary_cache::shaderFailed();circlevr::vkcore::logf("motor-secondary-cache: constructor fingerprint rejected");return;
        }
        for(auto& p:points){
            auto h=safetyhook::MidHook::create(reinterpret_cast<void*>(base+build::rva(p.at)),p.callback);
            if(!h){secondary_cache::shaderFailed();circlevr::vkcore::logf("motor-secondary-cache: constructor hook failed");return;}
            new safetyhook::MidHook(std::move(*h));
        }
        circlevr::vkcore::logf("motor-secondary-cache: early constructor hooks installed");
    });
}
bool monoVrEnabled(){static const bool on=[] {wchar_t s[8]{};return requested()&&GetEnvironmentVariableW(L"CIRCLE_MONO_VR",s,8)==1&&s[0]==L'1';}();return on;}
bool completeMonoFrame(void* info,std::uint64_t token,std::uint32_t renderFrame){
    std::lock_guard lock(proofMutex);
    if(!monoVrEnabled()||!proof.stereo||stopped||proof.info!=reinterpret_cast<std::uintptr_t>(info)||
       proof.token!=token||proof.simulation!=simulationEntries||proof.cameraCopies!=experimental::selectedViewSequence()||
       currentEyes[0].sequence!=renderFrame||field<std::uint64_t>(base+build::rva(0x4c523a8))!=token)return false;
    auto& saved=savedPairs[(savedIndex++)%savedPairs.size()];saved.token=token;
    saved.eyes[0]=saved.eyes[1]=currentEyes[0];saved.eyes[1].eye=1;
    return true;
}
bool installObservation(std::string& error) {
    static std::once_flag once;static bool ok=false;static std::string why;
    std::call_once(once,[]{
        NativeStateObserver observer;if(!observer.sample().nativeStateValid){
            if(!build::supported())circlevr::vkcore::logf("motor-build: unrecognized executable; checked SHA-256 %s",build::checkedHash()[0]?build::checkedHash():"(PE timestamp/size match no profile)");
            why=observer.validation();return;}
        base=reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
        circlevr::vkcore::logf("motor-build: exact executable profile %s (SHA-256 %s); separate address table and instruction fingerprints",build::current()->name,build::checkedHash());
        if(field<std::uintptr_t>(base+build::rva(0x4a717f0))!=base+build::rva(0x369fea0) || field<std::uintptr_t>(base+build::rva(0x369fef8))!=base+build::rva(0xc978a0) ||
           field<std::uintptr_t>(base+build::rva(0x3ea09e0))!=base+build::rva(0xc98240)){why="Motor render-system/vtable/frame-end slot mismatch";return;}
        if(!build::matches(0xd9f920,"\x40\x55\x53\x41\x55\x41\x57",7)){why="Native sparse model upload fingerprint mismatch";return;}
        struct Point {std::uintptr_t rva;const char* bytes;std::size_t size;void (*callback)(safetyhook::Context&);};
        const Point points[]={
            {0x89a953,"\xf3\x0f\x10\xaf\x20\x01\x00\x00",8,hairCurrentPlacementBegin},
            {0x89aab7,"\xf3\x0f\x5c\xd1\x41\x0f\x28\xca",8,hairCurrentPlacementEnd},
            {0x899fc2,"\xf3\x0f\x10\x81\x20\x01\x00\x00",8,hairHistoryCurrentPlacement},
            {0xc26a39,"\x8b\xf8\x81\xe7\x01\x00\x00\x80",8,secondaryCacheCounter},
            {0xc26a64,"\x48\x8b\x0d\xc5\xa7\xd9\x03",7,secondaryCachePick},
            {0xc26b2c,"\x48\x8d\x0d\xdd\xcf\xe4\x03",7,secondaryCacheRestore},
            {0xc1d347,"\xc3\xcc\xcc\xcc\xcc",5,secondaryPayloadResult},
            {0xc1d367,"\xc3\xcc\xcc\xcc\xcc",5,secondaryRecordResult},
            {0x83e265,"\xe8\x16\x43\x16\x00",5,particlePrepareBefore},
            {0x83e26a,"\x4c\x8b\x5b\x78\xb8\x56\x55\x55\x55",9,particlePrepareAfter},
            {0x83e3b5,"\x89\xa8\x08\x04\x00\x00",6,particlePoolReset},
            {0x8a1f38,"\xe8\x73\x09\x00\x00",5,particleLightBind},
            {0x8a1f43,"\xe8\x18\x04\x00\x00",5,particleBindStart},
            {0x8a23dd,"\x48\x85\xd2\x7e\x4a",5,particleBindClear},
            {0x8a2611,"\x41\x8b\x44\x28\x44",5,particleBindCpu},
            {0x8a2537,"\x49\x8b\x8e\x50\x06\x00\x00",7,particleBindGpu},
            {0x8a2e1c,"\x8d\x50\x0c\x40\x84\xf6",6,lighterParticleBegin},
            {0x8a2e7d,"\x0f\x10\x00\x8b\x93\xd8\x05\x00\x00",9,lighterParticlePreviousAxes},
            {0x8a2f90,"\xf3\x44\x0f\x10\x85\xb0\x01\x00\x00",9,lighterParticleAngularAxes},
            {0x8a313f,"\x0f\x10\x00\x0f\x11\x83\xe4\x04\x00\x00",10,lighterParticleCurrentAxes},
            {0x8a32d4,"\xf3\x0f\x10\x5c\x24\x40",6,lighterParticleCurrentAxes},
            {0x8a40d9,"\xe8\x72\xd4\x33\x00",5,lighterParticleGpuUpdate},
            {0xbdf500,"\x48\x89\x5c\x24\x08",5,lighterParticleDrawBind},
            {0x8a1f51,"\xe8\xea\x0c\x00\x00",5,particleGenerate},
            {0x89e81e,"\x8b\x8e\x70\x90\x01\x00",6,effectGenerate},
            {0x94c8b4,"\x74\x17\xff\xc0\x25\x01\x00\x00\x80",9,npcSkinSelection},
            {0x94a75f,"\x75\x07\xb8\x00\x00\x00\x01",7,secondaryAnimationSurface},
            {0x94d818,"\x75\x07\xb8\x00\x00\x00\x01",7,secondaryAnimationSurface},
            {0xc97b1b,"\x48\x6b\xc8\x58\x89\x94\x19\x0c\x02\x00\x00",11,glyphCount},
            {0xc97b81,"\xe8\xba\x7a\x10\x00",5,glyphUpload},
            {0x8854ca,"\x4d\x8b\x04\x24\x4d\x85\xc0",7,heldWaxDispatchTransform},
            {0x88a4e1,"\xf3\x45\x0f\x11\xae\x90\x00\x00\x00",9,heldWaxComputeTransform},
            {0xcdad2b,"\x48\x8b\x9c\x24\xc8\x01\x00\x00",8,playerBodyGpuVisibility},
            {0xc42cca,"\xf3\x0f\x11\x87\xa0\x00\x00\x00",8,scatterCurrentVolume},
            {0x89a439,"\x0f\x85\xe7\x01\x00\x00",6,playerHandHairVisibility},
            {0x89aea7,"\xf3\x0f\x5e\xd8\x0f\x57\xc0",7,hairWidthProjection},
            {0x89a727,"\x0f\x2f\x48\x0c\x0f\x82\x29\x01\x00\x00",10,hairVisibilityProjection},
            {0x83f09f,"\x80\xf9\x01\x0f\x85\xd9\x00\x00\x00",9,playerModelStatus},
            {0x19c2341,"\x48\x8b\x55\xb8\xb8\xc0\x00\x00\x00",9,observeWhipRenderBinding},
            {0x83f068,"\x84\xc9\x0f\x84\x80\x05\x00\x00",8,temporalMovedStatus},
            {0xc5c9c3,"\x4c\x8b\xcb\x49\x8b\xcf",6,playerModelPlacement},
            {0xc5c9f4,"\xf6\x47\x0c\x10\x75\x26",6,playerModelMatrix},
            {0xcda962,"\x48\x39\x87\xb0\x2d\x01\x00",7,objectUploadAudit},
            {0xcda194,"\xb8\x56\x55\x55\x55",5,objectCurrent},
            {0xcda1fd,"\xb8\x56\x55\x55\x55",5,objectCurrentMatrices},
            {0xcda266,"\x83\xc1\x02\xb8\x56\x55\x55\x55",8,objectPrevious},
            {0xcda2d2,"\x83\xc1\x02\xb8\x56\x55\x55\x55",8,objectPreviousMatrices},
            {0xcda8e1,"\xb8\x56\x55\x55\x55",5,objectUploadPick},
            {0xcda1aa,"\x48\x8b\xb4\xc7\x18\x5f\x04\x00",8,objectBindAudit},
            {0x88d706,"\x80\x89\x87\x00\x00\x00\x40\x4c\x8d\xb9\x30\x04\x00\x00",14,guiBuild},
            {0xc4953f,"\x49\x3b\x80\xb0\x2d\x01\x00",7,worldGuiDraw},
            {0xc4ce50,"\x49\x3b\x86\xb0\x2d\x01\x00",7,worldGuiVisibility},
            {0x88df63,"\x41\x89\xbd\xa0\x04\x00\x00\x41\x89\xbd\xd8\x04\x00\x00",14,guiConsumed},
            {0x88dcf2,"\x48\x6b\xd0\x70\x41\xc1\xe1\x04",8,guiBuffer},
            {0x88dd09,"\x44\x8b\x43\x1c\x48\x83\xc0\x03",8,guiBuffer},
            {0x88dcb3,"\xe8\xf8\x78\x30\x00",5,guiBinding},
            {0x88dcfd,"\xe8\xae\x78\x30\x00",5,guiBinding},
            {0x88dd30,"\xe8\x7b\x78\x30\x00",5,guiBinding},
            {0xc97a40,"\x40\x56\x41\x56\xb8\xa8\x38\x00\x00",9,begin},
            {0xc98240,"\x48\x89\x5c\x24\x20\x57\xb8\x70\x38\x00\x00",11,end},
            {0xc983b9,"\x48\x8b\x8c\x24\x60\x38\x00\x00",8,released},
            {0xc4a926,"\x49\x8d\x57\x30\xe8\x21\x75\x9f\xff",9,world},
            {0xc4a34f,"\x48\x8d\x53\x30\x48\x8b\xf8",7,aux},
            {0xc978a0,"\x48\x89\x5c\x24\x08\x48\x89\x6c\x24\x10",10,sync},
            {0xc4a92f,"\x80\x3d\x79\x6c\xdb\x03\x00",7,copied},
            {0xc4a35b,"\x48\x8b\x0d\xf6\x37\xdd\x03",7,copied},
            {0xc4a39c,"\x48\x8d\x53\x10\x44\x89\x7c\x24\x20",9,previousStored},
            {0xc4a9fb,"\x48\x8b\x05\xce\x3c\x89\x03",7,latched},
            {0xc45669,"\xe8\xb2\xa5\xfc\xff",5,packedReflectionInverse},
            {0xc1e8c6,"\x88\x83\x65\x42\x4d\x00",6,transparentDepthSort},
            {0xcc8c30,"\x4c\x8b\xdc\x55\x53\x57\x41\x56",8,latchBegin},
            {0xcc9e20,"\x48\x8b\x8d\x10\x0a\x00\x00",7,latchEnd},
            {0x5060b9,"\xe8\x62\xfe\xff\xff",5,opticalPlanes},
            {0x506259,"\xe8\x82\xfe\xff\xff",5,opticalPlanes},
            {0x50625e,"\x0f\x28\x7c\x24\x70",5,inverseOpticalPlanes},
            {0xeb2b15,"\x48\x8b\x0d\x3c\xb0\xb6\x03",7,fullFrameSettings},
            {0xb87add,"\x48\x8b\x05\xfc\x7b\x95\x03",7,fullFrameOutput},
            {0xc27932,"\xe8\xe9\x81\xfe\xff",5,contactShadowDiagnostic},
            {0xcefb97,"\xe8\x84\xff\xf1\xff",5,binWidth},
            {0xcefbbd,"\xe8\x5e\xff\xf1\xff",5,binHeight},
            {0xcefbd6,"\xe8\x45\xff\xf1\xff",5,binLeft},
            {0xcefbef,"\xe8\x2c\xff\xf1\xff",5,binTop},
            {0x1cc4192,"\xe8\x79\xe2\xff\xff",5,menu},
            {0x1cc1860,"\x48\x89\x5c\x24\x08\x48\x89\x74\x24\x10",10,menuDestroyed},
            {0x15919d8,"\x80\x7b\x5d\x01\x44\x0f\x28\xa4\x24\x80\x00\x00\x00",13,fpsComplete},
            {0xf1aef0,"\x40\x55\x41\x54\x48\x8d\xac\x24\x28\x76\xff\xff",12,simulation}};
        for(const auto& p:points){if(!build::matches(p.rva,p.bytes,p.size)){why="Motor render-boundary fingerprint mismatch at "+std::to_string(p.rva);return;}}
        const struct {uintptr_t at;const char* bytes;size_t size;} particleChecks[]={
            {0x9a2580,"\x48\x8b\x81\xa0\x07\x00\x00\x45\x33\xc9\x4c\x8b\xc1",13},
            {0x842897,"\x45\x8b\x88\xb0\x07\x00\x00\xb8\x56\x55\x55\x55\x49\x89\xa8\xa8\x07\x00\x00",19},
            {0x8a3640,"\x8b\x88\x80\x04\x00\x00\x89\x8b\xf8\x05\x00\x00",12},
            {0x89e454,"\x41\x8b\x80\x80\x04\x00\x00\xff\xc8\x39\x81\xd8\x44\x00\x00",15},
            {0x89ebe2,"\x48\x8b\x02\x8b\x88\x80\x04\x00\x00\x41\x89\x8d\xd8\x44\x00\x00",16},
            {0x8a2551,"\x80\xb9\x30\x07\x00\x00\x00\x0f\x85\x00\x02\x00\x00",13},
            {0x83e3bb,"\x48\x8b\x5c\x24\x30",5},
            {0x8a242c,"\x49\x8b\x86\x88\x00\x00\x00",7},
            {0x8a275e,"\x48\x83\xc5\x50\x4d\x2b\xe7",7},
            {0x8a1f3d,"\x4d\x8b\xc7\x48\x8b\xcb",6},
            {0x8a1f56,"\x4c\x8b\xb4\x24\x30\x01\x00\x00",8},
            {0x89f21c,"\x48\x8b\x8d\xa0\x00\x00\x00",7},
        };
        for(const auto& p:particleChecks)if(!build::matches(p.at,p.bytes,p.size)){
            why="Motor particle ring/bind/lifetime fingerprint mismatch";return;
        }
        if(!build::matches(0xc4d696,"\x8b\x4c\x24\x68\x4c\x8b\x44\x24\x58",9)){
            why="Motor Hands Only native culling continuation mismatch";return;
        }
        constexpr unsigned char jointEntry[]={0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18,0x41,0x56};
        std::array<unsigned char,sizeof(jointEntry)> actualJoint{};
        if(!build::matches(0x9517f0,jointEntry,sizeof(jointEntry))){why="Motor joint selector fingerprint mismatch";return;}
        for(auto call:jointCommitCallsites){
            std::array<unsigned char,5> bytes{};int32_t displacement{};
            if(!read(base+build::rva(call),bytes.data(),bytes.size())||bytes[0]!=0xe8){why="Motor joint callsite mismatch";return;}
            std::memcpy(&displacement,bytes.data()+1,4);
            if(int64_t(build::rva(call))+5+displacement!=int64_t(build::rva(0x9517f0))){why="Motor joint call target mismatch";return;}
        }
        HMODULE self=nullptr;
        if(!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,reinterpret_cast<LPCWSTR>(&installObservation),&self)){why="cannot pin Motor boundary observer";return;}
        auto jointHook=safetyhook::InlineHook::create(reinterpret_cast<void*>(base+build::rva(0x9517f0)),reinterpret_cast<void*>(&pairedJoints),safetyhook::InlineHook::StartDisabled);
        if(!jointHook){why="Motor paired joint hook creation failed";return;}
        nativePickJoints=jointHook->original<PickJoints>();
        if(!jointHook->enable()){why="Motor paired joint hook enable failed";return;}
        new safetyhook::InlineHook(std::move(*jointHook));
        circlevr::vkcore::logf("motor-joint-pair: native selector 9517F0 hooked; six validated model commit callers; same simulation pose/history for both eyes including stereo cinema");
        for(const auto& p:points){auto hook=safetyhook::MidHook::create(reinterpret_cast<void*>(base+build::rva(p.rva)),p.callback);if(!hook){why="Motor boundary hook installation failed";return;}new safetyhook::MidHook(std::move(*hook));}
        circlevr::vkcore::SeqEngine engine;
        engine.layout={0x158,0x160,0x164,0x1b8,0x2b04};
        engine.slot=reinterpret_cast<void**>(base+build::rva(0x3ea09e0));
        engine.frameEnd=reinterpret_cast<const std::byte*>(base+build::rva(0xc98240));
        engine.renderOne=reinterpret_cast<const std::byte*>(base+build::rva(0xc978a0));
        engine.renderSystem=reinterpret_cast<std::byte*>(base+build::rva(0x4a717f0));
        // The counter belongs to the submitted backend context, not rs+11A8.
        engine.backend=reinterpret_cast<std::byte* const*>(base+build::rva(0x4a729b8));
        engine.allowsNativeTouch=allowsTouch;engine.validateReentry=reentryValidated;engine.engineSpecificFixes=false;
        engine.completedReentry=completed;
        circlevr::vkcore::SeqHookSettings settings;settings.prevMatrices=false;
        adapterInstalled=true;
        if(!circlevr::vkcore::installSeqHooksForEngine(settings,engine)){adapterInstalled=false;why="CircleVR scheduler refused Motor adapter";return;}
        locomotion::installProbe();
        locomotion::installInput();
        circlevr::vkcore::logf("motor-particle-pair: Motor native ring/stamps/GPU binding verified; paired CPU particles/effects ON; GPU draw+atlas bindings retained per eye; particle-pair-sync.txt permits live comparison");
        ok=true;circlevr::vkcore::logf("motor-seq: CircleVR FrameEnd scheduler installed with Motor layout; native re-entry C978A0(rs,arg,frameInfo,flag) gated by menu lifecycle, FPS ownership and unchanged frame token/camera; cinema fallback, no AER");
    });error=why;return ok;
}
void observePresent(const circlevr::stereo_seq::PresentMatch& match) {
    if(!adapterInstalled)return;
    if(match.tagged)based=true;
    static std::atomic<unsigned> frames{0};const auto n=++frames;
    if(n<=20 || n%120==0){const auto c=circlevr::vkcore::seqCounters();circlevr::vkcore::logf("motor-seq-present: n %u backend %u frontend %u matched %d tag %u sourceRender %u queueMatched %llu untagged %llu desync %llu/%llu guard %u",
        n,c.backendFrames,c.renderFrames,match.tagged?1:0,match.tag.backendFrame,match.tag.renderFrame,c.tags.matched,c.tags.untagged,c.tags.missingPresent,c.tags.untaggedFrame,
        field<unsigned char>(base+build::rva(0x4a71948)));
        if(match.tagged && match.tag.eye!=circlevr::stereo_seq::Eye::Mono)circlevr::vkcore::logf("motor-seq-reentry: present %s token %llu backend %u render %u paired native pass (see camera record)",circlevr::stereo_seq::eyeName(match.tag.eye),match.tag.tick,match.tag.backendFrame,match.tag.renderFrame);
    }
}
bool pairEyes(std::uint64_t token,std::uint32_t rightRender,std::array<experimental::LiveEye,2>& eyes){
    std::lock_guard lock(proofMutex);
    // A paused simulation can keep its token while new render pairs complete.
    // Never take the oldest ring entry (possibly still immersive) for that token.
    for(unsigned age=0;age<savedPairs.size()&&age<savedIndex;++age){
        const auto& saved=savedPairs[(savedIndex-1-age)%savedPairs.size()];
        if(token&&saved.token==token&&saved.eyes[1].sequence==rightRender){eyes=saved.eyes;return true;}
    }
    return false;
}
bool ledgeCamera(std::array<float,12>& camera,bool nativeReady,bool assessHandoff){
    const auto n=locomotion::latestNative();const auto now=GetTickCount64();
    static std::mutex ledgeMutex;static uint64_t owner{},lastHang{};static bool wasWhipSwing{};static locomotion::Vec headOffset{};
    static locomotion::TraversalHandoff handoff;
    std::lock_guard stateLock(ledgeMutex);
    // Measured hanging and whip traversal states retain an inactive FPS camera.
    // Reuse the same render-only path, including local body/head/hair hiding.
    const bool hanging=locomotion::nativeLedgeHang(n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace)||
        locomotion::nativeWhipClimb(n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace)||
        locomotion::nativeWhipSwing(n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace)||
        locomotion::nativeLadder(n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace)||
        locomotion::nativeRopeSlide(n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace)||
        locomotion::nativeGripCamera(n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace);
    if(!requested()||stopped||menuState!=0||experimental::pauseMenuActive()||locomotion::flatInteractionActive()||!n.valid||now-n.tick>=100){owner=lastHang=0;handoff.reset();return false;}
    Fps f;{std::lock_guard lock(controlMutex);f=fps;}
    const bool whipPrompt=n.whipPrompt;
    const bool finishing=owner==n.entity&&lastHang&&now-lastHang<1500&&n.explicitReason==0&&!n.localSpace&&!n.animated&&
        ((n.movementMode==3&&n.movementType==5)||(n.movementMode==0&&(n.movementType==0||n.movementType==5))||
         (wasWhipSwing&&locomotion::whipSwingRelease(n.movementMode,n.movementType)));
    const auto traceExit=[&](unsigned why){
        static unsigned previousWhy=~0u;static uint64_t previousOwner{};
        if(why==previousWhy&&previousOwner==n.entity)return;
        previousWhy=why;previousOwner=n.entity;
        circlevr::vkcore::logf("motor-traversal-camera-edge: reason %u owner %llx retained %llx sinceHang %llu mode %u type %d explicit %d animated %d local %d hanging %d finishing %d fpsAllowed %d fpsActive %d blend %.4f fpsAge %llu eye %.4f %.4f %.4f body %.4f %.4f %.4f",why,n.entity,owner,lastHang?now-lastHang:0,n.movementMode,n.movementType,n.explicitReason,n.animated,n.localSpace,hanging,finishing,f.allowed,f.active,f.blend,now-f.ticks,f.origin[0],f.origin[1],f.origin[2],n.position.x,n.position.y,n.position.z);
    };
    if(hanging||whipPrompt||!finishing)handoff.reset();
    const bool handoffReady=!hanging&&!whipPrompt&&finishing&&assessHandoff&&
        handoff.update(now,f.component&&now-f.ticks<100&&f.allowed&&nativeReady);
    if(!hanging&&!whipPrompt&&(!finishing||handoffReady)){traceExit(handoffReady?1:2);owner=lastHang=0;handoff.reset();return false;}
    locomotion::Vec eye{f.origin[0],f.origin[1],f.origin[2]};
    const bool blending=owner==n.entity&&lastHang&&f.active&&std::isfinite(f.blend)&&f.blend<.999f;
    if(blending)eye=locomotion::ledgeBlendAnchor(n.position,headOffset,n.angles.y);
    const auto offset=eye-n.position;
    // Native pull-up temporarily lifts the head ahead of the physics origin.
    // Continue only a previously accepted ledge camera, with bounded fresh data.
    const bool plausibleAnchor=locomotion::ledgeEyePosition(eye,n.position)||
        (owner==n.entity&&lastHang&&now-lastHang<1500&&locomotion::finite(eye)&&std::hypot(offset.x,offset.y)<1.5f&&offset.z>.1f&&offset.z<3.5f);
    if(!f.component||now-f.ticks>=100||!plausibleAnchor||!std::isfinite(n.angles.y)){handoff.reset();traceExit(!plausibleAnchor?3:4);return false;}
    traceExit(0);
    if(hanging||whipPrompt){
        if(!blending)headOffset=locomotion::rotateWorldYaw(eye-n.position,-n.angles.y);
        if(hanging)wasWhipSwing=n.movementMode==12;
        owner=n.entity;lastHang=now;
    }
    camera={};camera[0]=eye.x;camera[1]=eye.y;camera[2]=eye.z;
    locomotion::levelGameplayCamera(camera,n.angles.y);
    return true;
}
bool projectionAllowed(){return requested() && !stopped && inGameplay && !inCinema;}
bool binocularHudActive(){return freshBinocularHud(binocularHudTick.load(),GetTickCount64(),projectionAllowed(),menuActive());}
bool nativeMainMenuActive(){const auto m=menuObject.load();return requested()&&menuState==1&&m&&field<std::uintptr_t>(m)==base+build::rva(0x37dd838)&&(field<unsigned char>(m+0xa5)||field<unsigned char>(m+0xa7));}
bool menuActive(){
 if(!requested())return false;
 if(nativeMainMenuActive()||experimental::pauseMenuActive())return true;
 Fps f;{std::lock_guard lock(controlMutex);f=fps;}
 // Title child pages hide their root; native UI cursor remains active.
 return locomotion::nativeMenuCursorActive()&&(!f.allowed||GetTickCount64()-f.ticks>=150);
}
bool allowsRenderSize(){return requested() && adapterInstalled && immersive();}

}

namespace circle::motor::sequential {
namespace {
void (*guiRasterCallback)(std::uintptr_t&) = nullptr;
void (*guiComputeCallback)(std::uintptr_t&) = nullptr;
void guiRasterHook(safetyhook::Context& r) { if(guiRasterCallback)guiRasterCallback(r.rax); }
void guiComputeHook(safetyhook::Context& r) { if(guiComputeCallback)guiComputeCallback(r.rcx); }
}
bool installGuiCompositeHooks(void (*raster)(std::uintptr_t&), void (*compute)(std::uintptr_t&)) {
    if(!adapterInstalled || !base || !raster || !compute ||
       !build::matches(0xcbd31b,"\x48\x89\x45\xa0",4) ||
       !build::matches(0xd0b67a,"\x48\x8b\x82\x70\x03\x00\x00",7)) return false;
    auto a=safetyhook::MidHook::create(reinterpret_cast<void*>(base+build::rva(0xcbd31b)),guiRasterHook);
    auto b=safetyhook::MidHook::create(reinterpret_cast<void*>(base+build::rva(0xd0b67a)),guiComputeHook);
    if(!a||!b)return false;
    guiRasterCallback=raster;guiComputeCallback=compute;
    new safetyhook::MidHook(std::move(*a));new safetyhook::MidHook(std::move(*b));
    return true;
}
}
