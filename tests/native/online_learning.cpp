#include "OnlineLearning.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <string>
using namespace OnlineLearning;

using String = std::string;
struct File {
  std::shared_ptr<std::fstream> stream;
  std::string path;
  explicit operator bool() const { return stream && stream->is_open() && stream->good(); }
  size_t size() { return std::filesystem::file_size(path); }
  size_t readBytes(char *p, size_t n) { stream->read(p, n); return size_t(stream->gcount()); }
  size_t write(const uint8_t *p, size_t n) { stream->write(reinterpret_cast<const char *>(p), n); return stream->good() ? n : 0; }
  void flush() { stream->flush(); }
  void close() { stream->close(); }
};
struct TestFS {
  bool failRename = false;
  std::string name(const char *p) { return std::string(".") + p; }
  File open(const char *path, const char *mode) {
    File f;
    f.path = name(path);
    f.stream = std::make_shared<std::fstream>(f.path, std::ios::binary | (mode[0] == 'w' ? std::ios::out | std::ios::trunc : std::ios::in));
    return f;
  }
  bool exists(const char *path) { return std::filesystem::exists(name(path)); }
  bool remove(const char *path) { return std::filesystem::remove(name(path)); }
  bool rename(const char *from, const char *to) {
    if (failRename) return false;
    std::error_code error;
    std::filesystem::rename(name(from), name(to), error);
    return !error;
  }
} LittleFS;
Engine onlineLearner;
bool archiveReady = true;
uint32_t onlineSequence = 0, onlineSavedAt = 0, onlineSaveDue = 0;
String onlineStorageStatus;
uint32_t millis() { return 7200000UL; }
#include "checkpoint_functions.inc"

void rawValues(float *raw, float room = 20) {
  const float values[12] = {room,10,22,22,600,0,1,1,0,0,0,0};
  std::memcpy(raw, values, sizeof(values));
}

void feed(Engine &engine, int first, int last, uint32_t signature = 1) {
  float raw[12]; rawValues(raw);
  for (int i = first; i <= last; ++i) assert(engine.sample(uint32_t(i) * 30000UL, signature, raw));
}

int main(int argc, char **argv) {
  assert(argc == 2);
  const std::string name(argv[1]);
  if (name == "delay") {
    Engine e; feed(e,0,19); assert(e.state.trained == 0);
    feed(e,20,20); assert(e.state.trained == 1);
    e.controls(2); feed(e,21,40,2); assert(e.state.trained == 1);
    feed(e,41,41,2); assert(e.state.trained == 2);
  } else if (name == "gap") {
    Engine e; feed(e,0,10); feed(e,13,32); assert(e.state.trained == 0);
    feed(e,33,33); assert(e.state.trained == 1);
  } else if (name == "invalid") {
    Engine e; float raw[12]; rawValues(raw);
    raw[11] = -1; assert(!e.sample(0,1,raw));
    rawValues(raw); raw[0] = NAN; assert(!e.sample(30000,1,raw));
    rawValues(raw);
    for (uint32_t i=0;i<=20;++i) assert(e.sample(uint32_t(UINT32_MAX-300000UL+i*30000UL),1,raw));
    assert(e.state.trained == 1);
  } else if (name == "pause") {
    Engine e; feed(e,0,60); const uint32_t trained=e.state.trained;
    e.setEnabled(false); feed(e,61,100); assert(e.state.trained==trained);
    e.setEnabled(true); feed(e,101,120); assert(e.state.trained==trained);
    feed(e,121,121); assert(e.state.trained==trained+1);
    Engine restored; assert(restored.restore(e.state));
    feed(restored,122,141); assert(restored.state.trained==e.state.trained);
    Persist bad=e.state; bad.candidate.bias=NAN; assert(!restored.restore(bad));
  } else if (name == "no_false_promotion") {
    Limits limits; limits.trainActive=10; limits.trainTotal=10; limits.validationActive=10; limits.validationTotal=20;
    Engine e(limits); feed(e,0,200);
    assert(e.state.trials>0 && e.state.accepted==0 && e.state.promotions==0);
  } else if (name == "frozen_trial") {
    Limits limits; limits.trainActive=10; limits.trainTotal=10; limits.validationActive=10; limits.validationTotal=20;
    Engine e(limits); e.state.candidate.bias=2*std::atanh(.1f);
    float raw[12]; bool trialStarted=false; Net frozen{};
    for (uint32_t i=0;i<180 && !e.state.accepted;++i) {
      rawValues(raw,20+float(i)*.01f);
      assert(e.sample(i*30000UL,1,raw));
      if (!trialStarted && e.state.trialRunning) { frozen=e.state.trial; trialStarted=true; }
      if (trialStarted && !e.state.accepted) {
        assert(std::memcmp(&frozen,&e.state.trial,sizeof(Net))==0);
        e.state.candidate.bias=-1; // Training candidate changes, frozen trial must not.
      }
    }
    assert(trialStarted && e.state.accepted && e.state.promotions==1);
    assert(std::memcmp(&frozen,&e.state.champion,sizeof(Net))==0);
    assert(std::isfinite(e.acceptedPrediction));
    raw[6]=0; e.sample(180*30000UL,2,raw);
    assert(!std::isfinite(e.acceptedPrediction)); // Off state was not validated.
  } else if (name == "bounded") {
    Engine e; float raw[12],x[12]; rawValues(raw); assert(Engine::normalise(raw,x));
    for(int i=0;i<2000;++i)e.state.candidate.train(x,i%2?1000:-1000);
    assert(e.state.candidate.valid());
    assert(std::fabs(e.state.candidate.predict(x))<=2);
    assert(sizeof(Engine)<5000);
  } else if (name == "checkpoint") {
    feed(onlineLearner,0,60); const uint32_t first=onlineLearner.state.trained;
    assert(saveOnlineLearning(true)); assert(onlineSequence==1);
    feed(onlineLearner,61,90); assert(saveOnlineLearning(true)); assert(onlineSequence==2);
    {std::ofstream damaged("./online-b.bin",std::ios::binary|std::ios::trunc); damaged<<"incomplete";}
    onlineLearner.reset(); onlineSequence=0; loadOnlineLearning();
    assert(onlineSequence==1 && onlineLearner.state.trained==first);
    feed(onlineLearner,100,140); assert(saveOnlineLearning(true));
    const uint32_t previous=onlineLearner.state.trained;
    feed(onlineLearner,141,170); LittleFS.failRename=true;
    assert(!saveOnlineLearning(true)); assert(onlineLearner.dirty);
    onlineLearner.reset(); onlineSequence=0; loadOnlineLearning();
    assert(onlineLearner.state.trained==previous);
    float raw[12];rawValues(raw);
    onlineLearner.sample(171*30000UL,1,raw);
    assert(onlineLearner.state.trained==previous); // Stale delayed labels are never restored.
  } else if (name == "replay") {
    uint32_t at,signature; float raw[12];
    while(std::cin>>at>>signature) {
      for(float &v:raw)std::cin>>v;
      onlineLearner.sample(at,signature,raw);
    }
    const auto &p=onlineLearner.state;
    std::cout<<"{\"samples\":"<<p.observed<<",\"examples\":"<<p.trained
      <<",\"active_examples\":"<<p.recentActive.count<<",\"accepted\":"<<p.accepted
      <<",\"active_network_mae\":"<<p.recentActive.network
      <<",\"active_baseline_mae\":"<<p.recentActive.persistence<<"}\n";
  } else return 2;
  return 0;
}
