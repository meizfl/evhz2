// Cargo.toml:
// [package]
// name = "evhz"
// version = "0.1.0"
// edition = "2021"
//
// [dependencies]
// ctrlc = "3.4"
//
// [target.'cfg(target_os = "linux")'.dependencies]
// evdev = "0.12"
// nix = "0.27"
//
// [target.'cfg(target_os = "windows")'.dependencies]
// windows = { version = "0.52", features = [
//     "Win32_UI_Input_KeyboardAndMouse",
//     "Win32_Foundation",
//     "Win32_UI_WindowsAndMessaging"
// ] }
//
// [target.'cfg(target_os = "macos")'.dependencies]
// core-foundation = "0.9"
// core-graphics = "0.23"

use std::collections::VecDeque;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::Arc;
use std::time::Instant;

const HZ_LIST: usize = 64;

struct DeviceStats {
    name: String,
    hz_history: VecDeque<u32>,
    avg_hz: u32,
    prev_time: Option<Instant>,
}

impl DeviceStats {
    fn new(name: String) -> Self {
        Self {
            name,
            hz_history: VecDeque::with_capacity(HZ_LIST),
            avg_hz: 0,
            prev_time: None,
        }
    }

    fn update(&mut self, verbose: bool) {
        let time = Instant::now();

        if let Some(prev) = self.prev_time {
            let diff = time.duration_since(prev);
            let micros = diff.as_micros() as u64;

            if micros > 0 {
                let hz = (1_000_000u64 / micros) as u32;

                if hz > 0 && hz < 20000 {
                    if self.hz_history.len() >= HZ_LIST {
                        self.hz_history.pop_front();
                    }
                    self.hz_history.push_back(hz);

                    let sum: u32 = self.hz_history.iter().sum();
                    self.avg_hz = sum / self.hz_history.len() as u32;

                    if verbose {
                        println!(
                            "{}: Latest {:5}Hz, Average {:5}Hz",
                            self.name, hz, self.avg_hz
                        );
                    }
                }
            }
        }

        self.prev_time = Some(time);
    }

    fn print_average(&self) {
        if self.avg_hz > 0 {
            println!("Average for {}: {:5}Hz", self.name, self.avg_hz);
        }
    }
}

#[cfg(target_os = "linux")]
mod platform {
    use super::*;
    use evdev::{Device, InputEventKind};
    use std::fs;

    pub fn run(verbose: bool, running: Arc<AtomicBool>) {
        let mut devices = Vec::new();
        let mut stats_map = std::collections::HashMap::new();

        // Scan /dev/input/event* devices
        for entry in fs::read_dir("/dev/input").expect("Failed to read /dev/input") {
            let entry = entry.unwrap();
            let path = entry.path();

            if let Some(name) = path.file_name() {
                let name_str = name.to_string_lossy();
                if name_str.starts_with("event") {
                    if let Ok(device) = Device::open(&path) {
                        let dev_name = device.name().unwrap_or("Unknown").to_string();

                        if verbose {
                            println!("{}: {}", name_str, dev_name);
                        }

                        stats_map.insert(path.clone(), DeviceStats::new(dev_name));
                        devices.push((path.clone(), device));
                    }
                }
            }
        }

        if verbose {
            println!();
        }

        // Use select to wait for events with timeout
        use std::os::unix::io::AsRawFd;

        while running.load(Ordering::SeqCst) {
            let mut fds: Vec<libc::pollfd> = devices.iter().map(|(_, device)| {
                libc::pollfd {
                    fd: device.as_raw_fd(),
                                                                events: libc::POLLIN,
                                                                revents: 0,
                }
            }).collect();

            // Poll with 100ms timeout so we can check running flag
            let ret = unsafe { libc::poll(fds.as_mut_ptr(), fds.len() as libc::nfds_t, 100) };

            if ret > 0 {
                for (idx, (path, device)) in devices.iter_mut().enumerate() {
                    if fds[idx].revents & libc::POLLIN != 0 {
                        if let Ok(events) = device.fetch_events() {
                            for event in events {
                                match event.kind() {
                                    InputEventKind::RelAxis(_) | InputEventKind::AbsAxis(_) => {
                                        if let Some(stats) = stats_map.get_mut(path) {
                                            stats.update(verbose);
                                        }
                                    }
                                    _ => {}
                                }
                            }
                        }
                    }
                }
            }
        }

        println!();
        for stats in stats_map.values() {
            stats.print_average();
        }
    }
}

#[cfg(target_os = "windows")]
mod platform {
    use super::*;
    use windows::Win32::Foundation::POINT;
    use windows::Win32::UI::Input::KeyboardAndMouse::GetAsyncKeyState;
    use windows::Win32::UI::WindowsAndMessaging::GetCursorPos;

    pub fn run(verbose: bool, running: Arc<AtomicBool>) {
        if verbose {
            println!("device0: Mouse");
            println!("device1: Keyboard");
            println!();
        }

        let mut mouse_stats = DeviceStats::new("Mouse".to_string());
        let mut keyboard_stats = DeviceStats::new("Keyboard".to_string());

        let mut last_pos = POINT { x: 0, y: 0 };
        let mut last_key_state = [false; 256];

        unsafe {
            let _ = GetCursorPos(&mut last_pos);
        }

        while running.load(Ordering::SeqCst) {
            // Check mouse movement
            let mut current_pos = POINT { x: 0, y: 0 };
            unsafe {
                if GetCursorPos(&mut current_pos).is_ok() {
                    if current_pos.x != last_pos.x || current_pos.y != last_pos.y {
                        mouse_stats.update(verbose);
                        last_pos = current_pos;
                    }
                }

                // Check keyboard
                for vk in 0..256 {
                    let state = GetAsyncKeyState(vk) as u16 & 0x8000 != 0;
                    if state && !last_key_state[vk as usize] {
                        keyboard_stats.update(verbose);
                    }
                    last_key_state[vk as usize] = state;
                }
            }

            std::thread::sleep(std::time::Duration::from_micros(100));
        }

        println!();
        mouse_stats.print_average();
        keyboard_stats.print_average();
    }
}

#[cfg(target_os = "macos")]
mod platform {
    use super::*;
    use core_graphics::event::{CGEvent, CGEventTap, CGEventTapLocation, CGEventTapOptions, CGEventTapPlacement, CGEventType};
    use core_foundation::runloop::{kCFRunLoopCommonModes, CFRunLoop};
    use std::sync::Mutex;

    lazy_static::lazy_static! {
        static ref MOUSE_STATS: Mutex<DeviceStats> = Mutex::new(DeviceStats::new("Mouse".to_string()));
        static ref KEYBOARD_STATS: Mutex<DeviceStats> = Mutex::new(DeviceStats::new("Keyboard".to_string()));
        static ref VERBOSE: Mutex<bool> = Mutex::new(false);
    }

    extern "C" fn event_callback(
        _proxy: CGEventTapProxy,
        event_type: CGEventType,
        _event: CGEvent,
        _user_info: *mut std::ffi::c_void,
    ) -> Option<CGEvent> {
        let verbose = *VERBOSE.lock().unwrap();

        match event_type {
            CGEventType::MouseMoved | CGEventType::LeftMouseDragged | CGEventType::RightMouseDragged => {
                MOUSE_STATS.lock().unwrap().update(verbose);
            }
            CGEventType::KeyDown | CGEventType::KeyUp => {
                KEYBOARD_STATS.lock().unwrap().update(verbose);
            }
            _ => {}
        }

        None
    }

    pub fn run(verbose: bool, running: Arc<AtomicBool>) {
        *VERBOSE.lock().unwrap() = verbose;

        if verbose {
            println!("device0: Mouse");
            println!("device1: Keyboard");
            println!();
        }

        let event_tap = CGEventTap::new(
            CGEventTapLocation::HID,
            CGEventTapPlacement::HeadInsertEventTap,
            CGEventTapOptions::ListenOnly,
            vec![
                CGEventType::MouseMoved,
                CGEventType::LeftMouseDragged,
                CGEventType::RightMouseDragged,
                CGEventType::KeyDown,
                CGEventType::KeyUp,
            ],
            event_callback,
        )
        .expect("Failed to create event tap. Run with sudo.");

        let loop_source = event_tap
        .mach_port
        .create_runloop_source(0)
        .expect("Failed to create runloop source");

        let run_loop = CFRunLoop::get_current();
        run_loop.add_source(&loop_source, unsafe { kCFRunLoopCommonModes });
        event_tap.enable();

        while running.load(Ordering::SeqCst) {
            std::thread::sleep(std::time::Duration::from_millis(100));
        }

        println!();
        MOUSE_STATS.lock().unwrap().print_average();
        KEYBOARD_STATS.lock().unwrap().print_average();
    }
}

#[cfg(target_os = "freebsd")]
mod platform {
    use super::*;
    // FreeBSD uses same evdev approach as Linux
    pub use super::platform::run;
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let mut verbose = true;

    for arg in &args[1..] {
        match arg.as_str() {
            "-h" | "--help" => {
                println!("Usage: {} [-n|-h]", args[0]);
                println!("-n, --nonverbose    nonverbose mode");
                println!("-h, --help          show this help");
                return;
            }
            "-n" | "--nonverbose" => {
                verbose = false;
            }
            _ => {
                eprintln!("Unknown option: {}", arg);
                return;
            }
        }
    }

    #[cfg(target_os = "linux")]
    {
        // Check if we can access /dev/input
        if std::fs::metadata("/dev/input/event0").is_err() {
            eprintln!("Cannot access /dev/input devices.");
            eprintln!("To run without root, add your user to the 'input' group:");
            eprintln!("  sudo usermod -aG input $USER");
            eprintln!("Then log out and log back in, or run with sudo.");
            std::process::exit(1);
        }
    }

    println!("Press CTRL-C to exit.\n");

    let running = Arc::new(AtomicBool::new(true));
    let r = running.clone();

    ctrlc::set_handler(move || {
        r.store(false, Ordering::SeqCst);
    })
    .expect("Error setting Ctrl-C handler");

    platform::run(verbose, running);
}
