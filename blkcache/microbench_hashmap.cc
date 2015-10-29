#include <iostream>
#include <atomic>
#include <sys/time.h>
#include <unordered_map>
#include "include/rocksdb/env.h"
#include "port/port_posix.h"
#include "util/mutexlock.h"
#include "blkcache/scalable_hash_table.h"
#include <gflags/gflags.h>
#include <functional>

using namespace rocksdb;
using namespace std;

DEFINE_int32(nsec, 10, "nsec");
DEFINE_int32(nthread_write, 1, "insert %");
DEFINE_int32(nthread_read, 0, "lookup %");
DEFINE_int32(nthread_erase, 0, "erase %");

uint64_t NowInMillSec() {
  timeval tv;
  gettimeofday(&tv, /*tz=*/ nullptr);
  return tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

//
// HashMap interface
//
template<class Key, class Value>
class HashMap {
 public:

  virtual ~HashMap() {}

  virtual bool Insert(const Key& key, const Value& val) = 0;
  virtual bool Erase(const Key& key) = 0;
  virtual bool Lookup(const Key& key, Value* val) = 0;
};

//
// Benchmark driver
//
template<class Key, class Value>
class MicroBenchmark {
 public:

  MicroBenchmark(HashMap<Key, Value>* impl, const size_t sec,
                 const size_t nthread_write, const size_t nthread_read,
                 const size_t nthread_erase)
    : impl_(impl),
      sec_(sec),
      ninserts_(0),
      nreads_(0),
      nerases_(0) {
    max_key_ = 1024 * 1024;
    val_ = string(1000, 'a');

    Prepop();

    StartThreads(nthread_write, WriteMain);
    StartThreads(nthread_read, ReadMain);
    StartThreads(nthread_erase, EraseMain);

    Env* env = Env::Default();
    env->WaitForJoin();

    if (sec_) {
      cout << "insert/sec=" << ninserts_ / sec_ << endl;
      cout << "read/sec=" << nreads_ / sec_ << endl;
      cout << "erases/sec=" << nerases_ / sec_ << endl;
    }
  }

  void RunWrite() {
    uint64_t start = NowInMillSec();
    while (!Timedout(start)) {
      impl_->Insert(random() + max_key_, string(1000, 'a'));
      ninserts_++;
    }
  }

  void RunRead() {
    uint64_t start = NowInMillSec();
    while (!Timedout(start)) {
      string s;
      int k = random() % max_key_;
      bool status = impl_->Lookup(k, &s);
      assert(status);
      nreads_++;
    }
  }

  void RunErase() {
    uint64_t start = NowInMillSec();
    while (!Timedout(start)) {
      impl_->Erase(random() + max_key_);
      nerases_++;
    }
  }


 private:

  bool Timedout(const uint64_t start) {
    return NowInMillSec() - start > sec_ * 1000;
  }

  void StartThreads(const size_t n, void (*fn)(void*)) {
    Env* env = Env::Default();
    for (size_t i = 0; i < n; ++i) {
      env->StartThread(fn, this);
    }
  }

  void Prepop() {
    for (size_t i = 0; i < max_key_; ++i) {
      bool status = impl_->Insert(i, string(1000, 'a'));
      assert(status);
    }
  }

  static void WriteMain(void* args) {
    ((MicroBenchmark<Key, Value>*) args)->RunWrite();
  }

  static void ReadMain(void* args) {
    ((MicroBenchmark<Key, Value>*) args)->RunRead();
  }

  static void EraseMain(void* args) {
    ((MicroBenchmark<Key, Value>*) args)->RunErase();
  }

  HashMap<Key, Value>* impl_;
  const size_t sec_;

  size_t max_key_;
  Value val_;

  atomic<size_t> ninserts_;
  atomic<size_t> nreads_;
  atomic<size_t> nerases_;
};

//
// SimpleHashMap -- unordered_map implementation
//
class SimpleHashMap : public HashMap<int, string> {
 public:

  bool Insert(const int& key, const string& val) override {
   WriteLock _(&rwlock_);
   map_.insert(make_pair(key, val));
   return true;
  }

  bool Erase(const int& key) override {
    WriteLock _(&rwlock_);
    auto it = map_.find(key);
    if (it == map_.end()) {
      return false;
    }
    map_.erase(it);
    return true;
  }

  bool Lookup(const int& key, string* val) override {
    ReadLock _(&rwlock_);
    return map_.find(key) != map_.end();
  }

 private:

  port::RWMutex rwlock_;
  std::unordered_map<int, string> map_;
};

//
// ScalableHashMap
//
class ScalableHashMap : public HashMap<int, string> {
 public:

  bool Insert(const int& key, const string& val) override {
    Node n(key, val);
    return impl_.Insert(n);
  }

  bool Erase(const int& key) override {
    Node n(key, string());
    return impl_.Erase(n, nullptr);
  }

  bool Lookup(const int& key, string* val) override {
    Node n(key, string());
    ReadLock _(impl_.GetMutex(n));
    return impl_.Find(n, nullptr);
  }

 private:

  struct Node {
    Node(const int key, const string& val)
      : key_(key)
      , val_(val) {
    }

    int key_;
    string val_;
  };

  struct Hash {
    uint64_t operator()(const Node& node) {
      return std::hash<uint64_t>()(node.key_);
    }
  };

  struct Equal {
    bool operator()(const Node& lhs, const Node& rhs) {
      return lhs.key_ == rhs.key_;
    }
  };

  ScalableHashTable<Node, Hash, Equal> impl_;
};


//
// main
//
int
main(int argc, char** argv) {
  google::SetUsageMessage(std::string("\nUSAGE:\n") + std::string(argv[0]) +
                          " [OPTIONS]...");
  google::ParseCommandLineFlags(&argc, &argv, false);

  //
  // Micro benchmark unordered_map
  //
  cout << "Micro benchmarking std::unordered_map" << endl;
  {
    SimpleHashMap impl;
    MicroBenchmark<int, string> _(&impl, FLAGS_nsec,
                                  FLAGS_nthread_write, FLAGS_nthread_read,
                                  FLAGS_nthread_erase);
  }
  //
  // Micro benchmark scalable hash table
  //
  cout << "Micro benchmarking scalable hash map" << endl;

  {
    ScalableHashMap impl;
    MicroBenchmark<int, string> _(&impl, FLAGS_nsec,
                                  FLAGS_nthread_write, FLAGS_nthread_read,
                                  FLAGS_nthread_erase);
  }

  return 0;
}