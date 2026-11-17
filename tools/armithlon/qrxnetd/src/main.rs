//! qrxnetd: the network of QRX OS One's guests, through Tor or a WireGuard tunnel.
//!
//! The ARMithlon engine gives the guest the Mac's sockets. When the user turns on Tor or the VPN,
//! the engine sends every guest connection here instead, as SOCKS5 over a Unix socket:
//!   CONNECT (IPv4 or name)      a TCP stream through Tor or through the tunnel
//!   RESOLVE (0xF0, Tor's)       a name to an IPv4 address, through Tor or the tunnel's DNS
//! There is no fallback to a direct connection: what cannot go through the tunnel fails. The engine
//! itself refuses every guest connection until this prints READY, and again once it has exited, so
//! the Mac's own address is never used for the guest.
//!
//!   qrxnetd CONFIG SOCKET
//! CONFIG has key = value lines:
//!   mode = tor | wireguard
//!   state_dir = DIR                      Tor's state and cache (mode tor)
//!   private_key = BASE64                 WireGuard: the interface's key
//!   address = 10.0.0.2                   WireGuard: the interface's IPv4 address in the tunnel
//!   dns = 10.0.0.1                       WireGuard: the DNS server inside the tunnel
//!   peer_public_key = BASE64             WireGuard: the server's key
//!   preshared_key = BASE64               WireGuard: optional
//!   endpoint = 203.0.113.5:51820         WireGuard: the server (an address, so no DNS outside)
//!   keepalive = 25                       WireGuard: optional, seconds

mod wg;

use anyhow::{Context, Result, anyhow, bail};
use std::collections::HashMap;
use std::net::{IpAddr, Ipv4Addr};
use std::sync::Arc;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::{UnixListener, UnixStream};

pub fn read_config(path: &str) -> Result<HashMap<String, String>> {
    let text = std::fs::read_to_string(path).with_context(|| format!("cannot read {path}"))?;
    let mut map = HashMap::new();
    for line in text.lines() {
        let line = line.trim();
        if line.is_empty() || line.starts_with('#') {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            map.insert(k.trim().to_ascii_lowercase(), v.trim().to_string());
        }
    }
    Ok(map)
}

/// Where a SOCKS request goes
#[derive(Clone)]
enum Backend {
    Tor(Arc<arti_client::TorClient<tor_rtcompat::PreferredRuntime>>),
    Wg(wg::Handle),
}

enum Target {
    Ip(Ipv4Addr),
    Name(String),
}

async fn read_request(s: &mut UnixStream) -> Result<(u8, Target, u16)> {
    let mut b = [0u8; 2];
    s.read_exact(&mut b).await?;
    if b[0] != 5 {
        bail!("not SOCKS5");
    }
    let mut methods = vec![0u8; b[1] as usize];
    s.read_exact(&mut methods).await?;
    s.write_all(&[5, 0]).await?;
    let mut h = [0u8; 4];
    s.read_exact(&mut h).await?;
    let target = match h[3] {
        1 => {
            let mut a = [0u8; 4];
            s.read_exact(&mut a).await?;
            Target::Ip(Ipv4Addr::from(a))
        }
        3 => {
            let mut n = [0u8; 1];
            s.read_exact(&mut n).await?;
            let mut name = vec![0u8; n[0] as usize];
            s.read_exact(&mut name).await?;
            Target::Name(String::from_utf8_lossy(&name).into_owned())
        }
        _ => bail!("address type {} not supported", h[3]),
    };
    let mut p = [0u8; 2];
    s.read_exact(&mut p).await?;
    Ok((h[1], target, u16::from_be_bytes(p)))
}

async fn reply(s: &mut UnixStream, code: u8, addr: Ipv4Addr) -> Result<()> {
    let mut r = vec![5, code, 0, 1];
    r.extend_from_slice(&addr.octets());
    r.extend_from_slice(&[0, 0]);
    s.write_all(&r).await?;
    Ok(())
}

async fn serve(mut s: UnixStream, backend: Backend) -> Result<()> {
    let (cmd, target, port) = read_request(&mut s).await?;
    match (cmd, &backend) {
        (0xF0, Backend::Tor(tor)) => {
            let name = match target {
                Target::Name(n) => n,
                Target::Ip(a) => return reply(&mut s, 0, a).await,
            };
            match tor.resolve(&name).await {
                Ok(list) => {
                    let v4 = list.iter().find_map(|a| if let IpAddr::V4(v) = a { Some(*v) } else { None });
                    match v4 {
                        Some(a) => reply(&mut s, 0, a).await,
                        None => reply(&mut s, 4, Ipv4Addr::UNSPECIFIED).await,
                    }
                }
                Err(_) => reply(&mut s, 4, Ipv4Addr::UNSPECIFIED).await,
            }
        }
        (0xF0, Backend::Wg(h)) => {
            let name = match target {
                Target::Name(n) => n,
                Target::Ip(a) => return reply(&mut s, 0, a).await,
            };
            match h.resolve(name).await {
                Ok(a) => reply(&mut s, 0, a).await,
                Err(_) => reply(&mut s, 4, Ipv4Addr::UNSPECIFIED).await,
            }
        }
        (1, Backend::Tor(tor)) => {
            let addr = match &target {
                Target::Ip(a) => a.to_string(),
                Target::Name(n) => n.clone(),
            };
            match tor.connect((addr.as_str(), port)).await {
                Ok(mut stream) => {
                    reply(&mut s, 0, Ipv4Addr::UNSPECIFIED).await?;
                    let _ = tokio::io::copy_bidirectional(&mut s, &mut stream).await;
                    Ok(())
                }
                Err(_) => reply(&mut s, 5, Ipv4Addr::UNSPECIFIED).await,
            }
        }
        (1, Backend::Wg(h)) => {
            let ip = match target {
                Target::Ip(a) => a,
                Target::Name(n) => match h.resolve(n).await {
                    Ok(a) => a,
                    Err(_) => return reply(&mut s, 4, Ipv4Addr::UNSPECIFIED).await,
                },
            };
            match h.connect(ip, port).await {
                Ok(conn) => {
                    reply(&mut s, 0, Ipv4Addr::UNSPECIFIED).await?;
                    wg::pump(s, conn).await;
                    Ok(())
                }
                Err(_) => reply(&mut s, 5, Ipv4Addr::UNSPECIFIED).await,
            }
        }
        _ => reply(&mut s, 7, Ipv4Addr::UNSPECIFIED).await, // command not supported (UDP, BIND)
    }
}

#[tokio::main]
async fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    if args.len() != 3 {
        bail!("usage: qrxnetd CONFIG SOCKET");
    }
    let cfg = read_config(&args[1])?;
    let mode = cfg.get("mode").map(String::as_str).unwrap_or("");
    let backend = match mode {
        "tor" => {
            let dir = cfg.get("state_dir").ok_or_else(|| anyhow!("state_dir missing"))?;
            let config = arti_client::config::TorClientConfigBuilder::from_directories(
                format!("{dir}/state"),
                format!("{dir}/cache"),
            )
            .build()?;
            eprintln!("qrxnetd: Tor is starting");
            let tor = arti_client::TorClient::create_bootstrapped(config).await?;
            Backend::Tor(tor)
        }
        "wireguard" => {
            let h = wg::start(&cfg)?;
            eprintln!("qrxnetd: WireGuard handshake");
            h.wait_ready(std::time::Duration::from_secs(20)).await?;
            Backend::Wg(h)
        }
        other => bail!("mode {other:?}: tor or wireguard"),
    };
    let _ = std::fs::remove_file(&args[2]);
    let listener = UnixListener::bind(&args[2])?;
    // the engine waits for this line before it lets the guest connect
    println!("READY");
    loop {
        let (s, _) = listener.accept().await?;
        let b = backend.clone();
        tokio::spawn(async move {
            let _ = serve(s, b).await;
        });
    }
}
