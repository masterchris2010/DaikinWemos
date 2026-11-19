#include "MultiDaikinCore.h"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <algorithm>
#include <ctime>
#include <string>
using namespace MultiDaikin;

struct {
  bool power=true;
  char mode='4', fan='A';
  uint8_t rawSetpoint=72;
  float target=22;
  bool swingV=false, swingH=false;
  int turbo=0, eco=0;
} ac;
bool basicOk=true, writeOk=true, echoWrite=true, readAfter=true;
bool sleepTimerActive=false;
uint32_t sleepDeadline, nextSleepAttempt;
const char *sleepResult;
int heatOffsetSteps=0;
uint32_t nowMs=100;
uint32_t millis(){return nowMs;}
bool queryBasic(){return basicOk;}
bool sendBasic(bool power,char mode,uint8_t raw,char fan) {
  if(!writeOk)return false;
  if(echoWrite){ac.power=power;ac.mode=mode;ac.rawSetpoint=raw;ac.fan=fan;}
  basicOk=readAfter;
  return true;
}
bool querySwing(){return true;}
bool sendSwing(bool, bool){return writeOk;}
bool readSpecial(char,uint8_t (&)[4]){return true;}
bool exchangeS21(char,char,uint8_t *,size_t,bool,void*,size_t &){return writeOk;}
void delay(int){}
using std::isfinite;
int constrain(int n,int a,int b){return std::min(b,std::max(a,n));}
#include "multi_execute.inc"
#include "multi_wire.inc"
struct String : std::string {
  using std::string::string;
  explicit String(uint16_t value):std::string(std::to_string(value)){}
};
struct RunMarker {uint32_t magic, day, minute, creator, id, step, checksum;};
RunMarker lastRun{}, persistedRun{};
const char *runPath="/multi-run.bin";
bool diskOk=true,clockOk=true;
uint32_t runDue=0;
std::string scheduleStatus;
struct LocalRule { bool enabled; unsigned minute,days; };
struct { LocalRule rules[8]; } schedule{};
bool writeRecord(const char *,RunMarker &marker){
  if(diskOk)persistedRun=marker;
  return diskOk;
}
tm clockValue{};
bool localClock(tm &result){result=clockValue;return clockOk;}
#include "multi_schedule.inc"

Plan plan() {
  Plan value{};
  value.creator=1;value.id=9;value.editor=1;value.revision=1;
  memcpy(value.name,"Mattina",8);
  value.days=62;value.enabled=1;value.count=2;
  value.steps[0].minute=390;
  value.steps[0].mode='4';value.steps[0].half=42;
  value.steps[0].count=1;value.steps[0].targets[0]=2;
  value.steps[1].minute=510;value.steps[1].mode='0';
  return value;
}

int main() {
  Peer peers[Peers]{};
  assert(!peer(peers,1,1,100,false)); // self-discovery
  auto p=peer(peers,1,2,100,false);
  assert(p && p->id==2 && !p->authenticated);
  p->ip=10;p->seen=100;
  auto moved=peer(peers,1,2,200,true);
  assert(p==moved);moved->ip=20;moved->seen=200;
  assert(!fresh(45201,moved->seen) && fresh(45201,moved->seen,ForgetMs));
  assert(!fresh(300201,moved->seen,ForgetMs));
  assert(authorize(*p,3,9,200)==403);
  p->challenge=9;p->issued=200;
  assert(authorize(*p,3,9,200)==0);
  assert(authorize(*p,4,9,45201)==403);
  p->lastOperation=3;p->result=409;p->challenge=8;p->issued=201;
  assert(authorize(*p,3,9,202)==409); // same operation, cached failure
  assert(authorize(*p,4,9,202)==403); // different operation, old challenge
  assert(authorize(*p,4,8,202)==0);
  p->lastOperation=4;p->result=200;p->challenge=7;
  assert(authorize(*p,3,9,202)==403); // delayed duplicate still cannot execute

  assert(execute({Power,0,0,0,0})==200 && !ac.power);
  echoWrite=false;
  assert(execute({Power,1,0,0,0})==409); // ACK alone never counts
  writeOk=false;
  assert(execute({Power,1,0,0,0})==502);
  writeOk=true;readAfter=false;
  assert(execute({Power,1,0,0,0})==202);
  assert(execute({Power,1,0,0,0})==503);
  basicOk=readAfter=echoWrite=true;
  assert(execute({Mode,'3',0,0,0})==200 && !ac.power);
  assert(execute({Temperature,32,0,0,0})==409);
  assert(execute({Swing,1,0,0,0})==409); // swing readback mismatch
  assert(execute({Scheduled,'4',42,0,0})==200 && ac.power && ac.rawSetpoint==70);
  assert(execute({Timer,0,0,0,30})==200 && sleepTimerActive);
  assert(execute({Timer,0,0,0,0})==200 && !sleepTimerActive);

  usingWireTests();

  Plan a[Records]{}, b[Records]{};
  const auto original=plan();
  assert(matches(original.steps[0],2) && !matches(original.steps[0],3));
  assert(matches(original.steps[1],3));
  assert(merge(a,original)==Applied && merge(a,original)==Ignored);
  auto left=original,right=original;
  left.revision=right.revision=2;left.editor=7;right.editor=8;
  left.steps[0].half=40;right.steps[0].half=44;
  assert(merge(a,left)==Applied && merge(a,right)==Applied);
  assert(merge(b,right)==Applied && merge(b,left)==Ignored);
  assert(memcmp(a,b,sizeof(a))==0);
  auto sameVersion=right;sameVersion.steps[0].half=46;
  assert(merge(a,sameVersion)==Applied && merge(a,right)==Ignored);
  auto deleted=right;deleted.revision=3;deleted.deleted=1;deleted.count=deleted.enabled=0;
  assert(merge(a,deleted)==Applied);
  FILE *file=tmpfile();assert(file);
  assert(fwrite(a,1,sizeof(a),file)==sizeof(a));rewind(file);
  memset(a,0,sizeof(a));
  assert(fread(a,1,sizeof(a),file)==sizeof(a));fclose(file);
  assert(merge(a,original)==Ignored && a[0].deleted);
  assert(merge(b,a[0])==Applied && b[0].deleted); // reconnect after a reboot
  auto invalid=original;invalid.steps[0].count=Targets+1;
  assert(merge(a,invalid)==Invalid);
  for(unsigned i=1;i<Records;++i){auto value=original;value.id=100+i;assert(merge(a,value)==Applied);}
  auto extra=original;extra.id=1000;assert(merge(a,extra)==Full);

  // Shared schedule uses only retained local state and the clock, without peers.
  auto localPlan=original;localPlan.steps[0].targets[0]=self;
  assert(merge(store->plans,localPlan)==Applied);
  clockValue.tm_year=126;clockValue.tm_yday=322;clockValue.tm_wday=1;
  clockValue.tm_hour=6;clockValue.tm_min=30;
  ac.power=false;nowMs=6000;
  schedule.rules[0]={true,390,2};
  tickShared();
  assert(!ac.power&&!persistedRun.magic);
  schedule.rules[0].enabled=false;
  nowMs+=2000;diskOk=false;tickShared();
  assert(!ac.power&&!persistedRun.magic);
  nowMs+=2000;diskOk=true;tickShared();
  assert(ac.power&&persistedRun.minute==390);
  lastRun=persistedRun;runDue=0;ac.power=false;nowMs+=2000;
  tickShared();
  assert(!ac.power); // Restart in the same minute must not repeat the command.
  ac.power=true;clockValue.tm_hour=8;clockValue.tm_min=30;nowMs+=2000;
  tickShared();
  assert(!ac.power&&persistedRun.minute==510);
}
