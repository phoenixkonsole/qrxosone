/*
 * qrx_p2p.h - native 68K libp2p peer for QRX IPFS.
 *
 * The peer speaks libp2p/Noise/Yamux over TCP.  It is deliberately a
 * small, synchronous implementation: one TCP connection is handled at a
 * time, and the 68K node does not try to run a full Go-style swarm.
 *
 * Supported application protocols:
 *   /ipfs/bitswap/1.2.0   block wantlists and block transfer
 *   /ipfs/kad/1.0.0       FIND_NODE, GET_PROVIDERS, ADD_PROVIDER
 *
 * The node keeps blocks in its own store.  The caller supplies the
 * blockstore callbacks; qrx_p2p.c never assumes a DOS path.
 */
#ifndef QRX_P2P_H
#define QRX_P2P_H

#include <exec/types.h>

#define QRX_P2P_VERSION        "0.1"
#define QRX_P2P_DEFAULT_PORT   4001
#define QRX_P2P_CONTROL_PORT   5002
#define QRX_P2P_MAX_PEERS      64
#define QRX_P2P_MAX_PROVIDERS  128
#define QRX_P2P_MAX_CID        96
#define QRX_P2P_MAX_HOST       128
#define QRX_P2P_MAX_ADDR       512
#define QRX_P2P_MAX_MESSAGE    (2UL * 1024UL * 1024UL)

struct qrx_p2p_backend {
    /* Return 1 and allocate *data with AllocVec when the block exists. */
    int (*block_get)(const char *cid, unsigned char **data,
                     unsigned long *len);
    /* Store a verified block.  Return 1 on success. */
    int (*block_put)(const char *cid, const unsigned char *data,
                     unsigned long len);
    int (*block_has)(const char *cid);
    /* Copy up to max pinned CIDs into cids (stride QRX_P2P_MAX_CID).
     * Return the number copied. */
    int (*pin_list)(char *cids, int max);
};

struct qrx_p2p;

/* Initialize identity and peer-store below dir.  The identity key is
 * created once and reused.  Returns NULL and a printable reason in err. */
struct qrx_p2p *qrx_p2p_new(const char *dir, int listen_port,
                            const struct qrx_p2p_backend *backend,
                            char *err, unsigned long errcap);
void qrx_p2p_free(struct qrx_p2p *p2p);

const char *qrx_p2p_peer_id(struct qrx_p2p *p2p);
int qrx_p2p_listen_port(struct qrx_p2p *p2p);
int qrx_p2p_listen_fd(struct qrx_p2p *p2p);
int qrx_p2p_listen(struct qrx_p2p *p2p);

/* Read bootstrap multiaddrs from file (one per line) and/or the
 * QRX_IPFS_BOOTSTRAP environment variable (comma or semicolon
 * separated).  Returns the number of valid entries. */
int qrx_p2p_load_bootstrap(struct qrx_p2p *p2p, const char *path);

/* Dial a multiaddr and keep the peer in the peerstore. */
int qrx_p2p_add_peer(struct qrx_p2p *p2p, const char *multiaddr);

/* Accept and serve one incoming TCP connection. */
int qrx_p2p_serve_fd(struct qrx_p2p *p2p, int fd);

/* Connect to known peers, run a DHT query and advertise local pins.
 * Safe to call periodically. */
int qrx_p2p_tick(struct qrx_p2p *p2p);

/* Fetch one block by CID through Bitswap.  The result is returned in
 * *data (AllocVec) and also passed to backend->block_put. */
int qrx_p2p_fetch(struct qrx_p2p *p2p, const char *cid,
                  unsigned char **data, unsigned long *len);

/* Advertise one CID through the DHT. */
int qrx_p2p_provide(struct qrx_p2p *p2p, const char *cid);

/* JSON status for the local HTTP API/control socket. */
int qrx_p2p_status_json(struct qrx_p2p *p2p, char *out, unsigned long cap);

/* Number of peers with a usable TCP address. */
int qrx_p2p_peer_count(struct qrx_p2p *p2p);

#endif
