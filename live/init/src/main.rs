//! LankeOS initramfs init：设置启动介质/squashfs/overlay，注入运行时文件，switch_root。

mod assets;
mod cmdline;
mod disk;
mod inject;
mod loopdev;
mod sys;
mod util;

use std::path::Path;
use std::time::Duration;

use sys::R;

const LABEL_BASE: &str = "LANKE_BASE";
const LABEL_DATA: &str = "LANKE_DATA";

fn main() {
    // PID 1：任何错误都不能直接退出（内核会 panic）。
    match run() {
        Ok(()) => rescue(),
        Err(e) => {
            eprintln!("lanke-init: {e}");
            rescue();
        }
    }
}

fn run() -> R<()> {
    // initramfs 里不一定预置这些目录（pack 产物可能只有 init 本身）。
    for dir in ["/proc", "/sys", "/dev", "/run", "/tmp", "/mnt", "/newroot"] {
        sys::mkdir_p(Path::new(dir))?;
    }

    sys::mount(
        Some(Path::new("proc")),
        Path::new("/proc"),
        Some("proc"),
        0,
        None,
    )?;
    sys::mount(
        Some(Path::new("sysfs")),
        Path::new("/sys"),
        Some("sysfs"),
        0,
        None,
    )?;
    sys::mount(
        Some(Path::new("devtmpfs")),
        Path::new("/dev"),
        Some("devtmpfs"),
        0,
        None,
    )?;

    println!(
        "Welcome to LankeOS GNU/Linux (Kernel {})",
        sys::os_release()
    );

    let cmdline = cmdline::read();
    sys::mkdir_p(Path::new("/mnt"))?;

    mount_boot_medium()?;

    let toram = cmdline::has_arg(&cmdline, "toram");
    if toram {
        setup_toram()?;
    }

    sys::mkdir_p(Path::new("/mnt/rootfs"))?;
    let (loopdev, loop_fd) = loopdev::setup_ro(Path::new("/mnt/medium/live/rootfs.sfs"))?;
    let mounted = sys::mount(
        Some(&loopdev),
        Path::new("/mnt/rootfs"),
        Some("squashfs"),
        libc::MS_RDONLY,
        None,
    );
    // 挂载已经持住 loop 设备，这时才能关闭 fd 让 autoclear 生效。
    sys::close(loop_fd);
    mounted?;

    let mut live = cmdline::has_arg(&cmdline, "live");
    let mut updated = false;

    if !live {
        println!("Persistent mode: checking LANKE_DATA...");
        match disk::find_by_label(LABEL_DATA) {
            Some(dev) => {
                println!("Found data partition: {}", dev.display());
                sys::mkdir_p(Path::new("/mnt/data"))?;
                mount_auto(&dev, Path::new("/mnt/data"))?;

                if cmdline::has_arg(&cmdline, "erase") {
                    println!("Erase requested.");
                    util::remove_children(Path::new("/mnt/data"))?;
                }

                sys::mkdir_p(Path::new("/mnt/data/upper/etc"))?;
                let image = Path::new("/mnt/rootfs/etc/lanke-release");
                let curr = Path::new("/mnt/data/upper/etc/lanke-release-curr");
                if image.is_file() {
                    if !curr.exists() {
                        println!("No release record.");
                        std::fs::copy(image, curr)
                            .map_err(|e| format!("copy {}: {e}", curr.display()))?;
                    } else {
                        println!("Checking release version...");
                        let a = std::fs::read(image)
                            .map_err(|e| format!("read {}: {e}", image.display()))?;
                        let b = std::fs::read(curr)
                            .map_err(|e| format!("read {}: {e}", curr.display()))?;
                        if a == b {
                            println!("Release matches.");
                        } else {
                            println!("Release mismatch detected.");
                            updated = true;
                            live = true;
                        }
                    }
                }
            }
            None => {
                println!("LANKE_DATA not found.");
                live = true;
            }
        }
    }

    sys::mkdir_p(Path::new("/newroot"))?;
    if live {
        setup_live(updated)?;
    } else {
        setup_persistent()?;
    }

    if live {
        inject::installer(Path::new("/newroot"))?;
    }

    sys::mkdir_p(Path::new("/newroot/etc/systemd/system"))?;
    sys::symlink_force(
        Path::new("/dev/null"),
        Path::new("/newroot/etc/systemd/system/ldconfig.service"),
    )?;
    inject::medium_shutdown(Path::new("/newroot"))?;
    sys::sync();

    for (src, dst) in [
        ("/dev", "/newroot/dev"),
        ("/proc", "/newroot/proc"),
        ("/sys", "/newroot/sys"),
    ] {
        sys::mkdir_p(Path::new(dst))?;
        sys::move_mount(Path::new(src), Path::new(dst))?;
    }
    sys::mkdir_p(Path::new("/newroot/run"))?;

    if live {
        sys::mkdir_p(Path::new("/newroot/mnt/lanke_live"))?;
        sys::move_mount(
            Path::new("/mnt/medium"),
            Path::new("/newroot/mnt/lanke_live"),
        )?;
    } else if !toram {
        sys::mkdir_p(Path::new("/newroot/mnt/base"))?;
        sys::move_mount(Path::new("/mnt/medium"), Path::new("/newroot/mnt/base"))?;
    }

    switch_root()
}

fn mount_auto(dev: &Path, target: &Path) -> R<()> {
    const TABLE: &[(&str, libc::c_ulong)] = &[
        ("vfat", 0),
        ("ext4", 0),
        ("exfat", 0),
        ("btrfs", 0),
        ("iso9660", libc::MS_RDONLY),
        ("udf", libc::MS_RDONLY),
    ];
    let mut last = String::new();
    for (fstype, flags) in TABLE {
        match sys::mount(Some(dev), target, Some(fstype), *flags, None) {
            Ok(()) => return Ok(()),
            Err(e) => last = e,
        }
    }
    Err(last)
}

fn mount_boot_medium() -> R<()> {
    println!("Scanning for boot medium with label: {LABEL_BASE}...");
    sys::mkdir_p(Path::new("/mnt/medium"))?;
    for attempt in 1..=5 {
        if let Some(dev) = disk::find_by_label(LABEL_BASE) {
            println!("Found medium: {}", dev.display());
            if mount_auto(&dev, Path::new("/mnt/medium")).is_ok() {
                if Path::new("/mnt/medium/live/rootfs.sfs").is_file() {
                    return Ok(());
                }
                println!("rootfs.sfs not found");
                let _ = sys::umount(Path::new("/mnt/medium"), 0);
            }
        }
        println!("Waiting device ({attempt}/5)");
        std::thread::sleep(Duration::from_secs(1));
    }
    Err("Cannot find LankeOS medium".into())
}

fn setup_toram() -> R<()> {
    println!("Toram detected.");
    sys::mkdir_p(Path::new("/mnt/ram_medium"))?;
    sys::mount(
        Some(Path::new("tmpfs")),
        Path::new("/mnt/ram_medium"),
        Some("tmpfs"),
        0,
        Some("size=90%"),
    )?;

    println!("Copying medium into RAM...");
    util::copy_tree(Path::new("/mnt/medium"), Path::new("/mnt/ram_medium"))?;

    println!("Switching medium to RAM...");
    // 必须在挂载 squashfs 之前完成：loop 设备会打开原介质上的 rootfs.sfs。
    sys::umount(Path::new("/mnt/medium"), 0)?;
    sys::move_mount(Path::new("/mnt/ram_medium"), Path::new("/mnt/medium"))?;
    let _ = std::fs::remove_dir("/mnt/ram_medium");
    println!("Running from RAM.");
    Ok(())
}

fn mount_overlay(base: &Path) -> R<()> {
    let data = format!(
        "lowerdir=/mnt/rootfs,upperdir={}/upper,workdir={}/work,redirect_dir=on",
        base.display(),
        base.display()
    );
    sys::mount(
        Some(Path::new("overlay")),
        Path::new("/newroot"),
        Some("overlay"),
        0,
        Some(&data),
    )
}

fn setup_live(updated: bool) -> R<()> {
    println!("Entering Live mode.");
    sys::mkdir_p(Path::new("/mnt/tmp"))?;
    sys::mount(
        Some(Path::new("tmpfs")),
        Path::new("/mnt/tmp"),
        Some("tmpfs"),
        0,
        Some("mode=0755"),
    )?;
    sys::mkdir_p(Path::new("/mnt/tmp/upper"))?;
    sys::mkdir_p(Path::new("/mnt/tmp/work"))?;
    mount_overlay(Path::new("/mnt/tmp"))?;

    if updated {
        println!("Creating update marker.");
        sys::write_file(Path::new("/newroot/.updated"), b"", None)?;
        sys::mkdir_p(Path::new("/newroot/mnt/data"))?;
        println!("Binding old LANKE_DATA into live environment.");
        sys::move_mount(Path::new("/mnt/data"), Path::new("/newroot/mnt/data"))?;
    }

    println!("Applying Live patches...");
    if Path::new("/newroot/etc/shadow").is_file() {
        util::clear_root_password(Path::new("/newroot/etc/shadow"))?;
    }

    let fish = util::detect_shell(Path::new("/newroot/etc/passwd")) == "fish";
    let rc = if fish {
        "home/LankeOS/.config/fish/config.fish"
    } else {
        "home/LankeOS/.bashrc"
    };
    inject::autologin(Path::new("/newroot"))?;
    inject::live_notice(Path::new("/newroot"), rc, fish)?;
    if updated {
        inject::update_notice(Path::new("/newroot"), rc, fish)?;
    }

    sys::mkdir_p(Path::new("/mnt/tmp/containerd"))?;
    sys::mkdir_p(Path::new("/newroot/var/lib/containerd"))?;
    sys::bind_mount(
        Path::new("/mnt/tmp/containerd"),
        Path::new("/newroot/var/lib/containerd"),
    )?;
    Ok(())
}

fn setup_persistent() -> R<()> {
    println!("Starting persistent overlay.");
    sys::mkdir_p(Path::new("/mnt/data/upper"))?;
    sys::mkdir_p(Path::new("/mnt/data/work"))?;
    mount_overlay(Path::new("/mnt/data"))?;

    sys::mkdir_p(Path::new("/mnt/data/containerd"))?;
    sys::mkdir_p(Path::new("/newroot/var/lib/containerd"))?;
    sys::bind_mount(
        Path::new("/mnt/data/containerd"),
        Path::new("/newroot/var/lib/containerd"),
    )?;
    Ok(())
}

fn switch_root() -> R<()> {
    let target = if sys::is_exec(Path::new("/newroot/sbin/init")) {
        "/sbin/init"
    } else {
        "/usr/bin/init"
    };
    println!("Transitioning to LankeOS...");

    sys::chdir(Path::new("/newroot"))?;
    sys::mount(Some(Path::new(".")), Path::new("/"), None, libc::MS_MOVE, None)?;
    sys::chroot(Path::new("."))?;
    sys::chdir(Path::new("/"))?;
    sys::exec(Path::new(target))
}

fn rescue() -> ! {
    eprintln!("lanke-init: dropping to rescue shell");
    let _ = sys::exec(Path::new("/bin/sh"));
    eprintln!("lanke-init: no rescue shell in initramfs; PID 1 is stuck");
    loop {
        std::thread::sleep(Duration::from_secs(3600));
    }
}
