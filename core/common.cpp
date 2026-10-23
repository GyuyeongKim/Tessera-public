#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdlib>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "tessera.hpp"

namespace tessera {

void ChunkFree::operator()(Triple* p) const {
  if (!p) return;
  if (mapped) munmap(p, bytes);
  else delete[] p;
}

ChunkPtr OutSpace::alloc(size_t cells) {
  heap.fetch_add(cells * sizeof(Triple), std::memory_order_relaxed);
  return ChunkPtr(new Triple[cells], ChunkFree(cells * sizeof(Triple)));
}

void OutSpace::release(ChunkPtr& buf) {
  if (buf && !buf.get_deleter().mapped) heap.fetch_sub(buf.get_deleter().bytes, std::memory_order_relaxed);
  buf.reset();
}

int OutSpace::file(int lane) {
  while (file_lock.exchange(true, std::memory_order_acquire)) {
  }
  int& fd = files[lane].fd;
  if (fd < 0 && !file_failed) {
    std::string d = dir;
    if (d.empty()) {
      const char* t = getenv("TMPDIR");
      d = t && *t ? t : "/tmp";
    }
    std::string path = d + "/tessera_spill_XXXXXX";
    std::vector<char> name(path.begin(), path.end());
    name.push_back(0);
    fd = mkstemp(name.data());
    if (fd >= 0) unlink(name.data());
    else file_failed = true;
  }
  const int f = fd;
  file_lock.store(false, std::memory_order_release);
  return f;
}

void OutSpace::settle(Cells& c, ChunkPtr& spare) {
  if (!budget || c.n == 0 || c.data.get_deleter().mapped || heap.load(std::memory_order_relaxed) <= budget) return;
  const int lane = (int)(std::hash<std::thread::id>()(std::this_thread::get_id()) % kSpillFiles);
  const int f = file(lane);
  if (f < 0) {
    spill_failed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const size_t bytes = c.n * sizeof(Triple);
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const int64_t off = files[lane].end.fetch_add((int64_t)((bytes + page - 1) / page * page), std::memory_order_relaxed);
  const char* p = reinterpret_cast<const char*>(c.data.get());
  for (size_t done = 0; done < bytes;) {
    const ssize_t w = pwrite(f, p + done, bytes - done, off + (int64_t)done);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) {
      spill_failed.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    done += (size_t)w;
  }
  void* m = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE, f, off);
  if (m == MAP_FAILED) {
    spill_failed.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  spare = std::move(c.data);
  c.data = ChunkPtr(static_cast<Triple*>(m), ChunkFree(bytes, true));
  spilled.fetch_add(1, std::memory_order_relaxed);
}

OutSpace::~OutSpace() {
  for (auto& f : files) {
    if (f.fd >= 0) close(f.fd);
  }
}

}
