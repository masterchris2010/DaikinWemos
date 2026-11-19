#pragma once

// Included after the existing S21 and settings helpers in the sketch.
namespace MultiDaikin {
constexpr uint16_t Port = 42162;
constexpr uint32_t Magic = 0x324d5744;
constexpr int SettingsAddress = 1400;
struct Settings {
  uint32_t magic;
  uint8_t key[32];
  uint32_t manual[Peers];
  uint8_t enabled, paired, spare[2];
  uint32_t checksum;
};
static_assert(DEVICE_NAME_ADDRESS + sizeof(DeviceNameSettings) <= SettingsAddress, "Multi settings overlap");
static_assert(SettingsAddress + sizeof(Settings) <= 2048, "Multi settings exceed EEPROM");
struct Message {
  uint64_t token, challenge;
  uint32_t magic, source, destination, digest, clock;
  uint16_t size, result;
  uint8_t kind, protocol, spare[2];
  uint8_t data[sizeof(Plan)];
  uint8_t mac[16];
};
static_assert(sizeof(Message) <= 368, "Keep datagrams small");
struct Store {
  uint32_t magic;
  uint8_t network[16];
  Plan plans[Records];
  uint32_t checksum;
};
struct RunMarker {
  uint32_t magic, day, minute, creator, id, step, checksum;
};
struct Job {
  uint64_t token, challenge;
  uint32_t id, next, started;
  uint16_t result;
  Operation op;
};
static Settings config{};
static Store *store=nullptr;
static RunMarker lastRun{};
static WiFiUDP udp;
static Peer peers[Peers]{};
static Job jobs[Peers+1]{};
static Message incoming{}, outgoing{};
static uint32_t durableDigest, batchId, self, probeDue, syncDue, persistDue, runDue, boundIp;
static uint16_t storageError;
static uint8_t syncPeer, syncRecord, jobIndex;
static uint64_t probeToken;
static bool listening, dirty;
static String scheduleStatus;
static const char *const storePath = "/multi-plans.bin";
static const char *const runPath = "/multi-run.bin";

const char *hostname() {
  static char name[24];
  if(!config.enabled) return "daikin-wemos";
  snprintf(name,sizeof(name),"daikin-wemos-%08x",unsigned(self));
  return name;
}

uint64_t nonce() {
  uint64_t value=0;
  ESP.random(reinterpret_cast<uint8_t *>(&value), sizeof(value));
  return value ? value : 1;
}

void mac(const void *data,size_t length,uint8_t *tag) {
  br_hmac_key_context key;
  br_hmac_context context;
  br_hmac_key_init(&key,&br_sha256_vtable,config.key,sizeof(config.key));
  br_hmac_init(&context,&key,16);
  br_hmac_update(&context,data,length);
  br_hmac_out(&context,tag);
}

bool sameTag(const uint8_t *a,const uint8_t *b) {
  uint8_t diff=0;
  for(unsigned i=0;i<16;++i) diff|=a[i]^b[i];
  return diff==0;
}

bool localIp(IPAddress ip) {
  const IPAddress mask=WiFi.subnetMask(), local=WiFi.localIP();
  if(!uint32_t(mask) || ip[0]==0 || ip[0]==127 || ip[0]>=224 || ip==local) return false;
  const uint32_t host=uint32_t(ip)&~uint32_t(mask);
  return (uint32_t(ip)&uint32_t(mask))==(uint32_t(local)&uint32_t(mask)) && host && host!=~uint32_t(mask);
}

template<class T> bool readRecord(const char *path,T &record) {
  File file=LittleFS.open(path,"r");
  return file && file.size()==sizeof(record) && file.read(reinterpret_cast<uint8_t *>(&record),sizeof(record))==sizeof(record) &&
      record.checksum==configChecksum(&record,offsetof(T,checksum));
}

template<class T> bool writeRecord(const char *path,T &record) {
  if(!archiveReady) return false;
  record.checksum=configChecksum(&record,offsetof(T,checksum));
  File file=LittleFS.open("/multi-write.tmp","w");
  if(!file) return false;
  bool ok=file.write(reinterpret_cast<const uint8_t *>(&record),sizeof(record))==sizeof(record);
  file.flush();
  ok=ok && file.size()==sizeof(record);
  file.close();
  return ok && LittleFS.rename("/multi-write.tmp",path);
}

void networkTag(uint8_t *value) {
  const char label[]="DaikinWemos schedule network v1";
  mac(label,sizeof(label),value);
}

uint32_t digest();

bool loadPlans() {
  if(!store) store=new(std::nothrow) Store;
  if(!store) {storageError=507;return false;}
  uint8_t tag[16];
  networkTag(tag);
  if(!config.paired || !archiveReady || !readRecord(storePath,*store) ||
     store->magic!=Magic || !sameTag(store->network,tag)) {
    memset(store,0,sizeof(*store));
    store->magic=Magic;
    memcpy(store->network,tag,sizeof(tag));
  }
  for(auto &plan:store->plans) if(plan.creator && !validPlan(plan)) plan={};
  dirty=false;
  durableDigest=digest();
  storageError=0;
  return true;
}

void begin() {
  uint8_t address[6];
  WiFi.macAddress(address);
  self=configChecksum(address,sizeof(address));
  if(!self) self=ESP.getChipId()|1;
  EEPROM.get(SettingsAddress,config);
  if(config.magic!=Magic || config.enabled>1 || config.paired>1 ||
      config.checksum!=configChecksum(&config,offsetof(Settings,checksum))) config={};
  if(config.enabled && config.paired) loadPlans();
  if(!archiveReady || !readRecord(runPath,lastRun) || lastRun.magic!=Magic) lastRun={};
}

uint32_t digest() {
  uint32_t hash=0;
  if(!store) return hash;
  for(const auto &plan:store->plans) if(plan.creator) hash^=configChecksum(&plan,sizeof(plan));
  return hash;
}

int16_t tenths(float v) {
  return isfinite(v)&&v>-100&&v<100 ? int16_t(lroundf(v*10)) : INT16_MIN;
}

Climate climate() {
  Climate c{};
  deviceName.toCharArray(c.name,sizeof(c.name));
  memcpy(c.version,"2.16",5);
  c.available=ac.known && uint32_t(millis()-ac.basicRead)<12000;
  c.room= c.available ? tenths(ac.room) : INT16_MIN;
  c.target=c.available ? tenths(ac.target-(ac.mode=='4'?heatOffsetSteps*.5f:0)) : INT16_MIN;
  c.power=ac.power; c.mode=ac.mode; c.fan=ac.fan;
  c.swing=(ac.swingKnown?4:0)|(ac.swingV?1:0)|(ac.swingH?2:0);
  c.turbo=ac.turbo; c.eco=ac.eco; c.timer=sleepSeconds();
  return c;
}

void sendMessage(IPAddress ip,Kind kind,uint32_t destination,uint64_t token,uint64_t challenge,
                 const void *data=nullptr,size_t size=0,uint16_t result=0) {
  if(!listening || size>sizeof(outgoing.data)) return;
  outgoing={};
  outgoing.magic=Magic; outgoing.protocol=1; outgoing.kind=kind; outgoing.source=self;
  outgoing.destination=destination; outgoing.token=token; outgoing.challenge=challenge;
  outgoing.size=size; outgoing.result=result;
  if(kind==Command) outgoing.spare[0]=1; // Request a state snapshot with the acknowledgement.
  if(kind==Status) {
    outgoing.digest=durableDigest;
    const time_t now=time(nullptr);
    if(now>=1704067200 && uint64_t(now)<4102444800ULL) outgoing.clock=uint32_t(now);
  }
  if(size) memcpy(outgoing.data,data,size);
  const size_t length=offsetof(Message,data)+size;
  uint8_t tag[16]{};
  if(kind!=Discover) mac(&outgoing,length,tag);
  if(udp.beginPacket(ip,Port)) {
    udp.write(reinterpret_cast<const uint8_t *>(&outgoing),length);
    udp.write(tag,sizeof(tag));
    udp.endPacket();
  }
}

uint16_t execute(Operation op) {
  if(!validOperation(op)) return 400;
  if(op.action==Timer && !op.minutes) {sleepTimerActive=false;sleepResult="";return 200;}
  if(!queryBasic()) return 503;
  if(op.action==Timer) {
    if(!ac.power) return 409;
    sleepDeadline=millis()+uint32_t(op.minutes)*60000UL;
    nextSleepAttempt=sleepDeadline;
    sleepTimerActive=true; sleepResult="";
    return 200;
  }
  if(op.action==Swing) {
    if(!querySwing()) return 503;
    const bool v=op.value&1, h=op.value&2;
    if(ac.swingV==v && ac.swingH==h) return 200;
    if(!sendSwing(v,h)) return 502;
    delay(120);
    return querySwing() && ac.swingV==v && ac.swingH==h ? 200 : 409;
  }
  if(op.action==Turbo || op.action==Eco) {
    const bool turbo=op.action==Turbo;
    const char command=turbo?'6':'7';
    uint8_t bytes[4];
    if(op.value && (!ac.power || (!turbo && ac.mode=='6'))) return 409;
    if(!readSpecial(command,bytes)) return 503;
    if((turbo?ac.turbo:ac.eco)==op.value) return 200;
    if(turbo) bytes[0]=(bytes[0]&~2)|(op.value?2:0);
    else { bytes[1]=op.value?'2':'0';bytes[2]=bytes[3]='0'; }
    size_t unused=0;
    if(!exchangeS21('D',command,bytes,sizeof(bytes),false,nullptr,unused)) return 502;
    delay(150);
    return readSpecial(command,bytes) && (turbo?ac.turbo:ac.eco)==op.value ? 200 : 409;
  }
  bool power=ac.power;
  char mode=ac.mode, fan=ac.fan;
  uint8_t raw=ac.rawSetpoint;
  if(op.action==Basic) {
    power=op.value;
    mode=op.extra;
    if(op.reserved) fan=op.reserved;
    if(mode!='2' && mode!='6') raw=uint8_t(op.minutes+(mode=='4'?heatOffsetSteps:0)+28);
  }
  if(op.action==Power) power=op.value;
  if(op.action==Fan) fan=op.value;
  if(op.action==Mode) {
    mode=op.value;
    if(mode!=ac.mode && (mode!='2' && mode!='6')) {
      int requested=isfinite(ac.target)?lroundf(ac.target*2)-(ac.mode=='4'?heatOffsetSteps:0):48;
      requested=constrain(requested,mode=='4'?32:36,60);
      raw=uint8_t(requested+(mode=='4'?heatOffsetSteps:0)+28);
    }
  }
  if(op.action==Temperature) {
    if(mode=='2'||mode=='6'||(mode!='4'&&op.value<36)) return 409;
    raw=uint8_t(op.value+(mode=='4'?heatOffsetSteps:0)+28);
  }
  if(op.action==Scheduled) {
    power=op.value!='0';
    if(power){mode=op.value;raw=uint8_t(op.extra+(mode=='4'?heatOffsetSteps:0)+28);}
  }
  if(power==ac.power && mode==ac.mode && raw==ac.rawSetpoint && fan==ac.fan) return 200;
  if(!sendBasic(power,mode,raw,fan)) return 502;
  delay(120);
  if(!queryBasic()) return 202;
  return ac.power==power && ac.mode==mode && ac.fan==fan &&
    (mode=='2'||mode=='6'||ac.rawSetpoint==raw) ? 200 : 409;
}

bool applyClock(uint32_t stamp) {
  if(stamp<1704067200 || stamp>=4102444800UL) return false;
  const timeval clockValue={time_t(stamp),0};
  return settimeofday(&clockValue,nullptr)==0;
}

void receive(uint32_t now) {
  const int length=udp.parsePacket();
  if(!length) return;
  const IPAddress from=udp.remoteIP();
  constexpr size_t prefix=offsetof(Message,data);
  incoming={};
  if(length<int(prefix+16) || length>int(prefix+sizeof(incoming.data)+16) || !localIp(from) ||
     udp.remotePort()!=Port || udp.read(reinterpret_cast<uint8_t *>(&incoming),length)!=length) {udp.flush();return;}
  const auto &m=incoming;
  if(m.magic!=Magic || m.protocol!=1 || !m.source || m.source==self ||
      (m.destination && m.destination!=self) || m.size>sizeof(m.data) || length!=int(prefix+m.size+16)) return;
  const bool discovery=m.kind==Discover;
  if(!discovery) {
    if(!config.paired) return;
    uint8_t tag[16];
    mac(&m,prefix+m.size,tag);
    if(!sameTag(tag,reinterpret_cast<const uint8_t *>(&m)+prefix+m.size)) return;
  }
  if(m.kind==Status && (m.destination!=self || m.token!=probeToken || !m.challenge || m.size!=sizeof(Climate))) return;
  if(discovery && m.size!=sizeof(Climate)) return;
  if(m.kind!=Discover && m.kind!=Probe && m.destination!=self) return;
  if((m.kind==Probe && (m.size || !m.token)) ||
     (m.kind==Command && m.size!=sizeof(Operation)) ||
     (m.kind==Record && m.size!=sizeof(Plan)) ||
     (m.kind==Ack && m.size && m.size!=sizeof(Climate)) || m.kind<Discover || m.kind>Record) return;
  Peer *p=peer(peers,self,m.source,now,!discovery);
  if(!p) return;
  if(discovery && fresh(now,p->authenticated,ForgetMs)) return;
  if(m.kind==Discover || m.kind==Status) {
    Climate c{};
    memcpy(&c,m.data,sizeof(c)); c.name[48]=0;c.version[7]=0;
    p->climate=c;
    p->seen=now;p->ip=uint32_t(from);
    if(!discovery) {
      p->authenticated=now; p->remoteChallenge=m.challenge;
      p->digest=m.digest;p->syncError=m.result;
      if(time(nullptr)<1704067200 && m.clock) applyClock(m.clock);
    }
  } else if(m.kind==Probe && !m.size && m.token) {
    if(!p->challenge || !fresh(now,p->issued)) {p->challenge=nonce();p->issued=now?now:1;}
    p->seen=now;
    if(!fresh(now,p->authenticated)) p->ip=uint32_t(from);
    const auto c=climate();
    sendMessage(from,Status,m.source,m.token,p->challenge,&c,sizeof(c),storageError);
  } else if(m.kind==Command && m.size==sizeof(Operation)) {
    Operation op{};
    memcpy(&op,m.data,sizeof(op));
    uint16_t result=authorize(*p,m.token,m.challenge,now);
    if(!result) {
      p->challenge=nonce();p->issued=now?now:1;
      result=op.action==Scheduled?400:execute(op);
      p->lastOperation=m.token;p->result=result;
    }
    const auto current=climate();
    sendMessage(from,Ack,m.source,m.token,p->challenge,
                m.spare[0]?&current:nullptr,m.spare[0]?sizeof(current):0,result);
  } else if(m.kind==Ack) {
    for(auto &j:jobs) if(j.id==m.source && j.token==m.token && !j.result) {
      j.result=m.result?m.result:502;
      p->remoteChallenge=m.challenge;
      if(m.size==sizeof(Climate)) {
        memcpy(&p->climate,m.data,sizeof(Climate));
        p->climate.name[48]=0;
        p->climate.version[7]=0;
        p->seen=p->authenticated=now;
        p->ip=uint32_t(from);
      }
      probeDue=now;
    }
  } else if(m.kind==Record && m.size==sizeof(Plan) && archiveReady && store) {
    Plan plan{};
    memcpy(&plan,m.data,sizeof(plan));
    const auto result=merge(store->plans,plan);
    if(result==Full) storageError=507;
    if(result==Invalid) storageError=422;
    if(result==Applied) {if(!dirty) persistDue=now+2000; dirty=true;}
  } else if(m.kind==Record) storageError=503;
}

void tickJobs(uint32_t now) {
  auto &job=jobs[jobIndex++%(Peers+1)];
  if(!job.id || job.result) return;
  if(job.id==self) {job.result=execute(job.op);return;}
  if(uint32_t(now-job.started)>=5000) {job.result=504;return;}
  if(int32_t(now-job.next)<0) return;
  job.next=now+800;
  Peer *p=nullptr;
  for(auto &item:peers) if(item.id==job.id) {p=&item;break;}
  if(!p || !fresh(now,p->authenticated) || !localIp(IPAddress(p->ip))) {job.result=503;return;}
  sendMessage(IPAddress(p->ip),Command,job.id,job.token,job.challenge,&job.op,sizeof(job.op));
}

bool flushPlans() {
  if(!dirty) return true;
  if(!store || !writeRecord(storePath,*store)) {storageError=500;return false;}
  dirty=false;
  durableDigest=digest();
  if(storageError==500)storageError=0;
  return true;
}

void tick() {
  const uint32_t now=millis();
  if(dirty && int32_t(now-persistDue)>=0) {
    if(!flushPlans()) persistDue=now+15000;
  }
  if(!config.enabled || WiFi.status()!=WL_CONNECTED) {
    if(listening) {udp.stop();listening=false;for(auto &p:peers)p.authenticated=0;}
    for(auto &job:jobs) if(job.id&&!job.result) job.result=503;
    return;
  }
  const uint32_t ip=uint32_t(WiFi.localIP());
  if(listening && ip!=boundIp) {udp.stop();listening=false;for(auto &p:peers)p.authenticated=0;}
  if(!listening) {
    if(int32_t(now-probeDue)<0) return;
    probeDue=now+2000;
    if(!udp.begin(Port)) return;
    listening=true;boundIp=ip;probeDue=now;
  }
  receive(now);
  tickJobs(now);
  if(int32_t(now-probeDue)>=0) {
    probeDue=now+12000;
    Climate c{};
    deviceName.toCharArray(c.name,sizeof(c.name));
    memcpy(c.version,"2.16",5);
    c.room=c.target=INT16_MIN;
    c.turbo=c.eco=-1;
    probeToken=nonce();
    const IPAddress broadcast(ip|~uint32_t(WiFi.subnetMask()));
    sendMessage(broadcast,Discover,0,0,0,&c,sizeof(c));
    if(config.paired) sendMessage(broadcast,Probe,0,probeToken,0);
    for(const auto manual:config.manual) if(manual && localIp(IPAddress(manual))) {
      sendMessage(IPAddress(manual),Discover,0,0,0,&c,sizeof(c));
      if(config.paired) sendMessage(IPAddress(manual),Probe,0,probeToken,0);
    }
    // A configured unicast address also works when broadcast is filtered.
    if(config.paired) for(const auto &p:peers) {
      if(p.id && fresh(now,p.seen,ForgetMs) && localIp(IPAddress(p.ip)))
        sendMessage(IPAddress(p.ip),Probe,p.id,probeToken,0);
    }
  }
  if(config.paired && store && !dirty && int32_t(now-syncDue)>=0) {
    syncDue=now+250;
    // One bounded packet per tick. Equal digests suppress record traffic.
    auto &p=peers[syncPeer];
    const auto &plan=store->plans[syncRecord];
    if(fresh(now,p.authenticated) && p.digest!=digest() && plan.creator && localIp(IPAddress(p.ip)))
      sendMessage(IPAddress(p.ip),Record,p.id,0,0,&plan,sizeof(plan));
    if(++syncPeer==Peers) {syncPeer=0;syncRecord=(syncRecord+1)%Records;}
  }
}

void tickShared() {
  if(!config.enabled || !config.paired || !archiveReady || !store || dirty || int32_t(millis()-runDue)<0) return;
  runDue=millis()+2000;
  struct tm local;
  if(!localClock(local)) return;
  const unsigned minute=local.tm_hour*60+local.tm_min, days=1U<<local.tm_wday;
  const uint32_t day=uint32_t(local.tm_year)*366+local.tm_yday;
  if(lastRun.day>day || (lastRun.day==day && lastRun.minute>=minute)) return;
  for(const auto &rule:schedule.rules)
    if(rule.enabled && rule.minute==minute && (rule.days&days)) return;
  const Plan *winner=nullptr;
  unsigned step=0;
  for(const auto &plan:store->plans) if(plan.creator && plan.enabled && !plan.deleted && (plan.days&days)) {
    for(unsigned i=0;i<plan.count;++i) if(plan.steps[i].minute==minute && matches(plan.steps[i],self)) {
      if(!winner || plan.creator<winner->creator || (plan.creator==winner->creator && plan.id<winner->id)) {
        winner=&plan;step=i;
      }
    }
  }
  if(!winner) return;
  RunMarker marker={Magic,day,minute,winner->creator,winner->id,step,0};
  if(!writeRecord(runPath,marker)) {scheduleStatus="Orario condiviso non eseguito: salvataggio non riuscito";return;}
  lastRun=marker;
  const auto &action=winner->steps[step];
  const uint16_t result=execute({Scheduled,action.mode,action.half,0,0});
  scheduleStatus=String(winner->name)+(result==200?" · confermato":" · non confermato ("+String(result)+")");
}
} // namespace MultiDaikin
