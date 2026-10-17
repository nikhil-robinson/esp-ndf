//! Same benchmark as bench/bench.h, written the way esp-hal intends:
//! PeriodicTimer + #[handler] ISR, CpuControl for core1, UsbSerialJtag driver,
//! Output GPIO. Hot code is #[ram] (= IRAM_ATTR in the C builds).
#![no_std]
#![no_main]

use core::cell::RefCell;
use core::sync::atomic::{AtomicU32, Ordering::*};
use critical_section::Mutex;
use esp_backtrace as _;
use esp_hal::{
    clock::CpuClock,
    gpio::{Level, Output, OutputConfig},
    handler, ram,
    system::{CpuControl, Stack},
    time::Duration,
    timer::{timg::TimerGroup, PeriodicTimer},
    usb::usb_serial_jtag::UsbSerialJtag,
    Blocking,
};
use esp_println::println;

esp_bootloader_esp_idf::esp_app_desc!();

const CPU_HZ: u32 = 240_000_000;
const LOOP_HZ: u32 = 20_000;
const PERIOD: u32 = CPU_HZ / LOOP_HZ;
const ITERS: u32 = LOOP_HZ * 5;
const ISR_PERIOD: u32 = CPU_HZ / 10_000;
const TLM_EVERY: u32 = 20;

#[derive(Default, Clone, Copy)]
struct Stats { iters: u32, misses: u32, lat_max: u32, exec_min: u32, exec_max: u32,
               work_per_s: u32, lat_sum: u64, exec_sum: u64, usb_bytes: u32 }

#[derive(Clone, Copy)]
struct Ctl { kp: f32, ki: f32, kd: f32, i: f32, prev: f32 }

static READY: [AtomicU32; 2] = [AtomicU32::new(0), AtomicU32::new(0)];
static DONE1: AtomicU32 = AtomicU32::new(0);
static ISR_ON: AtomicU32 = AtomicU32::new(0);
static mut RES: [Stats; 2] = [Stats { iters: 0, misses: 0, lat_max: 0, exec_min: 0, exec_max: 0,
                                      work_per_s: 0, lat_sum: 0, exec_sum: 0, usb_bytes: 0 }; 2];
static mut BENCH_IN: f32 = 1.0;   // volatile in/out, exactly like bench.h
static mut BENCH_SINK: f32 = 0.0;
static mut ISR_N: u32 = 0;
static mut ISR_LAST: u32 = 0;
static mut ISR_JMAX: u32 = 0;
static mut ISR_JSUM: u64 = 0;

static TIMER: Mutex<RefCell<Option<PeriodicTimer<'static, Blocking>>>> = Mutex::new(RefCell::new(None));
static ISR_PIN: Mutex<RefCell<Option<Output<'static>>>> = Mutex::new(RefCell::new(None));
static mut APP_STACK: Stack<16384> = Stack::new();

#[inline(always)]
fn ccount() -> u32 { esp_hal::xtensa_lx::timer::get_cycle_count() }

#[inline(always)]
fn work(c: &mut [Ctl; 8], x: f32) -> f32 {
    let mut out = 0.0;
    for a in c.iter_mut() {
        let e = x - a.prev * 0.5;
        a.i += e * 1e-4;
        let d = e - a.prev;
        a.prev = e;
        out += a.kp * e + a.ki * a.i + a.kd * d;
    }
    out
}

#[handler]
#[ram]
fn timer_isr() {
    let now = ccount();
    critical_section::with(|cs| {
        TIMER.borrow_ref_mut(cs).as_mut().unwrap().clear_interrupt();
        ISR_PIN.borrow_ref_mut(cs).as_mut().unwrap().toggle();
    });
    unsafe {
        if ISR_ON.load(Relaxed) != 0 && ISR_LAST != 0 {
            let j = (now.wrapping_sub(ISR_LAST).wrapping_sub(ISR_PERIOD) as i32).unsigned_abs();
            ISR_JSUM += j as u64;
            if j > ISR_JMAX { ISR_JMAX = j; }
            ISR_N += 1;
        }
        ISR_LAST = now;
    }
}

fn hex8(buf: &mut [u8], v: u32) {
    for k in 0..8 { buf[k] = b"0123456789abcdef"[((v >> (28 - 4 * k)) & 15) as usize]; }
}

#[ram]
fn bench_run(core: usize, pin: &mut Output, mut usb: Option<&mut UsbSerialJtag<'static, Blocking>>) {
    let s = unsafe { &mut *core::ptr::addr_of_mut!(RES[core]) };
    let mut c = [Ctl { kp: 1.2, ki: 0.3, kd: 0.05, i: 0.0, prev: 0.0 }; 8];
    let input = || unsafe { core::ptr::read_volatile(core::ptr::addr_of!(BENCH_IN)) };
    let sink = |v: f32| unsafe { core::ptr::write_volatile(core::ptr::addr_of_mut!(BENCH_SINK), v) };

    READY[core].store(1, Release);
    while READY[0].load(Acquire) == 0 || READY[1].load(Acquire) == 0 {}

    s.exec_min = u32::MAX;
    if core == 0 { ISR_ON.store(1, Relaxed); }
    let mut tlm = [0u8; 29];
    let mut next = ccount().wrapping_add(PERIOD);
    for i in 0..ITERS {
        while (ccount().wrapping_sub(next) as i32) < 0 {}
        let t0 = ccount();
        pin.set_high();
        sink(work(&mut c, input()));
        pin.set_low();
        if let Some(u) = usb.as_deref_mut() {
            if i % TLM_EVERY == 0 {
                tlm[0] = b'T'; tlm[1] = b' ';
                hex8(&mut tlm[2..10], i); tlm[10] = b' ';
                hex8(&mut tlm[11..19], s.exec_max); tlm[19] = b' ';
                hex8(&mut tlm[20..28], s.lat_max); tlm[28] = b'\n';
                for &b in tlm.iter() { if u.write_byte_nb(b).is_ok() { s.usb_bytes += 1; } }
                let _ = u.flush_tx_nb();
            }
        }
        let ex = ccount().wrapping_sub(t0);
        let lat = t0.wrapping_sub(next);
        s.lat_sum += lat as u64; if lat > s.lat_max { s.lat_max = lat; }
        s.exec_sum += ex as u64; if ex > s.exec_max { s.exec_max = ex; }
        if ex < s.exec_min { s.exec_min = ex; }
        next = next.wrapping_add(PERIOD);
        if (ccount().wrapping_sub(next) as i32) >= 0 { s.misses += 1; }
    }
    s.iters = ITERS;
    if core == 0 { ISR_ON.store(0, Relaxed); }

    let (mut n, t0) = (0u32, ccount());
    while ccount().wrapping_sub(t0) < CPU_HZ { sink(work(&mut c, input())); n += 1; }
    s.work_per_s = n;
}

fn ns(c: u64) -> u64 { c * 25 / 6 }

#[esp_hal::main]
fn main() -> ! {
    let p = esp_hal::init(esp_hal::Config::default().with_cpu_clock(CpuClock::max()));

    let mut pin0 = Output::new(p.GPIO4, Level::Low, OutputConfig::default());
    let pin1 = Output::new(p.GPIO5, Level::Low, OutputConfig::default());
    let isr_pin = Output::new(p.GPIO6, Level::Low, OutputConfig::default());
    let mut usb = UsbSerialJtag::new(p.USB_DEVICE);

    let tg0 = TimerGroup::new(p.TIMG0);
    let mut t = PeriodicTimer::new(tg0.timer0);
    t.set_interrupt_handler(timer_isr);   // installs on the calling core (core0)
    t.start(Duration::from_micros(100)).unwrap();
    t.listen();
    critical_section::with(|cs| {
        TIMER.borrow_ref_mut(cs).replace(t);
        ISR_PIN.borrow_ref_mut(cs).replace(isr_pin);
    });

    let mut cpu = CpuControl::new(p.CPU_CTRL);
    let _guard = cpu.start_app_core(unsafe { &mut *core::ptr::addr_of_mut!(APP_STACK) }, move || {
        let mut pin1 = pin1;
        bench_run(1, &mut pin1, None);
        DONE1.store(1, Release);
        loop {}
    }).unwrap();

    bench_run(0, &mut pin0, Some(&mut usb));
    while DONE1.load(Acquire) == 0 {}

    let r = unsafe { &*core::ptr::addr_of!(RES) };
    println!("\n=== ESP-HAL 1.2.2  ({} Hz loop, {} iters/core) ===", LOOP_HZ, ITERS);
    for (k, s) in r.iter().enumerate() {
        println!("core{}: wake lat avg {} ns max {} ns | exec min {} avg {} max {} ns | misses {} | work/s {}",
            k, ns(s.lat_sum / s.iters as u64), ns(s.lat_max as u64), ns(s.exec_min as u64),
            ns(s.exec_sum / s.iters as u64), ns(s.exec_max as u64), s.misses, s.work_per_s);
    }
    let (n, jmax, jsum) = unsafe { (ISR_N, ISR_JMAX, ISR_JSUM) };
    println!("isr  : {} calls, period jitter avg {} ns max {} ns | usb telemetry {} bytes",
        n, ns(if n > 0 { jsum / n as u64 } else { 0 }), ns(jmax as u64), r[0].usb_bytes);
    loop {}
}
