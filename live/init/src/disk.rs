//! 按卷标查找块设备（替代 busybox findfs，不依赖 udev/blkid 工具）。

use std::fs::File;
use std::io::{self, Read, Seek, SeekFrom};
use std::path::{Path, PathBuf};

fn trim_label(bytes: &[u8]) -> String {
    let end = bytes
        .iter()
        .rposition(|&b| b != b' ' && b != 0)
        .map_or(0, |i| i + 1);
    String::from_utf8_lossy(&bytes[..end]).into_owned()
}

/// 读取一个块设备上的卷标：FAT12/16/32、ext2/3/4、ISO9660。
fn read_label(dev: &Path) -> io::Result<Option<String>> {
    let mut f = File::open(dev)?;

    let mut boot = [0u8; 512];
    if f.read_exact(&mut boot).is_err() {
        return Ok(None);
    }

    // FAT12/16/32
    if boot[510] == 0x55 && boot[511] == 0xAA {
        let bps = u16::from_le_bytes([boot[11], boot[12]]);
        if matches!(bps, 512 | 1024 | 2048 | 4096) {
            if &boot[54..57] == b"FAT" {
                return Ok(Some(trim_label(&boot[43..54])));
            }
            if &boot[82..87] == b"FAT32" {
                return Ok(Some(trim_label(&boot[71..82])));
            }
        }
    }

    // ext2/3/4：超级块在 1024，s_magic 在 0x38，卷标在 0x78
    let mut sb = [0u8; 1024];
    f.seek(SeekFrom::Start(1024))?;
    if f.read_exact(&mut sb).is_ok() {
        let magic = u16::from_le_bytes([sb[0x38], sb[0x39]]);
        if magic == 0xEF53 {
            return Ok(Some(trim_label(&sb[0x78..0x88])));
        }
    }

    // ISO9660：主卷描述符在 32768，卷标 32 字节 @ 40
    let mut pvd = [0u8; 2048];
    f.seek(SeekFrom::Start(32768))?;
    if f.read_exact(&mut pvd).is_ok() && pvd[0] == 0x01 && &pvd[1..6] == b"CD001" {
        return Ok(Some(trim_label(&pvd[40..72])));
    }

    Ok(None)
}

/// 扫描 /sys/class/block，返回第一个卷标匹配的设备节点。
pub fn find_by_label(label: &str) -> Option<PathBuf> {
    let mut names: Vec<String> = std::fs::read_dir("/sys/class/block")
        .ok()?
        .filter_map(|e| e.ok())
        .filter_map(|e| e.file_name().into_string().ok())
        .filter(|n| !n.starts_with("loop") && !n.starts_with("ram"))
        .collect();
    names.sort();

    for name in names {
        let dev = PathBuf::from("/dev").join(name);
        if let Ok(Some(found)) = read_label(&dev) {
            if found == label {
                return Some(dev);
            }
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::trim_label;

    #[test]
    fn label_trimming() {
        assert_eq!(trim_label(b"LANKE_BASE "), "LANKE_BASE");
        assert_eq!(trim_label(b"LANKE_DATA\0\0\0"), "LANKE_DATA");
        assert_eq!(trim_label(b"           "), "");
    }
}
