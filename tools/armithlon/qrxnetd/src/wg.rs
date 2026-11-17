//! A WireGuard client in user space: boringtun for the protocol, smoltcp for TCP/IP inside the
//! tunnel. One thread owns both; SOCKS connections reach it through channels.

use anyhow::{Context, Result, anyhow, bail};
use base64::Engine;
use boringtun::noise::{Tunn, TunnResult};
use boringtun::x25519::{PublicKey, StaticSecret};
use smoltcp::iface::{Config, Interface, SocketHandle, SocketSet};
use smoltcp::phy::{Device, DeviceCapabilities, Medium, RxToken, TxToken};
use smoltcp::socket::{tcp, udp};
use smoltcp::time::Instant as SmolInstant;
use smoltcp::wire::{HardwareAddress, IpAddress, IpCidr, IpEndpoint};
use std::collections::{HashMap, VecDeque};
use std::net::{Ipv4Addr, SocketAddr, UdpSocket};
use std::sync::mpsc;
use std::time::{Duration, Instant};
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::UnixStream;
use tokio::sync::{oneshot, watch};

/// A connection inside the tunnel, seen from the SOCKS side
pub struct Conn {
    to_tunnel: mpsc::Sender<Vec<u8>>,           // empty: the client closed
    from_tunnel: tokio::sync::mpsc::Receiver<Vec<u8>>,
}

enum Cmd {
    Connect(Ipv4Addr, u16, oneshot::Sender<Result<Conn>>),
    Resolve(String, oneshot::Sender<Result<Ipv4Addr>>),
}

#[derive(Clone)]
pub struct Handle {
    cmd: mpsc::Sender<Cmd>,
    ready: watch::Receiver<bool>,
}

impl Handle {
    pub async fn wait_ready(&self, limit: Duration) -> Result<()> {
        let mut r = self.ready.clone();
        tokio::time::timeout(limit, async {
            while !*r.borrow() {
                if r.changed().await.is_err() {
                    break;
                }
            }
        })
        .await
        .map_err(|_| anyhow!("no WireGuard handshake: check the keys and the endpoint"))?;
        if *self.ready.borrow() { Ok(()) } else { bail!("WireGuard stopped") }
    }
    pub async fn connect(&self, ip: Ipv4Addr, port: u16) -> Result<Conn> {
        let (tx, rx) = oneshot::channel();
        self.cmd.send(Cmd::Connect(ip, port, tx)).map_err(|_| anyhow!("tunnel gone"))?;
        rx.await?
    }
    pub async fn resolve(&self, name: String) -> Result<Ipv4Addr> {
        if let Ok(ip) = name.parse::<Ipv4Addr>() {
            return Ok(ip);
        }
        let (tx, rx) = oneshot::channel();
        self.cmd.send(Cmd::Resolve(name, tx)).map_err(|_| anyhow!("tunnel gone"))?;
        rx.await?
    }
}

/// Copies between a SOCKS client and a tunnel connection until either side ends
pub async fn pump(s: UnixStream, mut conn: Conn) {
    let (mut rd, mut wr) = s.into_split();
    let to = conn.to_tunnel.clone();
    let up = tokio::spawn(async move {
        let mut buf = vec![0u8; 16384];
        loop {
            match rd.read(&mut buf).await {
                Ok(0) | Err(_) => {
                    let _ = to.send(Vec::new());
                    break;
                }
                Ok(n) => {
                    if to.send(buf[..n].to_vec()).is_err() {
                        break;
                    }
                }
            }
        }
    });
    while let Some(data) = conn.from_tunnel.recv().await {
        if data.is_empty() || wr.write_all(&data).await.is_err() {
            break;
        }
    }
    let _ = wr.shutdown().await;
    up.abort();
}

fn key(cfg: &HashMap<String, String>, name: &str) -> Result<[u8; 32]> {
    let v = cfg.get(name).ok_or_else(|| anyhow!("{name} missing"))?;
    let b = base64::engine::general_purpose::STANDARD.decode(v).with_context(|| format!("{name}: not base64"))?;
    b.try_into().map_err(|_| anyhow!("{name}: not 32 bytes"))
}

pub fn start(cfg: &HashMap<String, String>) -> Result<Handle> {
    let private = StaticSecret::from(key(cfg, "private_key")?);
    let peer = PublicKey::from(key(cfg, "peer_public_key")?);
    let psk = if cfg.contains_key("preshared_key") { Some(key(cfg, "preshared_key")?) } else { None };
    let address: Ipv4Addr = cfg.get("address").ok_or_else(|| anyhow!("address missing"))?
        .split('/').next().unwrap().parse().context("address")?;
    let dns: Ipv4Addr = cfg.get("dns").map(String::as_str).unwrap_or("1.1.1.1").parse().context("dns")?;
    // an address, so that no name lookup leaves the Mac outside the tunnel
    let endpoint: SocketAddr = cfg.get("endpoint").ok_or_else(|| anyhow!("endpoint missing"))?
        .parse().context("endpoint must be IP:port")?;
    let keepalive = cfg.get("keepalive").and_then(|k| k.parse::<u16>().ok());
    let tunn = Tunn::new(private, peer, psk, keepalive, 0, None);
    let udp = UdpSocket::bind(if endpoint.is_ipv4() { "0.0.0.0:0" } else { "[::]:0" })?;
    udp.connect(endpoint)?;
    udp.set_nonblocking(true)?;
    let (cmd_tx, cmd_rx) = mpsc::channel();
    let (ready_tx, ready_rx) = watch::channel(false);
    std::thread::Builder::new().name("wireguard".into()).spawn(move || {
        run(tunn, udp, address, dns, cmd_rx, ready_tx);
    })?;
    Ok(Handle { cmd: cmd_tx, ready: ready_rx })
}

/// smoltcp's view of the tunnel: IP packets in and out
struct Dev {
    rx: VecDeque<Vec<u8>>,
    tx: VecDeque<Vec<u8>>,
}
struct Rx(Vec<u8>);
struct Tx<'a>(&'a mut VecDeque<Vec<u8>>);
impl RxToken for Rx {
    fn consume<R, F: FnOnce(&[u8]) -> R>(self, f: F) -> R {
        f(&self.0)
    }
}
impl<'a> TxToken for Tx<'a> {
    fn consume<R, F: FnOnce(&mut [u8]) -> R>(self, len: usize, f: F) -> R {
        let mut b = vec![0u8; len];
        let r = f(&mut b);
        self.0.push_back(b);
        r
    }
}
impl Device for Dev {
    type RxToken<'a> = Rx where Self: 'a;
    type TxToken<'a> = Tx<'a> where Self: 'a;
    fn receive(&mut self, _: SmolInstant) -> Option<(Rx, Tx<'_>)> {
        let p = self.rx.pop_front()?;
        Some((Rx(p), Tx(&mut self.tx)))
    }
    fn transmit(&mut self, _: SmolInstant) -> Option<Tx<'_>> {
        Some(Tx(&mut self.tx))
    }
    fn capabilities(&self) -> DeviceCapabilities {
        let mut c = DeviceCapabilities::default();
        c.medium = Medium::Ip;
        c.max_transmission_unit = 1420;
        c
    }
}

struct TcpConn {
    handle: SocketHandle,
    from_client: mpsc::Receiver<Vec<u8>>,
    to_client: tokio::sync::mpsc::Sender<Vec<u8>>,
    pending: VecDeque<u8>,
    reply: Option<oneshot::Sender<Result<Conn>>>,
    conn: Option<Conn>,
    client_closed: bool,
    started: Instant,
}
struct DnsQuery {
    handle: SocketHandle,
    id: u16,
    name: String,
    sent: Option<Instant>,
    tries: u32,
    reply: oneshot::Sender<Result<Ipv4Addr>>,
}

fn dns_query(id: u16, name: &str) -> Vec<u8> {
    let mut q = vec![(id >> 8) as u8, id as u8, 1, 0, 0, 1, 0, 0, 0, 0, 0, 0];
    for label in name.trim_end_matches('.').split('.') {
        q.push(label.len() as u8);
        q.extend_from_slice(label.as_bytes());
    }
    q.extend_from_slice(&[0, 0, 1, 0, 1]);
    q
}
fn dns_answer(p: &[u8], id: u16) -> Option<Ipv4Addr> {
    if p.len() < 12 || u16::from_be_bytes([p[0], p[1]]) != id || p[3] & 0x0F != 0 {
        return None;
    }
    let (qd, an) = (u16::from_be_bytes([p[4], p[5]]), u16::from_be_bytes([p[6], p[7]]));
    let mut i = 12;
    let skip_name = |p: &[u8], mut i: usize| -> Option<usize> {
        loop {
            let l = *p.get(i)? as usize;
            if l == 0 { return Some(i + 1); }
            if l & 0xC0 == 0xC0 { return Some(i + 2); }
            i += l + 1;
        }
    };
    for _ in 0..qd {
        i = skip_name(p, i)? + 4;
    }
    for _ in 0..an {
        i = skip_name(p, i)?;
        let t = u16::from_be_bytes([*p.get(i)?, *p.get(i + 1)?]);
        let len = u16::from_be_bytes([*p.get(i + 8)?, *p.get(i + 9)?]) as usize;
        i += 10;
        if t == 1 && len == 4 {
            return Some(Ipv4Addr::new(p[i], p[i + 1], p[i + 2], p[i + 3]));
        }
        i += len;
    }
    None
}

fn send_net(udp: &UdpSocket, r: TunnResult) {
    if let TunnResult::WriteToNetwork(p) = r {
        let _ = udp.send(p);
    }
}

fn run(mut tunn: Tunn, udp: UdpSocket, address: Ipv4Addr, dns: Ipv4Addr, cmds: mpsc::Receiver<Cmd>, ready: watch::Sender<bool>) {
    let mut dev = Dev { rx: VecDeque::new(), tx: VecDeque::new() };
    let start = Instant::now();
    let now = || SmolInstant::from_millis(start.elapsed().as_millis() as i64);
    let mut iface = Interface::new(Config::new(HardwareAddress::Ip), &mut dev, now());
    iface.update_ip_addrs(|a| {
        let _ = a.push(IpCidr::new(IpAddress::Ipv4(address), 32));
    });
    let _ = iface.routes_mut().add_default_ipv4_route(address);
    let mut sockets = SocketSet::new(vec![]);
    let mut conns: Vec<TcpConn> = Vec::new();
    let mut queries: Vec<DnsQuery> = Vec::new();
    let mut next_port: u16 = 40000;
    let mut next_id: u16 = 1;
    let mut buf = vec![0u8; 65536];
    let mut out = vec![0u8; 65536];
    let mut last_timer = Instant::now();
    send_net(&udp, tunn.format_handshake_initiation(&mut out, false));

    loop {
        // the server's packets
        loop {
            let n = match udp.recv(&mut buf) {
                Ok(n) => n,
                Err(_) => break,
            };
            let mut r = tunn.decapsulate(None, &buf[..n], &mut out);
            loop {
                match r {
                    TunnResult::WriteToNetwork(p) => {
                        let _ = udp.send(p);
                        r = tunn.decapsulate(None, &[], &mut out);
                    }
                    TunnResult::WriteToTunnelV4(p, _) => {
                        dev.rx.push_back(p.to_vec());
                        break;
                    }
                    _ => break,
                }
            }
        }
        if last_timer.elapsed() >= Duration::from_millis(100) {
            last_timer = Instant::now();
            send_net(&udp, tunn.update_timers(&mut out));
            let up = tunn.time_since_last_handshake().is_some();
            if *ready.borrow() != up {
                let _ = ready.send(up);
            }
        }
        // new connections and lookups
        loop {
            match cmds.try_recv() {
                Ok(Cmd::Connect(ip, port, reply)) => {
                    let mut s = tcp::Socket::new(tcp::SocketBuffer::new(vec![0; 65536]), tcp::SocketBuffer::new(vec![0; 65536]));
                    next_port = if next_port >= 60000 { 40000 } else { next_port + 1 };
                    if s.connect(iface.context(), (IpAddress::Ipv4(ip), port), next_port).is_err() {
                        let _ = reply.send(Err(anyhow!("connect")));
                        continue;
                    }
                    let (to_t, from_client) = mpsc::channel();
                    let (to_client, from_t) = tokio::sync::mpsc::channel(64);
                    conns.push(TcpConn {
                        handle: sockets.add(s), from_client, to_client, pending: VecDeque::new(),
                        reply: Some(reply), conn: Some(Conn { to_tunnel: to_t, from_tunnel: from_t }),
                        client_closed: false, started: Instant::now(),
                    });
                }
                Ok(Cmd::Resolve(name, reply)) => {
                    let mut s = udp::Socket::new(
                        udp::PacketBuffer::new(vec![udp::PacketMetadata::EMPTY; 4], vec![0; 4096]),
                        udp::PacketBuffer::new(vec![udp::PacketMetadata::EMPTY; 4], vec![0; 4096]));
                    next_port = if next_port >= 60000 { 40000 } else { next_port + 1 };
                    let _ = s.bind(next_port);
                    next_id = next_id.wrapping_add(1);
                    queries.push(DnsQuery { handle: sockets.add(s), id: next_id, name, sent: None, tries: 0, reply });
                }
                Err(mpsc::TryRecvError::Empty) => break,
                Err(mpsc::TryRecvError::Disconnected) => return,
            }
        }
        iface.poll(now(), &mut dev, &mut sockets);
        // TCP: the clients' data in, the servers' data out
        conns.retain_mut(|c| {
            let s = sockets.get_mut::<tcp::Socket>(c.handle);
            if c.reply.is_some() {
                if s.state() == tcp::State::Established {
                    let _ = c.reply.take().unwrap().send(Ok(c.conn.take().unwrap()));
                } else if s.state() == tcp::State::Closed || c.started.elapsed() > Duration::from_secs(20) {
                    let _ = c.reply.take().unwrap().send(Err(anyhow!("refused")));
                    return false;
                } else {
                    return true;
                }
            }
            while let Ok(d) = c.from_client.try_recv() {
                if d.is_empty() { c.client_closed = true; } else { c.pending.extend(d); }
            }
            while !c.pending.is_empty() && s.can_send() {
                let (a, _) = c.pending.as_slices();
                match s.send_slice(a) {
                    Ok(n) if n > 0 => { c.pending.drain(..n); }
                    _ => break,
                }
            }
            if c.client_closed && c.pending.is_empty() { s.close(); }
            while s.can_recv() && c.to_client.capacity() > 0 {
                let mut b = vec![0u8; 16384];
                match s.recv_slice(&mut b) {
                    Ok(n) if n > 0 => { b.truncate(n); let _ = c.to_client.try_send(b); }
                    _ => break,
                }
            }
            if !s.is_active() || (!s.may_recv() && !s.can_recv()) {
                let _ = c.to_client.try_send(Vec::new());
                if !s.is_active() {
                    sockets.remove(c.handle);
                    return false;
                }
            }
            true
        });
        // DNS through the tunnel
        queries.retain_mut(|q| {
            let s = sockets.get_mut::<udp::Socket>(q.handle);
            let due = q.sent.map_or(true, |t| t.elapsed() > Duration::from_secs(2));
            if due && s.can_send() {
                if q.tries >= 3 {
                    sockets.remove(q.handle);
                    return false;                      // the reply sender drops: an error
                }
                let _ = s.send_slice(&dns_query(q.id, &q.name), IpEndpoint::new(IpAddress::Ipv4(dns), 53));
                q.sent = Some(Instant::now());
                q.tries += 1;
            }
            let s = sockets.get_mut::<udp::Socket>(q.handle);
            if let Ok((n, _)) = s.recv_slice(&mut buf) {
                if let Some(ip) = dns_answer(&buf[..n], q.id) {
                    let (dummy, _) = oneshot::channel();
                    let reply = std::mem::replace(&mut q.reply, dummy);
                    let _ = reply.send(Ok(ip));
                    sockets.remove(q.handle);
                    return false;
                }
            }
            true
        });
        iface.poll(now(), &mut dev, &mut sockets);
        // our packets into the tunnel
        while let Some(p) = dev.tx.pop_front() {
            send_net(&udp, tunn.encapsulate(&p, &mut out));
        }
        std::thread::sleep(Duration::from_millis(1));
    }
}
