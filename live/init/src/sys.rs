//! mount/umount/switch_root 等系统调用封装。

use std::ffi::CString;
use std::io;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};

pub type R<T> = Result<T, String>;

fn cpath(p: &Path) -> R<CString> {
    CString::new(p.as_os_str().as_bytes())
        .map_err(|_| format!("path contains NUL: {}", p.display()))
}

fn cstr(s: &str) -> R<CString> {
    CString::new(s).map_err(|_| format!("string contains NUL: {s}"))
}

pub fn mount(
    src: Option<&Path>,
    target: &Path,
    fstype: Option<&str>,
    flags: libc::c_ulong,
    data: Option<&str>,
) -> R<()> {
    let src = match src {
        Some(s) => Some(cpath(s)?),
        None => None,
    };
    let target_c = cpath(target)?;
    let fstype_c = match fstype {
        Some(f) => Some(cstr(f)?),
        None => None,
    };
    let data_c = match data {
        Some(d) => Some(cstr(d)?),
        None => None,
    };

    let rc = unsafe {
        libc::mount(
            src.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            target_c.as_ptr(),
            fstype_c.as_ref().map_or(std::ptr::null(), |c| c.as_ptr()),
            flags,
            data_c
                .as_ref()
                .map_or(std::ptr::null(), |c| c.as_ptr() as *const libc::c_void),
        )
    };
    if rc < 0 {
        Err(format!(
            "mount {}: {}",
            target.display(),
            io::Error::last_os_error()
        ))
    } else {
        Ok(())
    }
}

pub fn umount(target: &Path, flags: libc::c_int) -> R<()> {
    let target_c = cpath(target)?;
    let rc = unsafe { libc::umount2(target_c.as_ptr(), flags) };
    if rc < 0 {
        Err(format!(
            "umount {}: {}",
            target.display(),
            io::Error::last_os_error()
        ))
    } else {
        Ok(())
    }
}

pub fn move_mount(src: &Path, target: &Path) -> R<()> {
    mount(Some(src), target, None, libc::MS_MOVE, None)
}

pub fn bind_mount(src: &Path, target: &Path) -> R<()> {
    mount(Some(src), target, None, libc::MS_BIND, None)
}

pub fn mkdir_p(path: &Path) -> R<()> {
    std::fs::create_dir_all(path).map_err(|e| format!("mkdir {}: {e}", path.display()))
}

pub fn symlink_force(target: &Path, link: &Path) -> R<()> {
    if let Some(parent) = link.parent() {
        mkdir_p(parent)?;
    }
    match std::fs::remove_file(link) {
        Ok(()) => {}
        Err(e) if e.kind() == io::ErrorKind::NotFound => {}
        Err(e) => return Err(format!("remove {}: {e}", link.display())),
    }
    std::os::unix::fs::symlink(target, link)
        .map_err(|e| format!("symlink {} -> {}: {e}", link.display(), target.display()))
}

pub fn write_file(path: &Path, data: &[u8], mode: Option<u32>) -> R<()> {
    if let Some(parent) = path.parent() {
        mkdir_p(parent)?;
    }
    std::fs::write(path, data).map_err(|e| format!("write {}: {e}", path.display()))?;
    if let Some(mode) = mode {
        std::fs::set_permissions(path, std::fs::Permissions::from_mode(mode))
            .map_err(|e| format!("chmod {}: {e}", path.display()))?;
    }
    Ok(())
}

pub fn is_exec(path: &Path) -> bool {
    match std::fs::metadata(path) {
        Ok(m) => m.is_file() && m.permissions().mode() & 0o111 != 0,
        Err(_) => false,
    }
}

pub fn sync() {
    unsafe { libc::sync() }
}

pub fn chroot(path: &Path) -> R<()> {
    let path_c = cpath(path)?;
    let rc = unsafe { libc::chroot(path_c.as_ptr()) };
    if rc < 0 {
        Err(format!(
            "chroot {}: {}",
            path.display(),
            io::Error::last_os_error()
        ))
    } else {
        Ok(())
    }
}

pub fn chdir(path: &Path) -> R<()> {
    std::env::set_current_dir(path).map_err(|e| format!("chdir {}: {e}", path.display()))
}

pub fn exec(path: &Path) -> R<()> {
    let path_c = cpath(path)?;
    let argv = [path_c.as_ptr(), std::ptr::null()];
    unsafe { libc::execv(path_c.as_ptr(), argv.as_ptr()) };
    Err(format!(
        "exec {}: {}",
        path.display(),
        io::Error::last_os_error()
    ))
}

pub fn open(path: &Path, flags: libc::c_int) -> io::Result<libc::c_int> {
    let path_c = CString::new(path.as_os_str().as_bytes())
        .map_err(|_| io::Error::new(io::ErrorKind::InvalidInput, "path contains NUL"))?;
    let fd = unsafe { libc::open(path_c.as_ptr(), flags) };
    if fd < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(fd)
    }
}

pub fn close(fd: libc::c_int) {
    unsafe {
        libc::close(fd);
    }
}

pub fn ioctl(
    fd: libc::c_int,
    request: libc::c_ulong,
    arg: libc::c_ulong,
) -> io::Result<libc::c_int> {
    // glibc 的 ioctl request 是 c_ulong，musl 是 c_int。
    let rc = unsafe { libc::ioctl(fd, request as libc::Ioctl, arg) };
    if rc < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(rc)
    }
}

pub fn os_release() -> String {
    std::fs::read_to_string("/proc/sys/kernel/osrelease")
        .map(|s| s.trim().to_string())
        .unwrap_or_else(|_| "unknown".into())
}

pub fn root_join(root: &Path, rel: &str) -> PathBuf {
    root.join(rel)
}
