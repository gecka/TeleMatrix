//! Opt-in runtime instrumentation behind `TM_RUNTIME_STATS=1`.
//!
//! `docs/thread-count-review-2026-07-08.md` describes a `thread_stats.rs` with
//! this shape as landed; it never was (no file, no git history). This is the
//! real thing, written to answer "where is the CPU going" during a repro:
//!
//! * a process CPU ticker (`getrusage`) so the tracing timeline carries user/sys
//!   ms per interval — the in-app equivalent of watching %CPU, but correlated
//!   with the other probes instead of read off a separate tool;
//! * OS thread count, because the SQLite deadpools feed tokio's blocking pool
//!   and the total is the number that moved in the earlier reviews;
//! * per-runtime tokio metrics, since multiaccount means up to six runtimes and
//!   an aggregate process number cannot say which one is busy.
//!
//! Logged at **warn** under this crate's module path on purpose: the default
//! filter is `warn,telematrix_protocol=debug`, and an earlier attempt that used
//! `info!` with a custom `target:` was silently dropped by it.
//!
//! Gotcha worth repeating: on macOS a `.app` launched from Finder or `open`
//! does not inherit the shell environment. Run the inner Mach-O directly:
//! `TM_RUNTIME_STATS=1 build/TeleMatrix.app/Contents/MacOS/TeleMatrix`.

use std::sync::Once;
use std::time::Duration;

use tokio::runtime::Handle;
use tracing::warn;

const INTERVAL: Duration = Duration::from_secs(5);

/// True when `TM_RUNTIME_STATS` is set to anything but `0`/empty.
pub(crate) fn enabled() -> bool {
    static ENABLED: std::sync::OnceLock<bool> = std::sync::OnceLock::new();
    *ENABLED.get_or_init(|| match std::env::var("TM_RUNTIME_STATS") {
        Ok(v) => !v.is_empty() && v != "0",
        Err(_) => false,
    })
}

/// Cumulative process CPU time (user, system) in milliseconds.
#[cfg(unix)]
fn process_cpu_ms() -> Option<(u64, u64)> {
    // SAFETY: getrusage writes a fully-initialised rusage into our stack slot;
    // we only read it when it reports success.
    unsafe {
        let mut usage: libc::rusage = std::mem::zeroed();
        if libc::getrusage(libc::RUSAGE_SELF, &mut usage) != 0 {
            return None;
        }
        let to_ms = |t: libc::timeval| (t.tv_sec as u64) * 1000 + (t.tv_usec as u64) / 1000;
        Some((to_ms(usage.ru_utime), to_ms(usage.ru_stime)))
    }
}

#[cfg(not(unix))]
fn process_cpu_ms() -> Option<(u64, u64)> {
    None
}

/// Live OS thread count for this process.
#[cfg(target_os = "macos")]
fn os_thread_count() -> Option<u64> {
    // SAFETY: proc_pidinfo fills the struct it is given and reports how many
    // bytes it wrote; we only trust the result on an exact-size write.
    unsafe {
        let mut info: libc::proc_taskinfo = std::mem::zeroed();
        let size = std::mem::size_of::<libc::proc_taskinfo>() as libc::c_int;
        let written = libc::proc_pidinfo(
            std::process::id() as libc::c_int,
            libc::PROC_PIDTASKINFO,
            0,
            (&mut info as *mut libc::proc_taskinfo).cast(),
            size,
        );
        (written == size).then_some(info.pti_threadnum as u64)
    }
}

#[cfg(target_os = "linux")]
fn os_thread_count() -> Option<u64> {
    let status = std::fs::read_to_string("/proc/self/status").ok()?;
    status
        .lines()
        .find_map(|l| l.strip_prefix("Threads:"))
        .and_then(|v| v.trim().parse().ok())
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn os_thread_count() -> Option<u64> {
    None
}

/// Spawn the tickers for one account's runtime. Safe to call once per Handle:
/// the process-wide half is behind a `Once`, so six accounts do not log the
/// same CPU and thread numbers six times.
pub(crate) fn spawn(runtime: &tokio::runtime::Runtime, account: u32) {
    if !enabled() {
        return;
    }

    static PROCESS_TICKER: Once = Once::new();
    PROCESS_TICKER.call_once(|| {
        runtime.spawn(async move {
            let mut last = process_cpu_ms();
            loop {
                tokio::time::sleep(INTERVAL).await;
                let now = process_cpu_ms();
                let (user_ms, sys_ms) = match (last, now) {
                    (Some((pu, ps)), Some((nu, ns))) => {
                        (nu.saturating_sub(pu), ns.saturating_sub(ps))
                    }
                    _ => (0, 0),
                };
                last = now;
                // Percent of one core over the interval, so the number lines up
                // with what Activity Monitor reports.
                let cpu_pct = (user_ms + sys_ms) as f64 * 100.0 / INTERVAL.as_millis() as f64;
                warn!(
                    user_ms,
                    sys_ms,
                    cpu_pct = format_args!("{cpu_pct:.1}"),
                    os_threads = os_thread_count().unwrap_or(0),
                    "[stats] process"
                );
            }
        });
    });

    let handle = runtime.handle().clone();
    runtime.spawn(async move {
        loop {
            tokio::time::sleep(INTERVAL).await;
            let m = Handle::metrics(&handle);
            // Blocking-pool counts need `tokio_unstable`; the process-wide
            // os_threads above already tracks that pool's growth, which is what
            // the earlier thread-count reviews were measuring anyway.
            warn!(
                account,
                workers = m.num_workers(),
                alive_tasks = m.num_alive_tasks(),
                queue_depth = m.global_queue_depth(),
                "[stats] runtime"
            );
        }
    });
}
