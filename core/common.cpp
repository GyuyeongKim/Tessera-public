#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "tessera.hpp"

namespace tessera {

namespace {
const char* err_text(int r, const char* buf) { return r == 0 ? buf : "unknown error"; }
const char* err_text(const char* r, const char*) { return r; }
const char* strerror_r_text(int e, char* buf, size_t n) { return err_text(strerror_r(e, buf, n), buf); }
}

void ChunkFree::operator()(Triple* p) const {
  if (p && !map) delete[] p;
}

ChunkPtr OutSpace::alloc(size_t cells) {
  ChunkPtr p(new Triple[cells], ChunkFree(cells * sizeof(Triple)));
  heap.fetch_add(cells * sizeof(Triple), std::memory_order_relaxed);
  return p;
}

void OutSpace::spill_fail(const char* what, int err) {
  spill_failed.fetch_add(1, std::memory_order_relaxed);
  while (file_lock.exchange(true, std::memory_order_acquire)) {
  }
  if (!error[0] && err == 0) {
    limit_hit = true;
    snprintf(error, sizeof error,
             "tessera: output memory limit reached and the spill files in %s reached their limit (%llu bytes)",
             spill_dir.empty() ? "." : spill_dir.c_str(), (unsigned long long)spill_limit);
  } else if (!error[0]) {
    char why[128];
    snprintf(error, sizeof error, "tessera: output memory limit reached and the spill file could not be %s in %s: %s",
             what, spill_dir.empty() ? "." : spill_dir.c_str(), strerror_r_text(err, why, sizeof why));
  }
  file_lock.store(false, std::memory_order_release);
  failed.store(true, std::memory_order_relaxed);
}

void OutSpace::release(ChunkPtr& buf) {
  if (buf && !buf.get_deleter().map) heap.fetch_sub(buf.get_deleter().bytes, std::memory_order_relaxed);
  buf.reset();
}

void OutSpace::lanes(int n) {
  files.reset(n > 0 ? new SpillFile[n] : nullptr);
  n_files = n > 0 ? n : 0;
  spill_dir = dir;
  if (spill_dir.empty()) {
    const char* t = getenv("TMPDIR");
    spill_dir = t && *t ? t : "/tmp";
  }
  const std::string path = spill_dir + "/tessera_spill_XXXXXX";
  name_len = path.size() + 1;
  names.reset(budget && n > 0 ? new char[(size_t)(n + 1) * name_len] : nullptr);
  for (int i = 0; budget && i <= n; i++) memcpy(names.get() + (size_t)i * name_len, path.c_str(), name_len);
}

int OutSpace::file(int lane) {
  while (file_lock.exchange(true, std::memory_order_acquire)) {
  }
  int& fd = files[lane].fd;
  if (fd < 0 && !file_failed) {
    char* name = names.get() + (size_t)lane * name_len;
    fd = mkstemp(name);
    if (fd < 0 && errno == ENOENT && mkdir_dir && mkdir(spill_dir.c_str(), 0755) == 0) {
      memcpy(name, names.get() + (size_t)n_files * name_len, name_len);
      fd = mkstemp(name);
    }
    if (fd >= 0) {
      unlink(name);
    } else {
      file_failed = true;
      file_errno = errno;
    }
  }
  const int f = fd, err = file_errno;
  file_lock.store(false, std::memory_order_release);
  if (f < 0) spill_fail("created", err);
  return f;
}

void OutSpace::settle(Cells& c, ChunkPtr& spare, int lane) {
  if (!budget || c.n == 0 || !c.data || heap.load(std::memory_order_relaxed) <= budget) return;
  if (!spill) {
    no_memory();
    return;
  }
  const size_t bytes = c.n * sizeof(Triple);
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const size_t padded = (bytes + page - 1) / page * page;
  if (spill_limit && spill_bytes.fetch_add(padded, std::memory_order_relaxed) + padded > spill_limit) {
    spill_fail("written", 0);
    return;
  }
  const int f = lane >= 0 && lane < n_files ? file(lane) : -1;
  if (f < 0) return;
  const int64_t off = files[lane].end.fetch_add((int64_t)padded, std::memory_order_relaxed);
  const char* p = reinterpret_cast<const char*>(c.data.get());
  for (size_t done = 0; done < bytes;) {
    const ssize_t w = pwrite(f, p + done, bytes - done, off + (int64_t)done);
    if (w < 0 && errno == EINTR) continue;
    if (w <= 0) {
      spill_fail("written", w < 0 ? errno : ENOSPC);
      return;
    }
    done += (size_t)w;
  }
  spare = std::move(c.data);
  c.file_off = off;
  spilled.fetch_add(1, std::memory_order_relaxed);
}

bool OutSpace::map_spilled(std::vector<Cells>& chunks, int lane) {
  if (lane < 0 || lane >= n_files || files[lane].fd < 0) return true;
  const size_t len = (size_t)files[lane].end.load();
  if (len == 0) return true;
  void* m = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, files[lane].fd, 0);
  if (m == MAP_FAILED) {
    spill_fail("mapped", errno);
    return false;
  }
  std::shared_ptr<void> hold(m, [len](void* p) { munmap(p, len); });
  for (auto& c : chunks) {
    if (c.file_off < 0) continue;
    c.data = ChunkPtr(reinterpret_cast<Triple*>(static_cast<char*>(m) + c.file_off),
                      ChunkFree(c.n * sizeof(Triple), hold));
    c.file_off = -1;
  }
  return true;
}

OutSpace::~OutSpace() {
  for (int i = 0; i < n_files; i++) {
    if (files[i].fd >= 0) close(files[i].fd);
  }
}

}
