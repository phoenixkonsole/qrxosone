#ifndef QRX_NET_H
#define QRX_NET_H
int qrx_net_http_get(const char *url,char *out,int cap,char *status,int statuscap);
int qrx_net_available(void);
#endif
