#![no_std]
#![no_main]
extern crate alloc;
use alloc::{format, string::String, vec::Vec, collections::BTreeMap};
use core::alloc::{GlobalAlloc, Layout};

unsafe extern "C" {
    fn printf(fmt: *const u8, ...) -> i32;
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn aligned_alloc(a: usize, n: usize) -> *mut u8;
    fn abort() -> !;
}
struct CAlloc;
unsafe impl GlobalAlloc for CAlloc {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        unsafe { if l.align() <= 8 { malloc(l.size()) } else { aligned_alloc(l.align(), l.size()) } }
    }
    unsafe fn dealloc(&self, p: *mut u8, _: Layout) { unsafe { free(p) } }
}
#[global_allocator]
static A: CAlloc = CAlloc;
#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { unsafe { printf(b"RUST PANIC\n\0".as_ptr()); abort() } }

fn say(s: &str) { let mut v = Vec::from(s.as_bytes()); v.push(0); unsafe { printf(b"%s\n\0".as_ptr(), v.as_ptr()); } }

#[unsafe(no_mangle)]
pub extern "C" fn main(_argc: i32, _argv: *const *const u8) -> i32 {
    say("Hallo aus Rust auf QRX OS One 64");
    let v: Vec<u64> = (1..=20).map(|x| x * x).collect();
    let sum: u64 = v.iter().sum();
    let mut m = BTreeMap::new();
    for w in ["gecko", "rust", "aros", "rust", "m68k64", "rust"] { *m.entry(w).or_insert(0u32) += 1; }
    let s: String = format!("sum {} max {:?} map {:?} f {:.3} big {}", sum, v.iter().max(), m, core::hint::black_box(1.4142135623730951f64) * 3.5, u128::MAX / 7);
    say(&s);
    let ok = sum == 2870 && m["rust"] == 3 && s.contains("4.950");
    say(if ok { "PASS: Rust core+alloc on m68k64" } else { "FAIL" });
    if ok { 0 } else { 20 }
}
