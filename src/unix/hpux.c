/* Copyright Joyent, Inc. and other Node contributors. All rights reserved.
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */ 

#include "uv.h"
#include "internal.h"

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#include <net/if.h>
#include <net/if_dl.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <sys/pstat.h>
#include <sys/dk.h>
#include <sys/devpoll.h>


int uv__platform_loop_init(uv_loop_t* loop, int default_loop) {
   int err;
  int fd;

  loop->backend_fd = -1;

  fd = open("/dev/poll", O_RDWR);
  if (fd == -1)
    return -errno;

  err = uv__cloexec(fd, 1);
  if (err) {
    uv__close(fd);
    return err;
  }
  loop->backend_fd = fd;

 return 0;
}


void uv__platform_loop_delete(uv_loop_t* loop) {
  if (loop->backend_fd != -1) {
    uv__close(loop->backend_fd);
    loop->backend_fd = -1;
  }

  if (loop->backend_fd != -1) {
    uv__close(loop->backend_fd);
    loop->backend_fd = -1;
  }
}


void uv__platform_invalidate_fd(uv_loop_t* loop, int fd) {
struct pollfd* events;
  uintptr_t i;
  uintptr_t nfds;

  assert(loop->watchers != NULL);

  events = (struct pollfd*) loop->watchers[loop->nwatchers];
  nfds = (uintptr_t) loop->watchers[loop->nwatchers + 1];
  if (events == NULL)
    return;

  /* Invalidate events with same file descriptor */
  for (i = 0; i < nfds; i++)
    if ((int) events[i].fd == fd)
      events[i].events = -1;
}


void uv__io_poll(uv_loop_t* loop, int timeout) {
  struct pollfd events[1024];
  struct pollfd* pe;
  struct pollfd pc;
  struct dvpoll dvp;
  struct timespec spec;
  QUEUE* q;
  uv__io_t* w;
  uint64_t base;
  uint64_t diff;
  unsigned int nfds;
  unsigned int i;
  int saved_errno;
  int nevents;
  int count;
  int fd;

  if (loop->nfds == 0) {
    assert(QUEUE_EMPTY(&loop->watcher_queue));
    return;
  }

  while (!QUEUE_EMPTY(&loop->watcher_queue)) {
    q = QUEUE_HEAD(&loop->watcher_queue);
    QUEUE_REMOVE(q);
    QUEUE_INIT(q);

    w = QUEUE_DATA(q, uv__io_t, watcher_queue);
    assert(w->pevents != 0);

   pc.fd = w->fd;
   pc.events = w->pevents;
   if (write(loop->backend_fd, &pc, sizeof(pc)) < 0)
     abort(); 

    w->events = w->pevents;
  }

  assert(timeout >= -1);
  base = loop->time;
  count = 48; /* Benchmarks suggest this gives the best throughput. */

  for (;;) {
    if (timeout != -1) {
      spec.tv_sec = timeout / 1000;
      spec.tv_nsec = (timeout % 1000) * 1000000;
    }

    /* Work around a kernel bug where nfds is not updated. */
    events[0].revents = 0;

    saved_errno = 0;

    dvp.dp_fds = events;
    dvp.dp_nfds = 1024;
    dvp.dp_timeout = -1;

    nfds = ioctl(loop->backend_fd, DP_POLL, &dvp);

    /* Update loop->time unconditionally. It's tempting to skip the update when
     * timeout == 0 (i.e. non-blocking poll) but there is no guarantee that the
     * operating system didn't reschedule our process while in the syscall.
     */
    SAVE_ERRNO(uv__update_time(loop));

    if (events[0].revents == 0) {
      if (timeout == 0)
        return;

      if (timeout == -1)
        continue;

      goto update_timeout;
    }

    if (nfds == 0) {
      assert(timeout != -1);
      return;
    }

    nevents = 0;

    assert(loop->watchers != NULL);
    loop->watchers[loop->nwatchers] = (void*) events;
    loop->watchers[loop->nwatchers + 1] = (void*) (uintptr_t) nfds;
    for (i = 0; i < nfds; i++) {
      pe = events + i;
      fd = pe->fd;

      /* Skip invalidated events, see uv__platform_invalidate_fd */
      if (fd == -1)
        continue;

      assert(fd >= 0);
      assert((unsigned) fd < loop->nwatchers);

      w = loop->watchers[fd];

      /* File descriptor that we've stopped watching, ignore. */
      if (w == NULL)
        continue;

      w->cb(loop, w, pe->events);
      nevents++;

      if (w != loop->watchers[fd])
        continue;  /* Disabled by callback. */

      /* Events Ports operates in oneshot mode, rearm timer on next run. */
      if (w->pevents != 0 && QUEUE_EMPTY(&w->watcher_queue))
        QUEUE_INSERT_TAIL(&loop->watcher_queue, &w->watcher_queue);
    }
    loop->watchers[loop->nwatchers] = NULL;
    loop->watchers[loop->nwatchers + 1] = NULL;

    if (nevents != 0) {
      if (nfds == ARRAY_SIZE(events) && --count != 0) {
        /* Poll for more events but don't block this time. */
        timeout = 0;
        continue;
      }
      return;
    }

    if (saved_errno == ETIME) {
      assert(timeout != -1);
      return;
    }

    if (timeout == 0)
      return;

    if (timeout == -1)
      continue;

update_timeout:
    assert(timeout > 0);

    diff = loop->time - base;
    if (diff >= (uint64_t) timeout)
      return;

    timeout -= diff;
  }
}

extern  hrtime_t        gethrtime __((void)); 

uint64_t uv__hrtime(uv_clocktype_t type) {
  return gethrtime();
}


/*
 * We could use a static buffer for the path manipulations that we need outside
 * of the function, but this function could be called by multiple consumers and
 * we don't want to potentially create a race condition in the use of snprintf.
 */
int uv_exepath(char* buffer, size_t* size) {
  ssize_t res;
  char buf[128];

  if (buffer == NULL || size == NULL)
    return -EINVAL;

  snprintf(buf, sizeof(buf), "/proc/%lu/path/a.out", (unsigned long) getpid());
  res = readlink(buf, buffer, *size - 1);
  if (res == -1)
    return -errno;

  buffer[res] = '\0';
  *size = res;
  return 0;
}


uint64_t uv_get_free_memory(void) {
 struct pst_dynamic info;
 long pages = -1;
  if (pstat_getdynamic(&info, sizeof(info), 1, 0) == 1) {
    pages = info.psd_free;
  }
 return (uint64_t) sysconf(_SC_PAGESIZE) * pages;
}


uint64_t uv_get_total_memory(void) {
  struct pst_dynamic info;
 long pages = -1;
  if (pstat_getdynamic(&info, sizeof(info), 1, 0) == 1) {
    pages = info.psd_rm;
  }
return (uint64_t) sysconf(_SC_PAGESIZE) * pages;
}


void uv_loadavg(double avg[3]) {
struct pst_dynamic psd;
  if (pstat_getdynamic(&psd,sizeof(psd),(size_t)1,0)!=-1) {
    avg[0] = psd.psd_avg_1_min;
    avg[1] = psd.psd_avg_5_min;
    avg[2] = psd.psd_avg_15_min;
  }
}


#if defined(PORT_SOURCE_FILE)

static int uv__fs_event_rearm(uv_fs_event_t *handle) {
  if (handle->fd == -1)
    return -EBADF;

  if (port_associate(handle->loop->fs_fd,
                     PORT_SOURCE_FILE,
                     (uintptr_t) &handle->fo,
                     FILE_ATTRIB | FILE_MODIFIED,
                     handle) == -1) {
    return -errno;
  }
  handle->fd = PORT_LOADED;

  return 0;
}


static void uv__fs_event_read(uv_loop_t* loop,
                              uv__io_t* w,
                              unsigned int revents) {
  uv_fs_event_t *handle = NULL;
  timespec_t timeout;
  port_event_t pe;
  int events;
  int r;

  (void) w;
  (void) revents;

  do {
    uint_t n = 1;

    /*
     * Note that our use of port_getn() here (and not port_get()) is deliberate:
     * there is a bug in event ports (Sun bug 6456558) whereby a zeroed timeout
     * causes port_get() to return success instead of ETIME when there aren't
     * actually any events (!); by using port_getn() in lieu of port_get(),
     * we can at least workaround the bug by checking for zero returned events
     * and treating it as we would ETIME.
     */
    do {
      memset(&timeout, 0, sizeof timeout);
      r = port_getn(loop->fs_fd, &pe, 1, &n, &timeout);
    }
    while (r == -1 && errno == EINTR);

    if ((r == -1 && errno == ETIME) || n == 0)
      break;

    handle = (uv_fs_event_t*) pe.portev_user;
    assert((r == 0) && "unexpected port_get() error");

    events = 0;
    if (pe.events & (FILE_ATTRIB | FILE_MODIFIED))
      events |= UV_CHANGE;
    if (pe.events & ~(FILE_ATTRIB | FILE_MODIFIED))
      events |= UV_RENAME;
    assert(events != 0);
    handle->fd = PORT_FIRED;
    handle->cb(handle, NULL, events, 0);
  }
  while (handle->fd != PORT_DELETED);

  if (handle != NULL && handle->fd != PORT_DELETED)
    uv__fs_event_rearm(handle);  /* FIXME(bnoordhuis) Check return code. */
}


int uv_fs_event_init(uv_loop_t* loop, uv_fs_event_t* handle) {
  uv__handle_init(loop, (uv_handle_t*)handle, UV_FS_EVENT);
  return 0;
}


int uv_fs_event_start(uv_fs_event_t* handle,
                      uv_fs_event_cb cb,
                      const char* filename,
                      unsigned int flags) {
  int portfd;
  int first_run;

  if (uv__is_active(handle))
    return -EINVAL;

  first_run = 0;
  if (handle->loop->fs_fd == -1) {
    portfd = open("/dev/poll", O_RDWR);
    if (portfd == -1)
      return -errno;
    handle->loop->fs_fd = portfd;
    first_run = 1;
  }

  uv__handle_start(handle);
  handle->filename = strdup(filename);
  handle->fd = PORT_UNUSED;
  handle->cb = cb;

  memset(&handle->fo, 0, sizeof handle->fo);
  handle->fo.fo_name = handle->filename;
  uv__fs_event_rearm(handle);  /* FIXME(bnoordhuis) Check return code. */

  if (first_run) {
    uv__io_init(&handle->loop->fs_event_watcher, uv__fs_event_read, portfd);
    uv__io_start(handle->loop, &handle->loop->fs_event_watcher, UV__POLLIN);
  }

  return 0;
}


int uv_fs_event_stop(uv_fs_event_t* handle) {
  if (!uv__is_active(handle))
    return -EINVAL;

  if (handle->fd == PORT_FIRED || handle->fd == PORT_LOADED) {
    port_dissociate(handle->loop->fs_fd,
                    PORT_SOURCE_FILE,
                    (uintptr_t) &handle->fo);
  }

  handle->fd = PORT_DELETED;
  free(handle->filename);
  handle->filename = NULL;
  handle->fo.fo_name = NULL;
  uv__handle_stop(handle);

  return 0;
}

void uv__fs_event_close(uv_fs_event_t* handle) {
  uv_fs_event_stop(handle);
}

#else /* !defined(PORT_SOURCE_FILE) */

int uv_fs_event_init(uv_loop_t* loop, uv_fs_event_t* handle) {
  return -ENOSYS;
}


int uv_fs_event_start(uv_fs_event_t* handle,
                      uv_fs_event_cb cb,
                      const char* filename,
                      unsigned int flags) {
  return -ENOSYS;
}


int uv_fs_event_stop(uv_fs_event_t* handle) {
  return -ENOSYS;
}


void uv__fs_event_close(uv_fs_event_t* handle) {
  UNREACHABLE();
}

#endif /* defined(PORT_SOURCE_FILE) */


char** uv_setup_args(int argc, char** argv) {
  return argv;
}


int uv_set_process_title(const char* title) {
  return 0;
}


int uv_get_process_title(char* buffer, size_t size) {
  if (size > 0) {
    buffer[0] = '\0';
  }
  return 0;
}


int uv_resident_set_memory(size_t* rss) {
   return 0;
}


int uv_uptime(double* uptime) {

struct pst_status p;

 if (pstat_getproc(&p, sizeof(p), 0, 1) == 1)
  {
   *uptime = (time_t)p.pst_start;
  }
 else {
   *uptime = -1;
  }

  return 0;
}


int uv_cpu_info(uv_cpu_info_t** cpu_infos, int* count) {
uv_cpu_info_t* cpu_info;
struct pst_dynamic psd;
struct pst_processor *psp;
int i = 0;

if (pstat_getdynamic(&psd, sizeof(psd), (size_t)1, 0) != -1) {
int nspu = psd.psd_proc_cnt;
psp = (struct pst_processor *)
malloc(nspu * sizeof(struct pst_processor));
if (pstat_getprocessor(psp, sizeof(struct pst_processor), nspu,0) != -1) {

  *cpu_infos = (uv_cpu_info_t*) malloc(nspu * sizeof(uv_cpu_info_t));
  if (!*cpu_infos) {
    free(psp);
    abort();
  }

  *count = nspu;

  cpu_info = *cpu_infos;
  while (i < nspu) {
    cpu_info->cpu_times.user = (((uint64_t)psp[i].psp_usercycles.psc_hi << 32) + psp[i].psp_usercycles.psc_lo);
    cpu_info->cpu_times.sys = (((uint64_t)psp[i].psp_systemcycles.psc_hi << 32) + psp[i].psp_systemcycles.psc_lo);
    cpu_info->cpu_times.idle = (((uint64_t)psp[i].psp_idlecycles.psc_hi << 32) + psp[i].psp_idlecycles.psc_lo);
    cpu_info->cpu_times.irq = (((uint64_t)psp[i].psp_interruptcycles.psc_hi << 32) + psp[i].psp_interruptcycles.psc_lo);
    cpu_info->cpu_times.nice = 0;
    cpu_info++;
    i++;
  }
 }
}
return 0;
}


void uv_free_cpu_info(uv_cpu_info_t* cpu_infos, int count) {
  int i;

  for (i = 0; i < count; i++) {
    free(cpu_infos[i].model);
  }

  free(cpu_infos);
}


int uv_interface_addresses(uv_interface_address_t** addresses, int* count) {
#ifdef HPUX_NO_IFADDRS
  return 0;
#else
  uv_interface_address_t* address;
  struct sockaddr_dl* sa_addr;
  struct ifaddrs* addrs;
  struct ifaddrs* ent;
  int i;

  if (getifaddrs(&addrs))
    return -errno;

  *count = 0;

  /* Count the number of interfaces */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)) ||
        (ent->ifa_addr == NULL)) {
      continue;
    }

    (*count)++;
  }

  *addresses = malloc(*count * sizeof(**addresses));
  if (!(*addresses))
    return -ENOMEM;

  address = *addresses;

  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)))
      continue;

    if (ent->ifa_addr == NULL)
      continue;

    address->name = strdup(ent->ifa_name);

    if (ent->ifa_addr->sa_family == AF_INET6) {
      address->address.address6 = *((struct sockaddr_in6*) ent->ifa_addr);
    } else {
      address->address.address4 = *((struct sockaddr_in*) ent->ifa_addr);
    }

    if (ent->ifa_netmask->sa_family == AF_INET6) {
      address->netmask.netmask6 = *((struct sockaddr_in6*) ent->ifa_netmask);
    } else {
      address->netmask.netmask4 = *((struct sockaddr_in*) ent->ifa_netmask);
    }

    address->is_internal = !!((ent->ifa_flags & IFF_PRIVATE) ||
                           (ent->ifa_flags & IFF_LOOPBACK));

    address++;
  }

  /* Fill in physical addresses for each interface */
  for (ent = addrs; ent != NULL; ent = ent->ifa_next) {
    if (!((ent->ifa_flags & IFF_UP) && (ent->ifa_flags & IFF_RUNNING)) ||
        (ent->ifa_addr == NULL) ||
        (ent->ifa_addr->sa_family != AF_LINK)) {
      continue;
    }

    address = *addresses;

    for (i = 0; i < (*count); i++) {
      if (strcmp(address->name, ent->ifa_name) == 0) {
        sa_addr = (struct sockaddr_dl*)(ent->ifa_addr);
        memcpy(address->phys_addr, LLADDR(sa_addr), sizeof(address->phys_addr));
      }
      address++;
    }
  }

  freeifaddrs(addrs);

  return 0;
#endif  /* HPUX_NO_IFADDRS */
}


void uv_free_interface_addresses(uv_interface_address_t* addresses,
  int count) {
  int i;

  for (i = 0; i < count; i++) {
    free(addresses[i].name);
  }

  free(addresses);
}
