#ifndef QRX_TESTIMG_H
#define QRX_TESTIMG_H

/*
 * Synthetic Mac images for tests and for the guest test suite.
 *
 * Everything here is built from bytes this project defines: no Apple system
 * files, no ROM contents, no third-party material.  The MFS volume carries
 * three small files, one of them with a resource fork.
 */

#include <stdint.h>

#define QRX_TESTIMG_400K 409600u
#define QRX_TESTIMG_DC42_MAX (QRX_TESTIMG_400K + 84u)
#define QRX_TESTIMG_RSRC_MAX 1024u
#define QRX_TESTIMG_APM_SIZE 1048576u
#define QRX_TESTIMG_HFS800 819200u
#define QRX_TESTIMG_HFSP 2097152u
#define QRX_TESTIMG_HFSP_WRAP (QRX_TESTIMG_HFSP + 4096u)
#define QRX_TESTIMG_FORK_CAP 8192u

/* Builds a 400K MFS volume named "Test Disk" with three files. */
int qrx_testimg_mfs400(uint8_t *out, uint32_t cap, uint32_t *len);

/* Wraps raw sectors in a DiskCopy 4.2 header (no tags, 400K geometry). */
int qrx_testimg_dc42(const uint8_t *raw, uint32_t raw_len, uint8_t *out,
                     uint32_t cap, uint32_t *len);

/* Wraps raw sectors in a DiskCopy 4.2 header but breaks the data checksum. */
int qrx_testimg_dc42_badsum(const uint8_t *raw, uint32_t raw_len, uint8_t *out,
                            uint32_t cap, uint32_t *len);

/* Resource fork with one bcem resource announcing `blocks` 512-byte blocks. */
int qrx_testimg_bcem_rsrc(uint32_t blocks, uint8_t *out, uint32_t cap,
                          uint32_t *len);

/* 1 MiB Apple Partition Map image: map partition plus one MacTest volume. */
int qrx_testimg_apm(const uint8_t *raw, uint32_t raw_len, uint8_t *out,
                    uint32_t cap, uint32_t *len);

/* Builds an 800K HFS volume named "Test Disk": root files Hello, Payload
 * (with a resource fork) and Frag (four extents, the last one only reachable
 * through the extents overflow tree) plus the folder Docs with Note. */
int qrx_testimg_hfs800(uint8_t *out, uint32_t cap, uint32_t *len);

/* Builds a 2 MiB HFS+ volume (hfsx != 0 makes it HFSX with an extra
 * lower-case "hello" next to "Hello").  Files: Hello, Payload (resource
 * fork), Café (resource fork only as com.apple.ResourceFork in the
 * attributes tree), Frag (nine one-block extents, the last one only in the
 * extents overflow tree) and Docs/Note. */
int qrx_testimg_hfsp(uint8_t *out, uint32_t cap, uint32_t *len, int hfsx);

/* Wraps an HFS+ volume in an HFS master directory block (Mac OS 8.1-9). */
int qrx_testimg_hfsp_wrapper(const uint8_t *hfsp, uint32_t hfsp_len,
                             uint8_t *out, uint32_t cap, uint32_t *len);

/* Fork containers carrying the same synthetic file family: AppleSingle
 * (Hello, both forks), AppleDouble (Payload, resource fork only), MacBinary
 * II (Payload, 260-byte data fork padded to 128 bytes) and BinHex 4.0
 * (Runs, data fork with a ten-byte run, five literal 0x90 bytes and then
 * qrx_testimg_payload_byte values). */
int qrx_testimg_applesingle(uint8_t *out, uint32_t cap, uint32_t *len);
int qrx_testimg_appledouble(uint8_t *out, uint32_t cap, uint32_t *len);
int qrx_testimg_macbinary(uint8_t *out, uint32_t cap, uint32_t *len);
int qrx_testimg_binhex(uint8_t *out, uint32_t cap, uint32_t *len);

/* Expected byte of the BinHex data fork; the fork is 260 bytes long. */
uint8_t qrx_testimg_binhex_byte(uint32_t index);

/* Pattern helpers so tests and images agree on the bytes. */
uint8_t qrx_testimg_payload_byte(uint32_t index);
uint8_t qrx_testimg_rsrc_byte(uint32_t index);

#endif /* QRX_TESTIMG_H */
