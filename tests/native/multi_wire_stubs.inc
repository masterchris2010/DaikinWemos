#include <ctime>
bool applyClock(uint32_t){return true;}
struct IPAddress {
  uint32_t ip;
  explicit IPAddress(uint32_t value=0): ip(value) {}
  explicit operator uint32_t() const {return ip;}
};
constexpr uint16_t Port=42162;
constexpr uint32_t Magic=0x324d5744;
constexpr uint32_t self=10;
struct {
  Message wire{};
  int length=0;
  uint32_t from=0xc0a80114;
  int parsePacket(){return length;}
  IPAddress remoteIP(){return IPAddress(from);}
  uint16_t remotePort(){return Port;}
  int read(uint8_t *p,int n){memcpy(p,&wire,n);length=0;return n;}
  void flush(){length=0;}
} udp;
Peer peers[Peers]{};
struct {bool paired=true,enabled=true;} config;
struct {Plan plans[Records]{};} backing;
auto *store=&backing;
struct Job {uint64_t token; uint32_t id;uint16_t result;};
Job jobs[Peers+1]{};
Message incoming{};
uint32_t probeDue=0,persistDue=0;
uint64_t probeToken=55;
uint16_t storageError=0, ack=0;
bool dirty=false,archiveReady=true;
bool localIp(IPAddress ip){return (uint32_t(ip)&0xffffff00)==0xc0a80100;}
uint64_t nonce(){static uint64_t next=70;return ++next;}
Climate climate(){return Climate{};}
// This stand-in isolates packet framing and dispatch. The ESP8266 build links BearSSL HMAC.
void mac(const void *buf,size_t length,uint8_t *tag){
  uint8_t checksum=33;
  const auto *p=static_cast<const uint8_t *>(buf);
  for(size_t i=0;i<length;++i) checksum=uint8_t(checksum*17+p[i]);
  memset(tag,checksum,16);
}
bool sameTag(const uint8_t *a,const uint8_t *b){return memcmp(a,b,16)==0;}
void sendMessage(IPAddress,Kind kind,uint32_t,uint64_t,uint64_t,
                 const void *,size_t,uint16_t result=0){if(kind==Ack)ack=result;}
void receive(uint32_t now);
void inject(Kind kind,uint32_t source,uint32_t destination,uint64_t token,uint64_t challenge,
            const void *data=nullptr,size_t size=0,bool signedCorrectly=true){
  auto &wire=udp.wire;
  wire={};wire.magic=Magic;wire.protocol=1;wire.kind=kind;wire.source=source;
  wire.destination=destination;wire.token=token;wire.challenge=challenge;wire.size=size;
  if(size)memcpy(wire.data,data,size);
  const size_t length=offsetof(Message,data)+size;
  if(signedCorrectly)mac(&wire,length,reinterpret_cast<uint8_t *>(&wire)+length);
  udp.length=int(length+16);
  receive(2000);
}
void usingWireTests(){
  Climate remote{};
  inject(Discover,self,0,0,0,&remote,sizeof(remote));
  assert(!peers[0].id);
  inject(Discover,20,0,0,0,&remote,sizeof(remote));
  assert(peers[0].id==20&&!peers[0].authenticated);
  inject(Status,20,self,54,88,&remote,sizeof(remote));
  assert(!peers[0].authenticated);
  inject(Status,20,self,55,88,&remote,sizeof(remote),false);
  assert(!peers[0].authenticated);
  inject(Status,20,self,55,88,&remote,sizeof(remote));
  assert(peers[0].authenticated&&peers[0].remoteChallenge==88);
  udp.from=0xc0a80115;
  inject(Discover,20,0,0,0,&remote,sizeof(remote));
  assert(peers[0].ip==0xc0a80114);
  inject(Status,20,self,55,99,&remote,sizeof(remote));
  assert(peers[0].ip==0xc0a80115);
  inject(Probe,20,self,56,0);
  const auto challenge=peers[0].challenge;
  const Operation off={Power,0,0,0,0};
  ac.power=true;
  inject(Command,20,self,11,challenge,&off,sizeof(off),false);
  assert(ac.power&&ack==0);
  inject(Command,20,self,11,challenge,&off,sizeof(off));
  assert(!ac.power&&ack==200);
  ac.power=true; // Remote retry must not overwrite a subsequent manual change.
  inject(Command,20,self,11,challenge,&off,sizeof(off));
  assert(ac.power&&ack==200);
  inject(Command,20,self,12,challenge,&off,sizeof(off));
  assert(ac.power&&ack==403);
  jobs[0]={500,20,0};
  inject(Ack,20,self,499,88);
  assert(!jobs[0].result);
}
