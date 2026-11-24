use std::sync::atomic::{AtomicU32, Ordering};
static DROPS: AtomicU32 = AtomicU32::new(0);
struct D(u32);
impl Drop for D { fn drop(&mut self) { DROPS.fetch_add(self.0, Ordering::SeqCst); } }
#[inline(never)]
fn deep(n: u32) -> u32 { let _d = D(1); if n == 0 { panic!("tief unten") } deep(n - 1) + 1 }
fn main() {
    std::panic::set_hook(Box::new(|_| {}));
    let mut ok = true;
    let r = std::panic::catch_unwind(|| deep(5));
    let d = DROPS.load(Ordering::SeqCst);
    let msg = r.as_ref().err().and_then(|e| e.downcast_ref::<&str>()).map(|s| s.to_string());
    ok &= r.is_err() && d == 6 && msg.as_deref() == Some("tief unten");
    println!("catch_unwind deep: err={} drops={} payload={:?}", r.is_err(), d, msg);
    let a = std::panic::catch_unwind(|| { let v: Vec<u8> = Vec::new(); v[3] });
    ok &= a.is_err(); println!("index panic caught: {}", a.is_err());
    let b = std::thread::spawn(|| std::panic::catch_unwind(|| panic!("im thread")).is_err()).join();
    ok &= matches!(b, Ok(true)); println!("catch inside thread: {:?}", b);
    let t = std::thread::spawn(|| { let _d = D(10); let x: Option<u32> = None; x.unwrap() });
    let j = t.join();
    let d = DROPS.load(Ordering::SeqCst);
    ok &= j.is_err() && d == 16; println!("thread panic joined as Err: {} drops={}", j.is_err(), d);
    for i in 0..20 { ok &= std::panic::catch_unwind(move || { if i % 2 == 0 { panic!("{}", i) } i }).is_err() == (i % 2 == 0); }
    println!("20 alternating panics ok: {}", ok);
    println!("{}", if ok { "UNWIND ALL PASS" } else { "UNWIND FAILED" });
}
