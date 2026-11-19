#pragma once

namespace MultiDaikin {
bool number(const String &text,uint32_t &value,uint32_t maximum=UINT32_MAX) {
  value=0;
  if(!text.length() || text.length()>10) return false;
  for(unsigned i=0;i<text.length();++i) {
    const unsigned digit=unsigned(text[i]-'0');
    if(digit>9 || digit>maximum || value>(maximum-digit)/10) return false;
    value=value*10+digit;
  }
  return true;
}

bool browserMutation() {
  if(server.header("X-Multi-Request")=="1" && WiFi.status()==WL_CONNECTED && localIp(server.client().remoteIP()))
    return true;
  server.send(403,"text/plain","Richiesta locale non autorizzata.");
  return false;
}

bool configured() {
  if(config.enabled && config.paired && store) return true;
  server.send(409,"text/plain","Attiva Multi-Daikin e abbina le schede.");
  return false;
}

void configure() {
  if(!browserMutation()) return;
  if(server.header("X-Firmware-Key")!=firmwareKey.value) {server.send(403,"text/plain","Chiave firmware errata.");return;}
  if(server.hasArg("clock")) {
    uint32_t stamp;
    if(!number(server.arg("clock"),stamp) || !applyClock(stamp)) {
      server.send(400,"text/plain","Data non valida.");return;
    }
    server.send(200,"text/plain","Orologio sincronizzato con il browser.");
    return;
  }
  Settings next=config;
  uint32_t enabled;
  if(!number(server.arg("enabled"),enabled,1)) {server.send(400,"text/plain","Impostazione non valida.");return;}
  next.enabled=enabled;
  const String key=server.arg("key");
  if(key.length()) {
    if(key.length()!=64) {server.send(400,"text/plain","Servono 64 caratteri esadecimali.");return;}
    for(unsigned i=0;i<32;++i) {
      const char a=tolower(static_cast<unsigned char>(key[i*2])), b=tolower(static_cast<unsigned char>(key[i*2+1]));
      auto digit=[](char c)->int {return c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:-1;};
      const int hi=digit(a),lo=digit(b);
      if(hi<0||lo<0) {server.send(400,"text/plain","Chiave Multi-Daikin non valida.");return;}
      next.key[i]=(hi<<4)|lo;
    }
    next.paired=1;
  }
  for(unsigned i=0;i<Peers;++i) {
    const String value=server.arg("ip"+String(i));
    IPAddress ip;
    if(value.length() && (!ip.fromString(value) || !localIp(ip))) {
      server.send(400,"text/plain","Gli indirizzi manuali devono appartenere alla stessa sottorete.");return;
    }
    next.manual[i]=value.length()?uint32_t(ip):0;
  }
  const bool changedKey=next.paired!=config.paired || memcmp(next.key,config.key,sizeof(next.key));
  if((changedKey || !next.enabled) && !flushPlans()) {server.send(503,"text/plain","Salvataggio programmazioni in sospeso.");return;}
  next.magic=Magic;
  next.checksum=configChecksum(&next,offsetof(Settings,checksum));
  if(memcmp(&next,&config,sizeof(next))) {
    EEPROM.put(SettingsAddress,next);
    if(!EEPROM.commit()) {
      EEPROM.put(SettingsAddress,config);
      server.send(500,"text/plain","Salvataggio impostazioni non riuscito.");return;
    }
  }
  if(config.enabled!=next.enabled) {
    if(mdnsActive) MDNS.end();
    mdnsActive=false;
    wasConnected=false;
  }
  config=next;
  WiFi.hostname(hostname());
  if(!config.enabled) {delete store;store=nullptr;dirty=false;storageError=0;}
  else if(config.paired && (changedKey || !store)) loadPlans();
  if(changedKey || !config.enabled) {
    for(auto &p:peers) p={};
    for(auto &j:jobs) j={};
  }
  probeDue=millis();
  server.send(200,"text/plain","Configurazione salvata.");
}

void addDevice(String &json,uint32_t id,IPAddress ip,const Climate &c,bool online,bool trusted,
               uint32_t age,bool synced,uint16_t syncError) {
  json+=F("{\"id\":");json+=id;
  json+=F(",\"name\":");appendJsonString(json,c.name);
  json+=F(",\"version\":");appendJsonString(json,c.version);
  json+=F(",\"ip\":");appendJsonString(json,ip.toString().c_str());
  json+=F(",\"online\":");json+=online?"true":"false";
  json+=F(",\"paired\":");json+=trusted?"true":"false";
  json+=F(",\"age\":");json+=age;
  json+=F(",\"sync\":");json+=synced?"true":"false";
  json+=F(",\"syncError\":");json+=syncError;
  json+=F(",\"available\":");json+=c.available?"true":"false";
  json+=F(",\"power\":");json+=c.power?"true":"false";
  json+=F(",\"mode\":\"");json+=modes(c.mode)?char(c.mode):'-';json+='"';
  json+=F(",\"fan\":\"");json+=validFan(c.fan)?char(c.fan):'-';json+='"';
  json+=F(",\"room\":");json+=c.room==INT16_MIN?"null":String(c.room*.1f,1);
  json+=F(",\"target\":");json+=c.target==INT16_MIN?"null":String(c.target*.1f,1);
  json+=F(",\"swing\":");json+=c.swing&7;
  json+=F(",\"turbo\":");json+=int(c.turbo);
  json+=F(",\"eco\":");json+=int(c.eco);
  json+=F(",\"timer\":");json+=c.timer;
  json+='}';
}

void devices() {
  String json;
  json.reserve(360);
  json=F("{\"enabled\":");json+=config.enabled?"true":"false";
  json+=F(",\"paired\":");json+=config.paired?"true":"false";
  json+=F(",\"ready\":");json+=archiveReady&&store?"true":"false";
  json+=F(",\"pending\":");json+=dirty?"true":"false";
  json+=F(",\"error\":");json+=storageError;
  json+=F(",\"lastSchedule\":");appendJsonString(json,scheduleStatus.c_str());
  json+=F(",\"self\":");json+=self;
  json+=F(",\"manual\":[");
  for(unsigned i=0;i<Peers;++i) {
    if(i)json+=',';
    appendJsonString(json,config.manual[i]?IPAddress(config.manual[i]).toString().c_str():"");
  }
  json+=F("],\"devices\":[");
  const auto c=climate();
  addDevice(json,self,WiFi.localIP(),c,true,config.paired,0,!dirty,storageError);
  server.sendHeader("Cache-Control","no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"application/json",json);
  const uint32_t now=millis(), sum=digest();
  for(const auto &p:peers) if(config.enabled && p.id && fresh(now,p.seen,ForgetMs)) {
    json=",";
    const bool paired=p.authenticated!=0;
    addDevice(json,p.id,IPAddress(p.ip),p.climate,fresh(now,paired?p.authenticated:p.seen),paired,
      (now-(paired?p.authenticated:p.seen))/1000,p.digest==sum&&!dirty,p.syncError);
    server.sendContent(json);
  }
  server.sendContent("]}");
  server.sendContent("");
}

void jobStatus() {
  String json=F("{\"batch\":");json+=batchId;json+=F(",\"jobs\":[");
  bool first=true;
  for(const auto &j:jobs) if(j.id) {
    if(!first) json+=',';
    first=false;
    json+=F("{\"id\":");json+=j.id;json+=F(",\"code\":");json+=j.result;json+='}';
  }
  json+=F("]}");
  server.sendHeader("Cache-Control","no-store");
  server.send(200,"application/json",json);
}

void command() {
  if(!browserMutation() || !configured()) return;
  uint32_t id, action, value, minutes=0, mode=0, fan=0;
  if(!number(server.arg("id"),id) || !number(server.arg("action"),action,Basic) ||
     !number(server.arg("value"),value,255) ||
     (server.hasArg("minutes")&&!number(server.arg("minutes"),minutes,1440))) {
    server.send(400,"text/plain","Comando non valido.");return;
  }
  if(action==Scheduled || (action==Basic &&
      (!number(server.arg("mode"),mode,255) || !number(server.arg("fan"),fan,255)))) {
    server.send(400,"text/plain","Comando non valido.");return;
  }
  Operation op={uint8_t(action),uint8_t(value),uint8_t(mode),uint8_t(fan),uint16_t(minutes)};
  if(!validOperation(op) || (!id && (action>Mode || server.arg("confirmed")!="1"))) {
    server.send(400,"text/plain","Comando non valido o conferma mancante.");return;
  }
  for(const auto &j:jobs) if(j.id && !j.result) {server.send(409,"text/plain","Comando precedente in corso.");return;}
  const uint32_t now=millis();
  bool found=id==self || !id;
  for(const auto &p:peers) if(p.id==id && p.authenticated && fresh(now,p.seen,ForgetMs)) found=true;
  if(!found) {server.send(404,"text/plain","Dispositivo non abbinato o non disponibile.");return;}
  for(auto &j:jobs)j={};
  unsigned count=0;
  if(!id || id==self) jobs[count++]={nonce(),0,self,now,now,0,op};
  for(const auto &p:peers) if(p.id && (!id||p.id==id) && p.authenticated && fresh(now,p.seen,ForgetMs)) {
    const bool online=fresh(now,p.authenticated) && p.remoteChallenge;
    jobs[count++]={nonce(),p.remoteChallenge,p.id,now,now,uint16_t(online?0:503),op};
  }
  if(++batchId==0) ++batchId;
  jobStatus();
}

void appendPlan(String &json,const Plan &p) {
  json+=F("{\"id\":");json+=p.id;
  json+=F(",\"creator\":");json+=p.creator;
  json+=F(",\"editor\":");json+=p.editor;
  json+=F(",\"revision\":");json+=p.revision;
  json+=F(",\"name\":");appendJsonString(json,p.name);
  json+=F(",\"days\":");json+=p.days;
  json+=F(",\"enabled\":");json+=p.enabled?"true":"false";
  json+=F(",\"steps\":[");
  for(unsigned i=0;i<p.count;++i) {
    const auto &s=p.steps[i];
    if(i)json+=',';
    json+=F("{\"minute\":");json+=s.minute;
    json+=F(",\"mode\":\"");json+=char(s.mode);json+='"';
    json+=F(",\"half\":");json+=s.half;
    json+=F(",\"targets\":[");
    for(unsigned j=0;j<s.count;++j){if(j)json+=',';json+=s.targets[j];}
    json+=F("]}");
  }
  json+=F("]}");
}

void plans() {
  if(!configured()) return;
  unsigned used=0;
  for(const auto &p:store->plans) if(p.creator)++used;
  String json=F("{\"used\":");json+=used;json+=F(",\"limit\":");json+=Records;json+=F(",\"plans\":[");
  server.sendHeader("Cache-Control","no-store");
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200,"application/json",json);
  bool first=true;
  for(const auto &p:store->plans) if(p.creator&&!p.deleted) {
    json=first?"":",";
    appendPlan(json,p);
    server.sendContent(json);
    first=false;
  }
  server.sendContent("]}");
  server.sendContent("");
}

bool parseStep(unsigned i,Step &step) {
  const String suffix=String(i), mode=server.arg("mode"+suffix);
  uint32_t minute,half;
  if(mode.length()!=1 || !number(server.arg("minute"+suffix),minute,1439) ||
     !number(server.arg("half"+suffix),half,60)) return false;
  step.minute=minute;step.mode=mode[0];step.half=half;
  if(!server.hasArg("targets"+suffix)) return false;
  const String ids=server.arg("targets"+suffix);
  if(ids.endsWith(",")) return false;
  unsigned start=0;
  while(start<ids.length()) {
    const int comma=ids.indexOf(',',start);
    const unsigned end=comma<0?ids.length():unsigned(comma);
    if(step.count>=Targets || !number(ids.substring(start,end),step.targets[step.count])) return false;
    ++step.count;
    start=end+1;
  }
  return true;
}

void savePlan() {
  if(!browserMutation() || !configured()) return;
  if(!archiveReady) {server.send(503,"text/plain","Inizializza LittleFS da Apprendimento.");return;}
  Plan next{};
  uint32_t version=0,editor=0;
  if(!number(server.arg("id"),next.id) || !number(server.arg("creator"),next.creator) ||
     !number(server.arg("revision"),version) || !number(server.arg("editor"),editor)) {
    server.send(400,"text/plain","Identificativo non valido.");return;
  }
  int index=findPlan(store->plans,next.creator,next.id);
  const bool deleting=server.arg("delete")=="1";
  if(!next.id && !next.creator && !deleting) {
    next.creator=self;
    do {next.id=uint32_t(nonce());} while(!next.id || findPlan(store->plans,self,next.id)>=0);
    for(unsigned i=0;i<Records;++i) if(!store->plans[i].creator){index=i;break;}
    if(index<0) {server.send(507,"text/plain","Archivio condiviso pieno, comprese le eliminazioni.");return;}
  } else if(index<0 || store->plans[index].deleted || store->plans[index].revision!=version ||
      store->plans[index].editor!=editor || version==UINT32_MAX) {
    server.send(409,"text/plain","La programmazione è cambiata. Ricarica prima di modificare.");return;
  }
  next.revision=version+1;next.editor=self;next.deleted=deleting;
  if(!deleting) {
    String name=server.arg("name");
    name.trim();
    uint32_t days,count,enabled;
    if(!name.length()||name.length()>32||!number(server.arg("days"),days,127)||
        !number(server.arg("count"),count,Steps)||!number(server.arg("enabled"),enabled,1)) {
      server.send(400,"text/plain","Nome, giorni o numero di azioni non validi.");return;
    }
    name.toCharArray(next.name,sizeof(next.name));
    next.days=days;next.count=count;next.enabled=enabled;
    for(unsigned i=0;i<count;++i) if(!parseStep(i,next.steps[i])) {server.send(400,"text/plain","Azione non valida.");return;}
  }
  if(!validPlan(next)) {server.send(400,"text/plain","Completa giorni, orari e destinatari.");return;}
  const Plan previous=store->plans[index];
  store->plans[index]=next;
  if(!writeRecord(storePath,*store)) {
    store->plans[index]=previous;storageError=500;
    server.send(500,"text/plain","Salvataggio non riuscito.");return;
  }
  dirty=false;durableDigest=digest();storageError=0;probeDue=millis();
  server.send(200,"text/plain","Programmazione salvata. Sincronizzazione in corso.");
}

void routes() {
  server.on("/api/multi/devices",HTTP_GET,devices);
  server.on("/api/multi/config",HTTP_POST,configure);
  server.on("/api/multi/command",HTTP_POST,command);
  server.on("/api/multi/jobs",HTTP_GET,jobStatus);
  server.on("/api/multi/plans",HTTP_GET,plans);
  server.on("/api/multi/plans",HTTP_POST,savePlan);
}
} // namespace MultiDaikin
