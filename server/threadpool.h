#ifndef _THREADPOOL_H_
#define _THREADPOOL_H_

#include "epoll.h"
#include "function.h"
#include "socket.h"
#include "thread.h"

class CThreadPool {
 public:
  CThreadPool() {
    m_server = NULL;
    timespec tp = {0, 0};
    clock_gettime(CLOCK_REALTIME, &tp);
    char *buf = NULL;
    asprintf(&buf, "%ld.%ld.sock", tp.tv_sec, tp.tv_nsec % 1000000);
    if (buf != NULL) {
      m_path = buf;
      free(buf);
    }
    usleep(1);
  }
  ~CThreadPool() { Close(); }

  CThreadPool(const CThreadPool &) = delete;
  CThreadPool &operator=(const CThreadPool &) = delete;

  int Start(unsigned count) {
    int ret = 0;
    if (m_server != NULL) return -1;
    if (m_path.size() == 0) return -2;

    m_server = new CLocalSocket();
    if (m_server == NULL) return -3;

    ret = m_server->Init(CSocketParam(m_path, SOCK_ISSERVER));
    if (ret != 0) return -4;

    ret = m_epoll.Create(count);
    if (ret != 0) return -5;

    ret = m_epoll.Add(*m_server, EpollData((void *)m_server));
    if (ret != 0) return -6;

    m_threads.resize(count);
    for (unsigned i = 0; i < count; ++i) {
      m_threads[i] = new CThread(&CThreadPool::TaskDispatch, this);
      if (m_threads[i] == NULL) return -7;

      ret = m_threads[i]->Start();
      if (ret != 0) return -8;
    }
    return 0;
  }

  void Close() {
    m_epoll.Close();
    if (m_server) {
      CSocketBase *p = m_server;
      m_server = NULL;
      delete p;
    }
    for (auto thread : m_threads) {
      if (thread) delete thread;
    }
    m_threads.clear();
    unlink(m_path);
  }

  template <typename _FUNCTION_, typename... _ARGS_>
  int AddTask(_FUNCTION_ func, _ARGS_... args) {
    static thread_local CLocalSocket client;
    int ret = 0;
    if (client == -1) {
      ret = client.Init(CSocketParam(m_path, 0));
      if (ret != 0) return -1;
      ret = client.Link();
      if (ret != 0) return -2;
    }
    CFunctionBase *base = new CFunction<_FUNCTION_, _ARGS_...>(func, args...);
    if (base == NULL) return -3;

    Buffer data(sizeof(base));
    memcpy(data, &base, sizeof(base));
    ret = client.Send(data);
    if (ret != 0) {
      delete base;
      return -4;
    }
    return 0;
  }
  size_t Size() const { return m_threads.size(); }

 private:
  int TaskDispatch() {
    while (m_epoll != -1) {
      EPEvents events;
      int ret = 0;
      ssize_t esize = m_epoll.WaitEvent(events);
      for (ssize_t i = 0; i < esize; ++i) {
        if (events[i].events & EPOLLIN) {
          CSocketBase *pClient = NULL;

          if (events[i].data.ptr == m_server) {
            m_server->Link(&pClient);
            if (ret != 0) continue;
            ret = m_epoll.Add(*pClient, EpollData((void *)pClient));
            if (ret != 0) {
              delete pClient;
              continue;
            }
          } else {
            // 客户端收到数据
            pClient = (CSocketBase *)events[i].data.ptr;
            CFunctionBase *base = NULL;
            Buffer data(sizeof(base));
            ret = pClient->Recv(data);
            if (ret <= 0) {
              m_epoll.Del(*pClient);
              delete pClient;
              continue;
            }
            memcpy(&base, (char *)data, sizeof(base));
            if (base != NULL) {
              (*base)();
              delete base;
            }
          }
        }
      }
    }
    return 0;
  }

 private:
  CEpoll m_epoll;
  CSocketBase *m_server;
  std::vector<CThread *> m_threads;
  Buffer m_path;
};

#endif