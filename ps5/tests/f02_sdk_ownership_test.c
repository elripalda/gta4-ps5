#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "host_kernel.h"
#include "ps5platform/kernel.h"
#include "ps5platform/shm.h"

static bool fail_release, fail_unmap, fail_reserve, misplaced_reserve;
static unsigned release_calls, unmap_calls;
static const int32_t failure = (int32_t)0x8002000c;

int32_t __real_sceKernelReleaseDirectMemory(int64_t, size_t);
int32_t __real_sceKernelMunmap(void *, size_t);
int32_t __real_sceKernelReserveVirtualRange(void **, size_t, int, size_t);

int32_t __wrap_sceKernelReleaseDirectMemory(int64_t start, size_t bytes) {
  ++release_calls;
  if (fail_release) return failure;
  return __real_sceKernelReleaseDirectMemory(start, bytes);
}

int32_t __wrap_sceKernelMunmap(void *address, size_t bytes) {
  ++unmap_calls;
  if (fail_unmap) return failure;
  return __real_sceKernelMunmap(address, bytes);
}

int32_t __wrap_sceKernelReserveVirtualRange(void **address, size_t bytes, int flags,
                                            size_t alignment) {
  if (fail_reserve) return failure;
  if (misplaced_reserve) *address = NULL;
  return __real_sceKernelReserveVirtualRange(address, bytes, flags, alignment);
}

static void counters(uint64_t objects, uint64_t views, uint64_t ranges) {
  struct ps5_shm_stats stats;
  ps5_shm_live(&stats);
  assert(stats.objects == objects && stats.object_bytes == objects * 0x10000);
  assert(stats.views == views && stats.view_bytes == views * 0x4000);
  assert(stats.ranges == ranges && stats.range_bytes == ranges * 0x10000);
  assert(host_direct_allocations() == (long long)objects);
}

static void release_retry(void) {
  struct ps5_shm shm;
  assert(ps5_shm_create(0x4000, &shm) == 0);
  const struct ps5_shm owned = shm;
  counters(1, 0, 0);
  fail_release = true;
  for (unsigned retry = 0; retry < 3; ++retry) {
    assert(ps5_shm_destroy_reported(&shm) == failure);
    assert(memcmp(&shm, &owned, sizeof(shm)) == 0);
    counters(1, 0, 0);
  }
  fail_release = false;
  assert(ps5_shm_destroy_reported(&shm) == 0);
  assert(shm.bytes == 0 && shm.direct_start == 0);
  counters(0, 0, 0);
  const unsigned called = release_calls;
  assert(ps5_shm_destroy_reported(&shm) == 0);
  assert(release_calls == called);
  counters(0, 0, 0);
}

static void view_failure(bool reserve_failure, bool wrong_address) {
  struct ps5_shm shm;
  void *arena, *view;
  assert(ps5_shm_create(0x10000, &shm) == 0);
  assert(ps5_vrange_reserve(0x10000, NULL, 0x10000, &arena) == 0);
  assert(ps5_shm_map(&shm, 0, 0x4000, arena, PS5_SHM_READ | PS5_SHM_WRITE,
                     PS5_SHM_FIXED, &view) == 0);
  counters(1, 1, 1);
  struct ps5_shm_unmap_report report;
  memset(&report, 0x5a, sizeof(report));
  assert(ps5_shm_unmap_reported(NULL, 0x4000, 0, &report) == PS5_SHM_BAD_REQUEST);
  assert(report.unmap_result == PS5_SHM_BAD_REQUEST);
  assert(ps5_shm_unmap_reported(view, 0x4000, 0, NULL) == PS5_SHM_BAD_REQUEST);
  counters(1, 1, 1);
  fail_unmap = true;
  assert(ps5_shm_unmap_reported(view, 0x4000, PS5_SHM_KEEP_RESERVED, &report) == failure);
  assert(report.unmap_result == failure && report.reserved == 0);
  *(volatile uint32_t *)view = 0xabcdef;
  counters(1, 1, 1);
  fail_unmap = false;
  fail_reserve = reserve_failure;
  misplaced_reserve = wrong_address;
  const int rc = ps5_shm_unmap_reported(view, 0x4000, PS5_SHM_KEEP_RESERVED, &report);
  assert(report.unmap_result == 0 && report.reserved == 1);
  assert(rc == (reserve_failure ? failure : wrong_address ? PS5_SHM_NO_PLACE : 0));
  assert(report.reserve_result == rc);
  counters(1, 0, 1);
  fail_reserve = misplaced_reserve = false;
  if (reserve_failure || wrong_address) {
    void *hole = view;
    assert(__real_sceKernelReserveVirtualRange(&hole, 0x4000, PS5_KERNEL_MAP_FIXED, 0x4000) == 0);
    assert(hole == view);
  }
  assert(ps5_shm_destroy_reported(&shm) == 0);
  assert(ps5_vrange_release(arena, 0x10000) == 0);
  counters(0, 0, 0);
}

static void three_objects(void) {
  struct ps5_shm shm[3];
  for (unsigned i = 0; i < 3; ++i) assert(ps5_shm_create(0x10000, &shm[i]) == 0);
  counters(3, 0, 0);
  assert(ps5_shm_destroy_reported(&shm[0]) == 0);
  fail_release = true;
  assert(ps5_shm_destroy_reported(&shm[1]) == failure);
  counters(2, 0, 0);
  fail_release = false;
  assert(ps5_shm_destroy_reported(&shm[1]) == 0);
  assert(ps5_shm_destroy_reported(&shm[2]) == 0);
  counters(0, 0, 0);
}

int main(void) {
  assert(ps5_shm_destroy_reported(NULL) == PS5_SHM_BAD_REQUEST);
  counters(0, 0, 0);
  for (unsigned cycle = 0; cycle < 3; ++cycle) {
    release_retry();
    view_failure(false, false);
    view_failure(true, false);
    view_failure(false, true);
    three_objects();
  }
  printf("SDK ownership: 3 repeated cycles, release/unmap/reservation failures and retry counters passed (%u releases, %u unmaps)\n", release_calls, unmap_calls);
  return 0;
}
