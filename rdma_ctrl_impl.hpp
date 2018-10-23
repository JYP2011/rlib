#include <pthread.h>
#include <map>
#include <mutex>

namespace rdmaio {

class RdmaCtrl::RdmaCtrlImpl {
 public:
  RdmaCtrlImpl(int node_id, int tcp_base_port,std::string local_ip):
      lock_(),
      tcp_base_port_(tcp_base_port),
      node_id_(node_id),
      local_ip_(local_ip)
  {
    // start the background thread to handle QP connection request
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_create(&handler_tid_, &attr, &RdmaCtrlImpl::connection_handler_wrapper,this);
  }

  ~RdmaCtrlImpl() {
    running_ = false; // wait for the handler to join
    pthread_join(handler_tid_,NULL);
    RDMA_LOG(LOG_INFO) << "rdma controler close: does not handle any future connections.";
  }

  RNicHandler *open_device(DevIdx idx) {

    // already openend device
    if(rnic_instance() != nullptr)
      return rnic_instance();

    struct ibv_device **dev_list = nullptr; struct ibv_context *ib_ctx = nullptr; struct ibv_pd *pd = nullptr; int num_devices;
    int rc;  // return code

    dev_list = ibv_get_device_list(&num_devices);

    if(idx.dev_id >= num_devices || idx.dev_id < 0) {
      RDMA_LOG(LOG_WARNING) << "wrong dev_id: " << idx.dev_id << "; total " << num_devices <<" found";
      goto OPEN_END;
    }

    // alloc ctx
    ib_ctx = ibv_open_device(dev_list[idx.dev_id]);
    if(ib_ctx == nullptr) {
      RDMA_LOG(LOG_WARNING) << "failed to open ib ctx w error: " << strerror(errno);
      goto OPEN_END;
    }

    // alloc pd
    pd = ibv_alloc_pd(ib_ctx);
    if(pd == nullptr) {
      RDMA_LOG(LOG_WARNING) << "failed to alloc pd w error: " << strerror(errno);
      RDMA_VERIFY(LOG_INFO,ibv_close_device(ib_ctx) == 0) << "failed to close device " << idx.dev_id;
      goto OPEN_END;
    }

    // fill the lid
    ibv_port_attr port_attr;
    rc = ibv_query_port (ib_ctx, idx.port_id, &port_attr);
    if(rc < 0) {
      RDMA_LOG(LOG_WARNING) << "failed to query port status w error: " << strerror(errno);
      RDMA_VERIFY(LOG_INFO,ibv_close_device(ib_ctx) == 0) << "failed to close device " << idx.dev_id;
      RDMA_VERIFY(LOG_INFO,ibv_dealloc_pd(pd) == 0) << "failed to dealloc pd";
      goto OPEN_END;
    }

    // success open
    {
      RNicHandler *h = new RNicHandler(idx.dev_id,idx.port_id,ib_ctx,pd,port_attr.lid);
      RDMA_ASSERT(h != NULL);
      rnic_instance() = h;
    }

 OPEN_END:
    if(dev_list != nullptr)
      ibv_free_device_list(dev_list);
    return rnic_instance();
  }

  RCQP *get_rc_qp(QPIdx idx) {
    lock_.lock();
    uint64_t qid = get_rc_key(idx);
    if(qps_.find(qid) == qps_.end()) {
      lock_.unlock();
      return nullptr;
    }
    QP *res = qps_[qid];
    lock_.unlock();
    return dynamic_cast<RCQP *>(res);
  }

  UDQP *get_ud_qp(QPIdx idx) {
    lock_.lock();
    uint64_t qid = get_ud_key(idx);
    if(qps_.find(qid) == qps_.end()) {
      lock_.unlock();
      return nullptr;
    }
    QP *res = qps_[qid];
    lock_.unlock();
    return dynamic_cast<UDQP *>(res);
  }

  RCQP *create_rc_qp(QPIdx idx, RNicHandler *dev,MemoryAttr *attr) {

    RCQP *res = nullptr;
    lock_.lock();

    uint64_t qid = get_rc_key(idx);

    if(qps_.find(qid) != qps_.end()) {
      res = dynamic_cast<RCQP *>(qps_[qid]);
      goto END;
    }

    if(attr == NULL)
      res = new RCQP(dev,idx);
    else
      res = new RCQP(dev,idx,*attr);
    qps_.insert(std::make_pair(qid,res));
 END:
    lock_.unlock();
    return res;
  }

  UDQP *create_ud_qp(QPIdx idx, RNicHandler *dev,MemoryAttr *attr) {

    UDQP *res = nullptr;
    lock_.lock();
    uint64_t qid = get_ud_key(idx);

    if(qps_.find(qid) != qps_.end()) {
      res = dynamic_cast<UDQP *>(qps_[qid]);
      RDMA_LOG(LOG_WARNING) << "create an existing UD QP:" << idx.worker_id << " " << idx.index;
      goto END;
    }
    if(attr == NULL)
      res = new UDQP(dev,idx);
    else
      res = new UDQP(dev,idx,*attr);
    qps_.insert(std::make_pair(qid,res));
 END:
    lock_.unlock();
    return res;
  }

  bool register_memory(int mr_id,char *buf,uint64_t size,RNicHandler *rnic,int flag) {

    Memory *m = new Memory(buf,size,rnic->pd,flag);
    if(!m->valid()) {
      RDMA_LOG(LOG_WARNING) << "register mr to rnic error: " << strerror(errno);
      delete m;
      return false;
    }

    lock_.lock();
    if(mrs_.find(mr_id) != mrs_.end()) {
      RDMA_LOG(LOG_WARNING) << "mr " << mr_id << " has already been registered!";
      delete m;
    } else {
      mrs_.insert(std::make_pair(mr_id,m));
    }
    lock_.unlock();

    return true;
  }

  MemoryAttr get_local_mr(int mr_id) {
    MemoryAttr attr;
    lock_.lock();
    if(mrs_.find(mr_id) != mrs_.end())
      attr = mrs_[mr_id]->rattr;
    lock_.unlock();
    return attr;
  }

  void clear_dev_info() {
    cached_infos_.clear();
  }

  std::vector<RNicInfo>  query_devs() {

    int num_devices = 0;   struct ibv_device **dev_list = nullptr;

    if(cached_infos_.size() != 0) {
      //RDMA_LOG(LOG_INFO) << "use cached device info. If not wanted, use clear_dev_info(); ";
      num_devices = cached_infos_.size();
      goto QUERY_END;
    }
    { // query the device and its active ports using the underlying APIs
      dev_list = ibv_get_device_list(&num_devices);
      int temp_devices = num_devices;

      if(dev_list == nullptr) {
        RDMA_LOG(LOG_ERROR) << "cannot get ib devices.";
        num_devices = 0;
        goto QUERY_END;
      }

      for(uint dev_id = 0;dev_id < temp_devices;++dev_id) {

        struct ibv_context *ib_ctx = ibv_open_device(dev_list[dev_id]);
        if(ib_ctx == nullptr) {
          RDMA_LOG(LOG_ERROR) << "open dev " << dev_id << " error: " << strerror(errno) << " ignored";
          num_devices -= 1;
          continue;
        }
        cached_infos_.emplace_back(ibv_get_device_name(ib_ctx->device),dev_id,ib_ctx);
     QUERY_DEV_END:
        // close ib_ctx
        RDMA_VERIFY(LOG_INFO,ibv_close_device(ib_ctx) == 0) << "failed to close device " << dev_id;
      }
    }

 QUERY_END:
    if(dev_list != nullptr)
      ibv_free_device_list(dev_list);
    return std::vector<RNicInfo>(cached_infos_.begin(),cached_infos_.end());
  }

  /**
   * convert qp idx(node,worker,idx) -> key
   */
  uint32_t get_rc_key (const QPIdx idx) {
    return ::rdmaio::encode_qp_id(idx.node_id,RC_ID_BASE + idx.worker_id * 64 + idx.index);
  }

  uint32_t get_ud_key(const QPIdx idx) {
    return ::rdmaio::encode_qp_id(idx.worker_id,UD_ID_BASE + idx.index);
  }

  RdmaCtrl::DevIdx convert_port_idx(int idx) {

    if(cached_infos_.size() == 0)
      query_devs();

    for(int dev_id = 0; dev_id < cached_infos_.size();++dev_id) {

      int port_num = cached_infos_[dev_id].active_ports.size();

      for(int port_id = 1; port_id <= port_num; port_id++) {
        if(idx == 0) {
          // find one
          return DevIdx {.dev_id = dev_id,.port_id = port_id};
        }
        idx -= 1;
      }
    }
    // failed to find the dev according to the idx
    return DevIdx {.dev_id = -1,.port_id = -1};
  }

  RNicHandler *get_device() { return rnic_instance();}

  void close_device() { if(rnic_instance() != nullptr) delete rnic_instance(); rnic_instance() = nullptr;}

  void close_device(RNicHandler *rnic) { if(rnic != nullptr) delete rnic; }

  static void *connection_handler_wrapper(void *context)
  {
    return ((RdmaCtrlImpl *)context)->connection_handler();
  }

  /**
   * Using TCP to connect in-coming QP & MR requests
   */
  void *connection_handler(void) {

    pthread_detach(pthread_self());

    auto listenfd = PreConnector::get_listen_socket(local_ip_,tcp_base_port_);

    int opt = 1;
    RDMA_VERIFY(LOG_ERROR,setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR | SO_REUSEPORT,&opt,sizeof(int)) == 0)
        << "unable to configure socket status.";
    RDMA_VERIFY(LOG_ERROR,listen(listenfd,24) == 0) << "TCP listen error: " << strerror(errno);

    while(running_) {

      asm volatile("" ::: "memory");

      struct sockaddr_in cli_addr = {0};
      socklen_t clilen = sizeof(cli_addr);
      auto csfd = accept(listenfd,(struct sockaddr *) &cli_addr, &clilen);

      if(csfd < 0) {
        RDMA_LOG(LOG_ERROR) << "accept a wrong connection error: " << strerror(errno);
        continue;
      }

      if(!PreConnector::wait_recv(csfd,6000)) { RDMA_LOG(3) << "wrong";
        close(csfd);
        continue;
      }

      ConnArg arg;
      auto n = recv(csfd,(char *)(&arg),sizeof(ConnArg), MSG_WAITALL);

      if(n != sizeof(ConnArg)) {
        // an invalid message
        close(csfd);
        continue;
      }

      ConnReply reply; reply.ack = ERR;

      lock_.lock();
      switch(arg.type) {
        case ConnArg::MR:
          if(mrs_.find(arg.payload.mr.mr_id) != mrs_.end()) {
            memcpy((char *)(&(reply.payload.mr)),
                   (char *)(&(mrs_[arg.payload.mr.mr_id]->rattr)),sizeof(MemoryAttr));
            reply.ack = SUCC;
          }
          break;
        case ConnArg::QP: {
          QP *qp = NULL;
          switch(arg.payload.qp.qp_type) {
            case IBV_QPT_UD:
              {
              lock_.unlock();
              UDQP *ud_qp = get_ud_qp(create_ud_idx(arg.payload.qp.from_node,arg.payload.qp.from_worker));
              if(ud_qp != nullptr && ud_qp->ready()) {
                qp = ud_qp;
              }
              lock_.lock();
              }
              break;
            case IBV_QPT_RC:
              {
                lock_.unlock();
                QP *rc_qp = get_rc_qp(create_rc_idx(arg.payload.qp.from_node,arg.payload.qp.from_worker));
                qp = rc_qp;
                lock_.lock();
              }
              break;
            default:
              RDMA_LOG(LOG_ERROR) << "unknown QP connection type: " << arg.payload.qp.qp_type;
          }
          if(qp != nullptr) {
            reply.payload.qp = qp->get_attr();
            reply.ack = SUCC;
          }
          reply.payload.qp.node_id = node_id_;
          break;
        }
        default:
          RDMA_LOG(LOG_WARNING) << "received unknown connect type " << arg.type;
      }
      lock_.unlock();

      PreConnector::send_to(csfd,(char *)(&reply),sizeof(ConnReply));
      PreConnector::wait_close(csfd); // wait for the client to close the connection
    }
    // end of the server
    close(listenfd);
  }

 private:
  friend class RdmaCtrl;
  static RNicHandler* &rnic_instance() {
    static thread_local RNicHandler * handler = NULL;
    return handler;
  }

  std::vector<RNicInfo> cached_infos_;

  // registered MRs at this control manager
  std::map<int,Memory *>      mrs_;

  // created QPs on this control manager
  std::map<uint64_t,QP *> qps_;

  // local node information
  const int node_id_;
  const int tcp_base_port_;
  const std::string local_ip_;

  std::mutex lock_;

  pthread_t handler_tid_;
  bool running_ = true;

  bool link_symmetric_rcqps(const std::vector<std::string> &cluster,int l_mrid,int mr_id,int wid,int idx) {

    std::vector<bool> ready_list(cluster.size(),false);
    std::vector<MemoryAttr> mrs;

    MemoryAttr local_mr = get_local_mr(l_mrid);

    for(auto s : cluster) {
      // get the target mr
   retry:
      MemoryAttr mr;
      auto rc = QP::get_remote_mr(s,tcp_base_port_,mr_id,&mr);
      if(rc != SUCC) {
        usleep(2000);
        goto retry;
      }
      mrs.push_back(mr);
    }
    while(true) {
      int connected = 0, i = 0;
      for(auto s : cluster) {

        if(ready_list[i]) {
          i++; connected++;
          continue;
        }
        RCQP *qp = create_rc_qp(QPIdx { .node_id = i,.worker_id = wid,.index = idx },
                                get_device(),&local_mr);
        RDMA_ASSERT(qp != nullptr);

        qp->bind_remote_mr(mrs[i]);

        if(qp->connect(s,tcp_base_port_) == SUCC) {
          ready_list[i++] = true;
          connected++;
        }
      }
      if(connected == cluster.size())
        break;
      else
        usleep(1000);
    }
    return true; // This example does not use error handling
  }
}; //

// link to the main class
inline __attribute__ ((always_inline))
RdmaCtrl::RdmaCtrl(int node_id, int tcp_base_port,std::string ip)
    :impl_(new RdmaCtrlImpl(node_id,tcp_base_port,ip)){
}

inline __attribute__ ((always_inline))
RdmaCtrl::~RdmaCtrl() {
  impl_.reset();
}

inline __attribute__ ((always_inline))
std::vector<RNicInfo> RdmaCtrl::query_devs() {
  return impl_->query_devs();
}

inline __attribute__ ((always_inline))
void RdmaCtrl::clear_dev_info() {
  return impl_->clear_dev_info();
}

inline __attribute__ ((always_inline))
RNicHandler *RdmaCtrl::get_device() {
  return impl_->get_device();
}

inline __attribute__ ((always_inline))
RNicHandler *RdmaCtrl::open_device(DevIdx idx) {
  return impl_->open_device(idx);
}

inline __attribute__ ((always_inline))
void RdmaCtrl::close_device() {
  return impl_->close_device();
}

inline __attribute__ ((always_inline))
void RdmaCtrl::close_device(RNicHandler *rnic) {
  return impl_->close_device(rnic);
}

inline __attribute__ ((always_inline))
RdmaCtrl::DevIdx RdmaCtrl::convert_port_idx(int idx) {
  return impl_->convert_port_idx(idx);
}

inline __attribute__ ((always_inline))
bool RdmaCtrl::register_memory(int id,char *buf,uint64_t size,RNicHandler *rnic,int flag) {
  return impl_->register_memory(id,buf,size,rnic,flag);
}

inline __attribute__ ((always_inline))
MemoryAttr RdmaCtrl::get_local_mr(int mr_id) {
  return impl_->get_local_mr(mr_id);
}

inline __attribute__ ((always_inline))
RCQP *RdmaCtrl::create_rc_qp(QPIdx idx, RNicHandler *dev,MemoryAttr *attr) {
  return impl_->create_rc_qp(idx,dev,attr);
}

inline __attribute__ ((always_inline))
UDQP *RdmaCtrl::create_ud_qp(QPIdx idx, RNicHandler *dev,MemoryAttr *attr) {
  return impl_->create_ud_qp(idx,dev,attr);
}

inline __attribute__ ((always_inline))
RCQP *RdmaCtrl::get_rc_qp(QPIdx idx) {
  return impl_->get_rc_qp(idx);
}

inline __attribute__ ((always_inline))
UDQP *RdmaCtrl::get_ud_qp(QPIdx idx) {
  return impl_->get_ud_qp(idx);
}

inline __attribute__ ((always_inline))
int RdmaCtrl::current_node_id() {
  return impl_->node_id_;
}

inline __attribute__ ((always_inline))
int RdmaCtrl::listening_port() {
  return impl_->tcp_base_port_;
}

inline __attribute__ ((always_inline))
bool RdmaCtrl::link_symmetric_rcqps(const std::vector<std::string> &cluster,
                                    int l_mrid,int mr_id,int wid,int idx) {
  return impl_->link_symmetric_rcqps(cluster,l_mrid,mr_id,wid,idx);
}

};
