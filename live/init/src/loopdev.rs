//! 只读 loop 设备设置（替代 `mount -o ro,loop`）。

use std::path::{Path, PathBuf};

use crate::sys;

const LOOP_CTL_GET_FREE: libc::c_ulong = 0x4C82;
const LOOP_SET_FD: libc::c_ulong = 0x4C00;
const LOOP_CLR_FD: libc::c_ulong = 0x4C01;
const LOOP_SET_STATUS64: libc::c_ulong = 0x4C04;

const LO_FLAGS_READ_ONLY: u32 = 1;
const LO_FLAGS_AUTOCLEAR: u32 = 4;

#[repr(C)]
#[derive(Clone, Copy)]
struct LoopInfo64 {
    lo_device: u64,
    lo_inode: u64,
    lo_rdevice: u64,
    lo_offset: u64,
    lo_sizelimit: u64,
    lo_number: u32,
    lo_encrypt_type: u32,
    lo_encrypt_key_size: u32,
    lo_flags: u32,
    lo_file_name: [u8; 64],
    lo_crypt_name: [u8; 64],
    lo_encrypt_key: [u8; 32],
    lo_init: [u64; 2],
}

/// 把 backing file 设成一个只读、autoclear 的 loop 设备。
/// 返回设备路径和 loop 设备的 fd：调用方必须在挂载完成后再 close，
/// 否则 autoclear 会在挂载前把 loop 清掉（读到 EIO）。
pub fn setup_ro(backing: &Path) -> sys::R<(PathBuf, libc::c_int)> {
    let ctl = sys::open(Path::new("/dev/loop-control"), libc::O_RDWR | libc::O_CLOEXEC)
        .map_err(|e| format!("open /dev/loop-control: {e}"))?;
    let idx = sys::ioctl(ctl, LOOP_CTL_GET_FREE, 0)
        .map_err(|e| format!("LOOP_CTL_GET_FREE: {e}"))?;
    sys::close(ctl);
    if idx < 0 {
        return Err("LOOP_CTL_GET_FREE returned negative index".into());
    }

    let dev = PathBuf::from(format!("/dev/loop{idx}"));
    let loop_fd = sys::open(&dev, libc::O_RDWR | libc::O_CLOEXEC)
        .map_err(|e| format!("open {}: {e}", dev.display()))?;
    let backing_fd = match sys::open(backing, libc::O_RDONLY | libc::O_CLOEXEC) {
        Ok(fd) => fd,
        Err(e) => {
            sys::close(loop_fd);
            return Err(format!("open {}: {e}", backing.display()));
        }
    };

    if let Err(e) = sys::ioctl(loop_fd, LOOP_SET_FD, backing_fd as libc::c_ulong) {
        sys::close(backing_fd);
        sys::close(loop_fd);
        return Err(format!("LOOP_SET_FD {}: {e}", dev.display()));
    }
    sys::close(backing_fd);

    let mut info: LoopInfo64 = unsafe { std::mem::zeroed() };
    info.lo_flags = LO_FLAGS_READ_ONLY | LO_FLAGS_AUTOCLEAR;
    let name = backing.to_string_lossy();
    let name = name.as_bytes();
    let n = name.len().min(info.lo_file_name.len() - 1);
    info.lo_file_name[..n].copy_from_slice(&name[..n]);

    let status = sys::ioctl(
        loop_fd,
        LOOP_SET_STATUS64,
        &info as *const LoopInfo64 as libc::c_ulong,
    );
    if let Err(e) = status {
        let _ = sys::ioctl(loop_fd, LOOP_CLR_FD, 0);
        sys::close(loop_fd);
        return Err(format!("LOOP_SET_STATUS64 {}: {e}", dev.display()));
    }

    Ok((dev, loop_fd))
}
