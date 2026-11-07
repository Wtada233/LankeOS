//! 把运行时文件写入新根：内容全部来自 assets.rs。

use std::path::Path;

use crate::assets;
use crate::sys::{self, R};
use crate::util;

pub const UPDATE_GUARD: &str = "/.updated";

pub fn autologin(root: &Path) -> R<()> {
    for rel in [
        "etc/systemd/system/getty@tty1.service.d/autologin.conf",
        "etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf",
    ] {
        sys::write_file(&sys::root_join(root, rel), assets::AUTOLOGIN.as_bytes(), None)?;
    }
    Ok(())
}

pub fn notice(root: &Path, rel: &str, content: &str, guard: Option<&str>, fish: bool) -> R<()> {
    util::append_echo_lines(&sys::root_join(root, rel), content, guard, fish)
}

pub fn live_notice(root: &Path, rel: &str, fish: bool) -> R<()> {
    notice(root, rel, assets::LIVE_NOTICE, None, fish)
}

pub fn update_notice(root: &Path, rel: &str, fish: bool) -> R<()> {
    notice(root, rel, assets::UPDATE_NOTICE, Some(UPDATE_GUARD), fish)
}

pub fn installer(root: &Path) -> R<()> {
    sys::write_file(
        &sys::root_join(root, "usr/bin/lanke_install"),
        assets::INSTALLER.as_bytes(),
        Some(0o755),
    )
}

pub fn medium_shutdown(root: &Path) -> R<()> {
    sys::write_file(
        &sys::root_join(root, "etc/systemd/system/lanke-medium-shutdown.service"),
        assets::MEDIUM_SHUTDOWN.as_bytes(),
        None,
    )?;
    for rel in [
        "etc/systemd/system/mnt-lanke_live.mount.d/lazy-unmount.conf",
        "etc/systemd/system/mnt-base.mount.d/lazy-unmount.conf",
    ] {
        sys::write_file(
            &sys::root_join(root, rel),
            assets::MEDIUM_LAZY.as_bytes(),
            None,
        )?;
    }
    sys::symlink_force(
        Path::new("../lanke-medium-shutdown.service"),
        &sys::root_join(
            root,
            "etc/systemd/system/multi-user.target.wants/lanke-medium-shutdown.service",
        ),
    )
}
