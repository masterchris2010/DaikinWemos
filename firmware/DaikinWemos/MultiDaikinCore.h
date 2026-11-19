#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

namespace MultiDaikin {
constexpr unsigned Peers = 7, Records = 16, Steps = 6, Targets = 8;
constexpr uint32_t OfflineMs = 45000, ForgetMs = 300000;
enum Kind : uint8_t { Discover = 1, Probe, Status, Command, Ack, Record };
enum Action : uint8_t { Power = 1, Temperature, Mode, Fan, Swing, Turbo, Eco, Timer, Scheduled, Basic };
struct Operation { uint8_t action, value, extra, reserved; uint16_t minutes; };
struct Step { uint32_t targets[Targets]; uint16_t minute; uint8_t count, mode, half, spare[3]; };
struct Plan {
  uint32_t creator, id, revision, editor;
  char name[33];
  uint8_t days, enabled, deleted, count, spare[3];
  Step steps[Steps];
};
static_assert(sizeof(Plan) == 296, "Protocol layout");
inline bool modes(uint8_t mode) { return mode=='1'||mode=='2'||mode=='3'||mode=='4'||mode=='6'; }
inline bool validOperation(Operation op) {
  switch (op.action) {
    case Power: case Turbo: case Eco: return op.value<=1;
    case Temperature: return op.value>=32 && op.value<=60;
    case Mode: return modes(op.value);
    case Fan: return op.value=='A'||op.value=='B'||(op.value>='3'&&op.value<='7');
    case Swing: return op.value<=3;
    case Timer: return op.minutes<=1440;
    case Basic: return op.value<=1 && modes(op.extra) && op.minutes>=32 && op.minutes<=60 &&
      (!op.reserved || validOperation({Fan,op.reserved,0,0,0}));
    case Scheduled: return op.value=='0'||((op.value=='1'||op.value=='3'||op.value=='4')&&op.extra>=(op.value=='4'?32:36)&&op.extra<=60);
    default: return false;
  }
}
inline bool validPlan(const Plan &p) {
  if (!p.creator || !p.id || !p.revision || !p.editor || p.deleted > 1) return false;
  if (p.deleted) return !p.count && !p.enabled;
  if (!p.name[0] || p.name[32] || !p.days || p.days>127 || p.enabled>1 || !p.count || p.count>Steps) return false;
  for (unsigned i=0;i<p.count;++i) {
    const Step &s=p.steps[i];
    if (s.minute>=1440 || s.count>Targets || !validOperation({Scheduled,s.mode,s.half,0,0}) || (s.mode=='0' && s.half)) return false;
    for (unsigned j=0;j<s.count;++j) {
      if (!s.targets[j]) return false;
      for (unsigned k=0;k<j;++k) if (s.targets[j]==s.targets[k]) return false;
    }
  }
  return true;
}
inline bool matches(const Step &s,uint32_t id) {
  if (!s.count) return true;
  for (unsigned i=0;i<s.count;++i) if (s.targets[i]==id) return true;
  return false;
}
inline bool newer(const Plan &a,const Plan &b) {
  return a.revision>b.revision || (a.revision==b.revision &&
    (a.editor>b.editor || (a.editor==b.editor && memcmp(&a,&b,sizeof(a))>0)));
}
enum Merge { Ignored, Applied, Full, Invalid };
inline int findPlan(const Plan *plans,uint32_t creator,uint32_t id) {
  for (unsigned i=0;i<Records;++i) if(plans[i].creator==creator && plans[i].id==id) return i;
  return -1;
}
inline Merge merge(Plan *plans,const Plan &incoming) {
  if (!validPlan(incoming)) return Invalid;
  int index=findPlan(plans,incoming.creator,incoming.id);
  if(index>=0) {
    if(!newer(incoming,plans[index])) return Ignored;
  } else {
    for(unsigned i=0;i<Records;++i) if(!plans[i].creator){index=i;break;}
    if(index<0) return Full;
  }
  plans[index]=incoming;
  return Applied;
}
struct Climate {
  char name[49], version[8];
  int16_t room, target; // tenths of a degree. INT16_MIN means unavailable.
  int8_t turbo, eco;
  uint8_t power, mode, fan, swing, available, spare;
  uint32_t timer;
};
struct Peer {
  uint64_t challenge, remoteChallenge, lastOperation;
  uint32_t id, ip, seen, authenticated, digest, issued;
  uint16_t result, syncError;
  Climate climate;
};
inline bool fresh(uint32_t now,uint32_t then,uint32_t limit=OfflineMs) {
  return then && uint32_t(now-then)<limit;
}
inline Peer *peer(Peer *peers,uint32_t self,uint32_t id,uint32_t now,bool trusted) {
  if(!id || id==self) return nullptr;
  Peer *empty=nullptr, *stale=nullptr, *untrusted=nullptr;
  for(unsigned i=0;i<Peers;++i){
    auto &p=peers[i];
    if(p.id==id) return &p;
    if(!p.id && !empty) empty=&p;
    if(p.id && !fresh(now,p.seen,ForgetMs) && !stale) stale=&p;
    if(trusted && !fresh(now,p.authenticated) && !untrusted) untrusted=&p;
  }
  Peer *slot=empty?empty:stale?stale:untrusted;
  if(slot) { *slot={};slot->id=id; }
  return slot;
}
inline uint16_t authorize(Peer &p,uint64_t operation,uint64_t challenge,uint32_t now) {
  if(operation && operation==p.lastOperation) return p.result;
  return operation && challenge && challenge==p.challenge && fresh(now,p.issued) ? 0 : 403;
}
} // namespace MultiDaikin
