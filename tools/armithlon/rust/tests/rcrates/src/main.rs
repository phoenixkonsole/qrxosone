// QRX OS One 64: the crates Gecko vendors that needed AROS support (libc, rustix, socket2, mtu,
// libloading, errno), run in the AROS 64 guest in Unix path mode (nixmain.o). See ../../README.md.
// Exercises the Gecko-vendored crates that needed AROS support in libc/rustix/socket2/mtu/libloading.
use std::io::{Read, Write};
use std::net::SocketAddr;

fn check(name: &str, ok: bool, detail: String, fails: &mut u32) {
    println!("{} {}: {}", if ok { "PASS" } else { "FAIL" }, name, detail);
    if !ok { *fails += 1; }
}

fn main() {
    let mut f = 0u32;
    // rustix: open/write/fstat/read/close, getpid, clock
    {
        use rustix::fs::{self, Mode, OFlags};
        let r = (|| -> rustix::io::Result<(u64, Vec<u8>)> {
            let fd = fs::open("/T/rx.txt", OFlags::CREATE | OFlags::TRUNC | OFlags::RDWR, Mode::from_raw_mode(0o644))?;
            rustix::io::write(&fd, b"rustix on AROS")?;
            let st = fs::fstat(&fd)?;
            fs::seek(&fd, rustix::fs::SeekFrom::Start(0))?;
            let mut b = [0u8; 32];
            let n = rustix::io::read(&fd, &mut b)?;
            Ok((st.st_size as u64, b[..n].to_vec()))
        })();
        match r {
            Ok((sz, data)) => check("rustix fs", sz == 14 && data == b"rustix on AROS", format!("size {} data {:?}", sz, String::from_utf8_lossy(&data)), &mut f),
            Err(e) => check("rustix fs", false, format!("{e}"), &mut f),
        }
        let pid = rustix::process::getpid();
        check("rustix getpid", pid.as_raw_nonzero().get() > 0, format!("{}", pid.as_raw_nonzero()), &mut f);
        let t = rustix::time::clock_gettime(rustix::time::ClockId::Monotonic);
        check("rustix clock", t.tv_sec >= 0, format!("{}.{:09}", t.tv_sec, t.tv_nsec), &mut f);
    }
    // memmap2: anonymous map
    match memmap2::MmapMut::map_anon(65536) {
        Ok(mut m) => {
            m[0] = 0x5a; m[65535] = 0xa5;
            check("memmap2 anon", m[0] == 0x5a && m[65535] == 0xa5 && m[100] == 0, format!("len {}", m.len()), &mut f);
        }
        Err(e) => check("memmap2 anon", false, format!("{e}"), &mut f),
    }
    // memmap2: read-only file map
    {
        let r = (|| -> std::io::Result<Vec<u8>> {
            let file = std::fs::File::open("/T/rx.txt")?;
            let m = unsafe { memmap2::Mmap::map(&file)? };
            Ok(m[..].to_vec())
        })();
        match r {
            Ok(d) => check("memmap2 file", d == b"rustix on AROS", format!("{:?}", String::from_utf8_lossy(&d)), &mut f),
            Err(e) => check("memmap2 file", false, format!("{e}"), &mut f),
        }
    }
    // memmap2: shared writable file map, written back by flush (msync) and visible to read()
    {
        let r = (|| -> std::io::Result<Vec<u8>> {
            let file = std::fs::OpenOptions::new().read(true).write(true).open("/T/rx.txt")?;
            let mut m = unsafe { memmap2::MmapMut::map_mut(&file)? };
            m[0..6].copy_from_slice(b"RUSTIX");
            m.flush()?;
            drop(m);
            // AROS opens a file for writing with an exclusive lock: close it before reading it again
            drop(file);
            std::fs::read("/T/rx.txt")
        })();
        match r {
            Ok(d) => check("memmap2 shared write", d == b"RUSTIX on AROS", format!("{:?}", String::from_utf8_lossy(&d)), &mut f),
            Err(e) => check("memmap2 shared write", false, format!("{e}"), &mut f),
        }
    }
    // socket2: loopback TCP
    {
        use socket2::{Domain, Socket, Type};
        let r = (|| -> std::io::Result<Vec<u8>> {
            let l = Socket::new(Domain::IPV4, Type::STREAM, None)?;
            l.set_reuse_address(true)?;
            l.bind(&"127.0.0.1:0".parse::<SocketAddr>().unwrap().into())?;
            l.listen(4)?;
            let addr = l.local_addr()?.as_socket().unwrap();
            let c = Socket::new(Domain::IPV4, Type::STREAM, None)?;
            c.set_tcp_nodelay(true)?;
            c.connect(&addr.into())?;
            let (s, _) = l.accept()?;
            let mut cs: std::net::TcpStream = c.into();
            cs.write_all(b"socket2 hello")?;
            let mut ss: std::net::TcpStream = s.into();
            let mut b = [0u8; 13];
            ss.read_exact(&mut b)?;
            Ok(b.to_vec())
        })();
        match r {
            Ok(d) => check("socket2 tcp", d == b"socket2 hello", format!("{:?}", String::from_utf8_lossy(&d)), &mut f),
            Err(e) => check("socket2 tcp", false, format!("{e}"), &mut f),
        }
    }
    // rand
    {
        use rand::Rng;
        let mut rng = rand::thread_rng();
        let a: u64 = rng.gen();
        let b: u64 = rng.gen();
        check("rand", a != b, format!("{a:016x} {b:016x}"), &mut f);
    }
    // tempfile
    match tempfile::tempfile() {
        Ok(mut t) => {
            let ok = t.write_all(b"tmp").is_ok();
            check("tempfile", ok, format!("in {:?}", std::env::temp_dir()).into(), &mut f);
        }
        Err(e) => check("tempfile", false, format!("{e}"), &mut f),
    }
    // libloading: no dynamic linker on AROS, must report an error
    match unsafe { libloading::Library::new("foo.so") } {
        Ok(_) => check("libloading", false, "dlopen claimed success".into(), &mut f),
        Err(e) => check("libloading", true, format!("error as expected: {e}"), &mut f),
    }
    // mtu: not supported on AROS, must be an error
    match mtu::interface_and_mtu("127.0.0.1".parse().unwrap()) {
        Ok(v) => check("mtu", false, format!("unexpected {:?}", v), &mut f),
        Err(e) => check("mtu", true, format!("error as expected: {e}"), &mut f),
    }
    // encoding_rs: Gecko's UTF-8 -> UTF-16 conversion (nsLocalFile::GetPath and every string)
    {
        let src = "/QRXSYS/Firefox";
        let mut dst = [0u16; 32];
        let n = encoding_rs::mem::convert_utf8_to_utf16(src.as_bytes(), &mut dst);
        let back = String::from_utf16_lossy(&dst[..n]);
        let valid = encoding_rs::mem::utf8_latin1_up_to(src.as_bytes());
        check("encoding_rs utf8->utf16", n == src.len() && back == src, format!("{} units {:?}, latin1_up_to {}", n, back, valid), &mut f);
        let umlaut = "Grüße, 世界";
        let mut d2 = [0u16; 32];
        let n2 = encoding_rs::mem::convert_utf8_to_utf16(umlaut.as_bytes(), &mut d2);
        check("encoding_rs non-ASCII", String::from_utf16_lossy(&d2[..n2]) == umlaut, format!("{} units", n2), &mut f);
    }
    println!("{}", if f == 0 { "CRATES ALL PASS".to_string() } else { format!("CRATES {f} FAILED") });
}
