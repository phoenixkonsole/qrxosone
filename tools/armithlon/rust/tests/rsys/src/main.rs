//! Network, processes, pipes and random numbers of Rust std on QRX OS One 64.
use std::io::{ErrorKind, Read, Write};
use std::net::{TcpListener, TcpStream, ToSocketAddrs, UdpSocket};
use std::process::{Command, Stdio};
use std::time::{Duration, Instant};

fn check(name: &str, ok: bool) -> bool {
    println!("{} {}", if ok { "PASS" } else { "FAIL" }, name);
    ok
}

fn child() -> ! {
    // reads stdin to its end, answers in capitals on stdout, a line on stderr, ends with 3
    let mut s = String::new();
    std::io::stdin().read_to_string(&mut s).unwrap();
    print!("{}", s.to_uppercase());
    eprint!("stderr vom Kind");
    std::process::exit(3);
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.get(1).map(|s| s.as_str()) == Some("child") {
        child();
    }
    let mut all = true;

    // random bytes: the host's getentropy
    use std::hash::BuildHasher;
    let x1 = std::collections::hash_map::RandomState::new().hash_one(42u32);
    let x2 = std::collections::hash_map::RandomState::new().hash_one(42u32);
    all &= check(&format!("random seeds differ ({:016x} {:016x})", x1, x2), x1 != x2);

    // TCP: a listener and a client in the guest, both through the host's sockets
    let l = TcpListener::bind("127.0.0.1:0").unwrap();
    let addr = l.local_addr().unwrap();
    all &= check(&format!("tcp listen on {}", addr), addr.port() != 0);
    let srv = std::thread::spawn(move || {
        let (mut c, peer) = l.accept().unwrap();
        let mut buf = [0u8; 64];
        let n = c.read(&mut buf).unwrap();
        c.write_all(&buf[..n]).unwrap();
        c.write_all(b" zurueck").unwrap();
        drop(c);
        (peer, l)
    });
    let mut c = TcpStream::connect(addr).unwrap();
    c.set_nodelay(true).unwrap();
    c.write_all(b"hallo").unwrap();
    let mut got = String::new();
    c.read_to_string(&mut got).unwrap();
    let (peer, l) = srv.join().unwrap();
    all &= check(&format!("tcp echo {:?} (peer {})", got, peer), got == "hallo zurueck");

    // read timeout on an idle connection
    let srv = std::thread::spawn(move || { let (c, _) = l.accept().unwrap(); std::thread::sleep(Duration::from_millis(600)); drop(c); l });
    let c = TcpStream::connect_timeout(&addr, Duration::from_secs(5)).unwrap();
    c.set_read_timeout(Some(Duration::from_millis(200))).unwrap();
    let t0 = Instant::now();
    let e = (&c).read(&mut [0u8; 8]).unwrap_err();
    let el = t0.elapsed();
    all &= check(&format!("read timeout {:?} after {} ms", e.kind(), el.as_millis()),
                 matches!(e.kind(), ErrorKind::WouldBlock | ErrorKind::TimedOut) && el >= Duration::from_millis(150));
    let l = srv.join().unwrap();

    // nonblocking accept
    l.set_nonblocking(true).unwrap();
    let e = l.accept().unwrap_err();
    all &= check(&format!("nonblocking accept {:?}", e.kind()), e.kind() == ErrorKind::WouldBlock);
    drop(l);

    // a port nobody listens on
    let e = TcpStream::connect(addr).unwrap_err();
    all &= check(&format!("refused {:?}", e.kind()), e.kind() == ErrorKind::ConnectionRefused);

    // UDP
    let u1 = UdpSocket::bind("127.0.0.1:0").unwrap();
    let u2 = UdpSocket::bind("127.0.0.1:0").unwrap();
    u1.send_to(b"datagramm", u2.local_addr().unwrap()).unwrap();
    let mut buf = [0u8; 32];
    let (n, from) = u2.recv_from(&mut buf).unwrap();
    all &= check(&format!("udp {:?} from {}", std::str::from_utf8(&buf[..n]).unwrap(), from),
                 &buf[..n] == b"datagramm" && from == u1.local_addr().unwrap());

    // names
    let v: Vec<_> = "localhost:80".to_socket_addrs().map(|i| i.collect()).unwrap_or_default();
    all &= check(&format!("resolve localhost {:?}", v), v.iter().any(|a| a.ip().is_loopback() && a.port() == 80));

    // processes: an AROS command with its output
    let o = Command::new("C:Echo").arg("hallo von Echo").output();
    match &o {
        Ok(o) => all &= check(&format!("Command C:Echo: {} {:?}", o.status, String::from_utf8_lossy(&o.stdout)),
                              o.status.success() && String::from_utf8_lossy(&o.stdout).trim_end() == "hallo von Echo"),
        Err(e) => all &= check(&format!("Command C:Echo: {}", e), false),
    }
    // this program as child: stdin, stdout and stderr piped, return code 3
    let mut ch = Command::new("C:T").arg("child").stdin(Stdio::piped()).stdout(Stdio::piped())
        .stderr(Stdio::piped()).spawn().unwrap();
    ch.stdin.take().unwrap().write_all(b"gross geschrieben\n").unwrap();
    let mut out = String::new();
    ch.stdout.take().unwrap().read_to_string(&mut out).unwrap();
    let mut err = String::new();
    ch.stderr.take().unwrap().read_to_string(&mut err).unwrap();
    let st = ch.wait().unwrap();
    all &= check(&format!("child: {:?} {:?} {}", out, err, st),
                 out == "GROSS GESCHRIEBEN\n" && err == "stderr vom Kind" && st.code() == Some(3));
    // poll on a pipe: not ready while the child waits for its input, ready once it has answered
    {
        use std::os::fd::AsRawFd;
        #[repr(C)]
        struct PollFd { fd: i32, events: i16, revents: i16 }
        unsafe extern "C" { fn poll(fds: *mut PollFd, n: u32, timeout: i32) -> i32; }
        let mut ch = Command::new("C:T").arg("child").stdin(Stdio::piped()).stdout(Stdio::piped())
            .stderr(Stdio::null()).spawn().unwrap();
        let out = ch.stdout.take().unwrap();
        let mut p = PollFd { fd: out.as_raw_fd(), events: 1, revents: 0 };
        let before = unsafe { poll(&mut p, 1, 300) };
        ch.stdin.take().unwrap().write_all(b"x").unwrap();
        let t0 = Instant::now();
        let after = unsafe { poll(&mut p, 1, 10000) };
        let ms = t0.elapsed().as_millis();
        let _ = ch.wait();
        all &= check(&format!("poll on a pipe: {} before, {} after {} ms", before, after, ms), before == 0 && after == 1);
    }
    let e = Command::new("C:gibtsnicht").output().unwrap_err();
    all &= check(&format!("missing program: {:?}", e.kind()), e.kind() == ErrorKind::NotFound);
    // try_wait while it runs
    let mut w = Command::new("C:Wait").arg("1").spawn().unwrap();
    let early = w.try_wait().unwrap();
    let st = w.wait().unwrap();
    all &= check(&format!("try_wait {:?} then {}", early, st), early.is_none() && st.success());

    println!("{}", if all { "SYS ALL PASS" } else { "SYS SOME FAILED" });
}
