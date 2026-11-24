use std::collections::HashMap;
use std::io::{Read, Write};
use std::sync::{mpsc, Arc, Condvar, Mutex};
use std::time::{Duration, Instant};

fn check(name: &str, ok: bool) -> bool {
    println!("{} {}", if ok { "PASS" } else { "FAIL" }, name);
    ok
}

fn main() {
    let mut all = true;
    let args: Vec<String> = std::env::args().collect();
    all &= check("args", !args.is_empty());

    // threads + mutex
    let n = Arc::new(Mutex::new(0u64));
    let hs: Vec<_> = (0..4).map(|i| { let n = n.clone(); std::thread::spawn(move || {
        for _ in 0..1000 { *n.lock().unwrap() += 1; } i }) }).collect();
    let ids: u64 = hs.into_iter().map(|h| h.join().unwrap()).sum();
    all &= check("threads+mutex", *n.lock().unwrap() == 4000 && ids == 6);

    // channel
    let (tx, rx) = mpsc::channel();
    let t = std::thread::spawn(move || { for i in 0..100u32 { tx.send(i).unwrap(); } });
    let s: u32 = rx.iter().sum(); t.join().unwrap();
    all &= check("mpsc", s == 4950);

    // condvar with timeout, and sleep
    let pair = Arc::new((Mutex::new(false), Condvar::new()));
    let t0 = Instant::now();
    let (l, c) = &*pair;
    let (g0, r) = c.wait_timeout(l.lock().unwrap(), Duration::from_millis(200)).unwrap(); drop(g0);
    let el = t0.elapsed();
    all &= check(&format!("condvar timeout ({} ms)", el.as_millis()), r.timed_out() && el >= Duration::from_millis(150));
    let p2 = pair.clone();
    let t = std::thread::spawn(move || { std::thread::sleep(Duration::from_millis(50));
        let (l, c) = &*p2; *l.lock().unwrap() = true; c.notify_one(); });
    let mut g = l.lock().unwrap();
    while !*g { g = c.wait(g).unwrap(); }
    drop(g); t.join().unwrap();
    all &= check("condvar notify", true);

    // files
    let p = "SYS:T/rust-std-test.txt";
    let ok = (|| -> std::io::Result<bool> {
        std::fs::File::create(p)?.write_all(b"QRX OS One 64\n")?;
        let mut s = String::new();
        std::fs::File::open(p)?.read_to_string(&mut s)?;
        let md = std::fs::metadata(p)?;
        std::fs::remove_file(p)?;
        Ok(s == "QRX OS One 64\n" && md.len() == 14 && md.is_file() && std::fs::metadata(p).is_err())
    })();
    all &= check(&format!("fs {:?}", ok), matches!(ok, Ok(true)));
    let ok = std::fs::read_dir("C:").map(|d| d.filter_map(|e| e.ok()).count());
    all &= check(&format!("read_dir C: {:?}", ok), matches!(ok, Ok(n) if n > 3));
    let e = std::fs::File::open("SYS:T/does-not-exist").unwrap_err();
    all &= check(&format!("io error kind {:?} \"{}\"", e.kind(), e), e.kind() == std::io::ErrorKind::NotFound);

    // hashmap (random seed), floats (soft-float), env, time
    let mut m = HashMap::new();
    for i in 0..1000 { m.insert(format!("k{}", i), i); }
    all &= check("hashmap", m.len() == 1000 && m["k777"] == 777);
    let x: f64 = (2.0f64).sqrt() * 3.5 + (1.0f64).exp();
    all &= check(&format!("f64 {:.6}", x), format!("{:.6}", x) == "7.668029");
    unsafe { std::env::set_var("QRXTEST", "ja"); }
    all &= check("env", std::env::var("QRXTEST").as_deref() == Ok("ja"));
    let st = std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).map(|d| d.as_secs());
    all &= check(&format!("system time {:?}", st), matches!(st, Ok(s) if s > 1_000_000_000));
    let net = std::net::TcpStream::connect("127.0.0.1:1");
    println!("info net: {:?}", net.map(|_| ()));
    println!("{}", if all { "ALL PASS" } else { "SOME FAILED" });
}
