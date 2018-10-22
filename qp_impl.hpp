#pragma once

#include "pre_connector.hpp"

namespace rdmaio {

const int MAX_INLINE_SIZE = 64;

/**
 * These are magic numbers, served as the keys / identifications
 * Currently we not allow user defined keys, but can simply added
 */
const uint32_t DEFAULT_QKEY    = 0x111111;
const uint32_t DEFAULT_PSN     = 3185;

/**
 * QP encoder, provde a default naming to identity QPs
 */
enum  {
  RC_ID_BASE = 0,
  UC_ID_BASE = 10000,
  UD_ID_BASE = 20000
};

inline constexpr uint32_t index_mask() {
  return 0xffff;
}

inline uint32_t mac_mask() {
  return ::rdmaio::index_mask() << 16;
}

inline uint32_t encode_qp_id(int m,int idx) {
  return static_cast<uint32_t>(static_cast<uint32_t>(m) << 16) | static_cast<uint32_t>(idx);
}

inline uint32_t decode_qp_mac(uint32_t key) {
  return (key & ::rdmaio::mac_mask()) >> 16;
}

inline uint32_t decode_qp_index(uint32_t key) {
  return key & ::rdmaio::index_mask();
}

class QPImpl {
 public:
  QPImpl() = default;
  ~QPImpl() = default;

  static ConnStatus get_remote_helper(ConnArg *arg, ConnReply *reply,std::string ip,int port) {

    ConnStatus ret = SUCC;

    auto socket = PreConnector::get_send_socket(ip,port);
    if(socket < 0) {
      return ERR;
    }

    auto n = send(socket,(char *)(arg),sizeof(ConnArg),0);

    if(n != sizeof(ConnArg)) {
      ret = ERR; goto CONN_END;
    }

    // receive reply
    if(!PreConnector::wait_recv(socket,10000)) {
      ret = TIMEOUT; goto CONN_END;
    }

    n = recv(socket,(char *)((reply)), sizeof(ConnReply), MSG_WAITALL);
    if(n != sizeof(ConnReply)) {
      ret = ERR; goto CONN_END;
    }
    if(reply->ack != SUCC) {
      ret = NOT_READY; goto CONN_END;
    }
 CONN_END:
    shutdown(socket,SHUT_RDWR);
    close(socket);
    return ret;
  }

  static ConnStatus get_remote_mr(std::string ip,int port,int mr_id,MemoryAttr *attr) {

    ConnArg arg; ConnReply reply;
    arg.type = ConnArg::MR;
    arg.payload.mr.mr_id = mr_id;

    auto ret = get_remote_helper(&arg,&reply,ip,port);
    if(ret == SUCC) {
      attr->key = reply.payload.mr.key;
      attr->buf = reply.payload.mr.buf;
    }
    return ret;
  }

  static ConnStatus poll_till_completion(ibv_cq *cq,ibv_wc &wc, struct timeval timeout) {

    struct timeval start_time; gettimeofday (&start_time, NULL);
    int poll_result = 0; int64_t diff;
    do {
      poll_result = ibv_poll_cq (cq, 1, &wc);

      struct timeval cur_time; gettimeofday(&cur_time,NULL);
      diff = diff_time(cur_time,start_time);

    } while( (poll_result == 0) && (diff <= (timeout.tv_sec * 1000 + timeout.tv_usec)));

    if(poll_result == 0) {
      RDMA_ASSERT(false);
      return TIMEOUT;
    }
    if(poll_result < 0) {
      RDMA_ASSERT(false);
      return ERR;
    }
    RDMA_ASSERT(wc.status == IBV_WC_SUCCESS);
    return SUCC;
  }
};

class RCQPImpl {
 public:
  RCQPImpl()  = default;
  ~RCQPImpl() = default;

  static const int RC_MAX_SEND_SIZE = 128;
  static const int RC_MAX_RECV_SIZE = 512;

  static void ready2init(ibv_qp *qp,RNicHandler *rnic,int flag) {

    struct ibv_qp_attr qp_attr = {};
    qp_attr.qp_state = IBV_QPS_INIT;
    qp_attr.pkey_index = 0;
    qp_attr.port_num = rnic->port_id;
    qp_attr.qp_access_flags = flag;

    int flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS;
    int rc = ibv_modify_qp(qp, &qp_attr,flags);
    RDMA_VERIFY(LOG_WARNING,rc == 0) <<  "Failed to modify RC to INIT state, %s\n" <<  strerror(errno);

    if(rc != 0) {
      // error handling
      RDMA_LOG(LOG_WARNING) << " change state to init failed. ";
    }
  }

  static bool ready2rcv(ibv_qp *qp,QPAttr &attr,RNicHandler *rnic) {

    struct ibv_qp_attr qp_attr = {};

    qp_attr.qp_state = IBV_QPS_RTR;
    qp_attr.path_mtu = IBV_MTU_4096;
    qp_attr.dest_qp_num = attr.qpn;
    qp_attr.rq_psn = DEFAULT_PSN;
    qp_attr.max_dest_rd_atomic = 16;
    qp_attr.min_rnr_timer = 20;

    qp_attr.ah_attr.dlid = attr.lid;
    qp_attr.ah_attr.sl = 0;
    qp_attr.ah_attr.src_path_bits = 0;
    qp_attr.ah_attr.port_num = rnic->port_id; /* Local port! */

    qp_attr.ah_attr.is_global = 1;
    qp_attr.ah_attr.grh.dgid.global.subnet_prefix = attr.addr.subnet_prefix;
    qp_attr.ah_attr.grh.dgid.global.interface_id = attr.addr.interface_id;
    qp_attr.ah_attr.grh.sgid_index = 0;
    qp_attr.ah_attr.grh.flow_label = 0;
    qp_attr.ah_attr.grh.hop_limit = 255;

    int flags = IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN
                | IBV_QP_MAX_DEST_RD_ATOMIC | IBV_QP_MIN_RNR_TIMER;
    auto rc = ibv_modify_qp(qp, &qp_attr,flags);
    return rc == 0;
  }

  static bool ready2send(ibv_qp *qp) {

    int rc, flags;
    struct ibv_qp_attr qp_attr = {};

    qp_attr.qp_state = IBV_QPS_RTS;
    qp_attr.sq_psn = DEFAULT_PSN;
    qp_attr.timeout = 21; // we use a longer timeout
    qp_attr.retry_cnt = 7;
    qp_attr.rnr_retry = 7;
    qp_attr.max_rd_atomic = 16;
    qp_attr.max_dest_rd_atomic = 16;

    flags = IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
            IBV_QP_MAX_QP_RD_ATOMIC;
    rc = ibv_modify_qp(qp, &qp_attr,flags);
    return rc == 0;
  }

  static void init(ibv_qp *&qp,ibv_cq *&cq,RNicHandler *rnic,int flags) {

    // create the CQ
    cq = ibv_create_cq(rnic->ctx, RC_MAX_SEND_SIZE, NULL, NULL, 0);
    RDMA_VERIFY(LOG_WARNING,cq != NULL) << "create cq error: " << strerror(errno);

    // create the QP
    struct ibv_qp_init_attr qp_init_attr = {};

    qp_init_attr.send_cq = cq;
    qp_init_attr.recv_cq = cq; // TODO, need seperate handling for two-sided over RC QP
    qp_init_attr.qp_type = IBV_QPT_RC;

    qp_init_attr.cap.max_send_wr = RC_MAX_SEND_SIZE;
    qp_init_attr.cap.max_recv_wr = RC_MAX_RECV_SIZE;	/* Can be set to 1, if RC Two-sided is not required */
    qp_init_attr.cap.max_send_sge = 1;
    qp_init_attr.cap.max_recv_sge = 1;
    qp_init_attr.cap.max_inline_data = MAX_INLINE_SIZE;

    qp = ibv_create_qp(rnic->pd, &qp_init_attr);
    RDMA_VERIFY(LOG_WARNING,qp != NULL);

    if(qp)
      ready2init(qp,rnic,flags);
  }
};

class UDQPImpl {
 public:
  UDQPImpl() = default;
  ~UDQPImpl() = default;

  static const int UD_MAX_SEND_SIZE = 128;
  static const int UD_MAX_RECV_SIZE = 2048;

  static void init(ibv_qp *&qp,ibv_cq *&cq,ibv_cq *&recv_cq,RNicHandler *rnic) {

    if(qp != nullptr)
      return;

	if((cq = ibv_create_cq(rnic->ctx, UD_MAX_SEND_SIZE, NULL, NULL, 0)) == NULL) {
      RDMA_LOG(LOG_ERROR) << "create send cq for UD QP error: " << strerror(errno);
      return;
    }

	if((recv_cq = ibv_create_cq(rnic->ctx, UD_MAX_RECV_SIZE, NULL, NULL, 0)) == NULL) {
      RDMA_LOG(LOG_ERROR) << "create recv cq for UD QP error: " << strerror(errno);
      return;
    }

	/* Initialize creation attributes */
	struct ibv_qp_init_attr qp_init_attr = {};
	qp_init_attr.send_cq = cq;
	qp_init_attr.recv_cq = recv_cq;
	qp_init_attr.qp_type = IBV_QPT_UD;

	qp_init_attr.cap.max_send_wr = UD_MAX_SEND_SIZE;
	qp_init_attr.cap.max_recv_wr = UD_MAX_RECV_SIZE;
	qp_init_attr.cap.max_send_sge = 1;
	qp_init_attr.cap.max_recv_sge = 1;
	qp_init_attr.cap.max_inline_data = MAX_INLINE_SIZE;

	if((qp = ibv_create_qp(rnic->pd, &qp_init_attr)) == nullptr) {
      RDMA_LOG(LOG_ERROR) << "create send qp for UD QP error: " << strerror(errno);
      return;
    }

    // change QP status
	ready2init(qp, rnic); // shall always succeed
#if 1
	if(!ready2rcv(qp,rnic)) {
      RDMA_LOG(LOG_WARNING) << "change ud qp to ready to recv error: " << strerror(errno);
    }
#endif
#if 2
	if(!ready2send(qp)) {
      RDMA_LOG(LOG_WARNING) << "change ud qp to ready to send error: " << strerror(errno);
    }
#endif
  }

  static void ready2init(ibv_qp *qp,RNicHandler *rnic) {

	int rc, flags = IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT | IBV_QP_QKEY;
	struct ibv_qp_attr qp_attr = {};
	qp_attr.qp_state = IBV_QPS_INIT;
	qp_attr.pkey_index = 0;
	qp_attr.port_num = rnic->port_id;
	qp_attr.qkey = DEFAULT_QKEY;

	if((rc = ibv_modify_qp(qp, &qp_attr, flags)) != 0) {
      RDMA_LOG(LOG_WARNING) << "modify ud qp to init error: " << strerror(errno);
    }
  }

  static bool ready2rcv(ibv_qp *qp,RNicHandler *rnic) {

	int rc, flags = IBV_QP_STATE;
	struct ibv_qp_attr qp_attr = {};
	qp_attr.qp_state = IBV_QPS_RTR;

	rc = ibv_modify_qp(qp, &qp_attr, flags);
    return rc == 0;
  }

  static bool ready2send(ibv_qp *qp) {

	int rc, flags = 0;
	struct ibv_qp_attr qp_attr = {};
	qp_attr.qp_state = IBV_QPS_RTS;
	qp_attr.sq_psn = DEFAULT_PSN;

	flags = IBV_QP_STATE | IBV_QP_SQ_PSN;
	rc = ibv_modify_qp(qp, &qp_attr, flags);
    return rc == 0;
  }

  static ibv_ah *create_ah(RNicHandler *rnic,QPAttr &attr) {

    struct ibv_ah_attr ah_attr;
    ah_attr.is_global = 1;
    ah_attr.dlid = attr.lid;
    ah_attr.sl = 0;
    ah_attr.src_path_bits = 0;
    ah_attr.port_num = attr.port_id;

    ah_attr.grh.dgid.global.subnet_prefix = attr.addr.subnet_prefix;
    ah_attr.grh.dgid.global.interface_id = attr.addr.interface_id;
    ah_attr.grh.flow_label = 0;
    ah_attr.grh.hop_limit = 255;
    ah_attr.grh.sgid_index = rnic->gid;

    return ibv_create_ah(rnic->pd, &ah_attr);
  }

};

} // namespace rdmaio