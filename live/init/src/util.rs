//! 文件树操作、passwd/shadow 处理、shell rc 注入。

use std::io::Write;
use std::path::Path;

use crate::sys::{self, R};

pub fn copy_tree(src: &Path, dst: &Path) -> R<()> {
    sys::mkdir_p(dst)?;
    let entries = std::fs::read_dir(src).map_err(|e| format!("read_dir {}: {e}", src.display()))?;
    for entry in entries {
        let entry = entry.map_err(|e| format!("read_dir {}: {e}", src.display()))?;
        let from = entry.path();
        let to = dst.join(entry.file_name());
        let ft = entry
            .file_type()
            .map_err(|e| format!("stat {}: {e}", from.display()))?;
        if ft.is_dir() {
            copy_tree(&from, &to)?;
        } else if ft.is_symlink() {
            let target = std::fs::read_link(&from)
                .map_err(|e| format!("readlink {}: {e}", from.display()))?;
            std::os::unix::fs::symlink(&target, &to)
                .map_err(|e| format!("symlink {}: {e}", to.display()))?;
        } else if ft.is_file() {
            std::fs::copy(&from, &to)
                .map_err(|e| format!("copy {} -> {}: {e}", from.display(), to.display()))?;
        }
    }
    Ok(())
}

pub fn remove_children(dir: &Path) -> R<()> {
    let entries = std::fs::read_dir(dir).map_err(|e| format!("read_dir {}: {e}", dir.display()))?;
    for entry in entries {
        let entry = entry.map_err(|e| format!("read_dir {}: {e}", dir.display()))?;
        let path = entry.path();
        let ft = entry
            .file_type()
            .map_err(|e| format!("stat {}: {e}", path.display()))?;
        let result = if ft.is_dir() {
            std::fs::remove_dir_all(&path)
        } else {
            std::fs::remove_file(&path)
        };
        result.map_err(|e| format!("remove {}: {e}", path.display()))?;
    }
    Ok(())
}

/// 解析 /etc/passwd，返回默认用户的登录 shell 类型：fish 或 bash。
pub fn detect_shell(passwd: &Path) -> &'static str {
    if let Ok(content) = std::fs::read_to_string(passwd) {
        for line in content.lines() {
            let fields: Vec<&str> = line.split(':').collect();
            if fields.len() >= 7 && fields[0] == "LankeOS" {
                return if fields[6].contains("fish") {
                    "fish"
                } else {
                    "bash"
                };
            }
        }
    }
    "bash"
}

/// 把 shadow 里 root 的密码字段清空（live 默认无密码）。
pub fn clear_root_password(path: &Path) -> R<()> {
    let data =
        std::fs::read_to_string(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let mut out = String::with_capacity(data.len());
    for line in data.split_inclusive('\n') {
        if line.starts_with("root:") {
            let first = match line.find(':') {
                Some(i) => i,
                None => {
                    out.push_str(line);
                    continue;
                }
            };
            let second = match line[first + 1..].find(':') {
                Some(i) => first + 1 + i,
                None => {
                    out.push_str(line);
                    continue;
                }
            };
            out.push_str(&line[..first + 1]);
            out.push_str(&line[second..]);
            continue;
        }
        out.push_str(line);
    }
    std::fs::write(path, out).map_err(|e| format!("write {}: {e}", path.display()))
}

pub fn shell_quote(s: &str) -> String {
    s.replace('\'', "'\\''")
}

/// 复刻 modules/inject 的 echo 模式：把内容逐行变成 echo '<line>' 追加到目标。
pub fn append_echo_lines(path: &Path, content: &str, guard: Option<&str>, fish: bool) -> R<()> {
    let mut out = String::new();
    if let Some(guard) = guard {
        if fish {
            out.push_str(&format!("if test -f {guard}\n"));
        } else {
            out.push_str(&format!("if [ -f {guard} ]; then\n"));
        }
    }
    for line in content.lines() {
        out.push_str(&format!("echo '{}'\n", shell_quote(line)));
    }
    if guard.is_some() {
        out.push_str(if fish { "end\n" } else { "fi\n" });
    }

    if let Some(parent) = path.parent() {
        sys::mkdir_p(parent)?;
    }
    let mut file = std::fs::OpenOptions::new()
        .create(true)
        .append(true)
        .open(path)
        .map_err(|e| format!("open {}: {e}", path.display()))?;
    file.write_all(out.as_bytes())
        .map_err(|e| format!("append {}: {e}", path.display()))
}

#[cfg(test)]
mod tests {
    use super::{clear_root_password, detect_shell, shell_quote};

    #[test]
    fn root_password_is_cleared() {
        let dir = std::env::temp_dir().join(format!("lanke-init-test-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let shadow = dir.join("shadow");
        std::fs::write(
            &shadow,
            "root:$6$abc:19000:0:99999:7:::\nLankeOS:x:1:1:::\n",
        )
        .unwrap();
        clear_root_password(&shadow).unwrap();
        let got = std::fs::read_to_string(&shadow).unwrap();
        assert_eq!(got, "root::19000:0:99999:7:::\nLankeOS:x:1:1:::\n");
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn detects_fish_and_bash() {
        let dir = std::env::temp_dir().join(format!("lanke-init-passwd-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let passwd = dir.join("passwd");
        std::fs::write(
            &passwd,
            "LankeOS:x:1000:1000::/home/LankeOS:/usr/bin/fish\n",
        )
        .unwrap();
        assert_eq!(detect_shell(&passwd), "fish");
        std::fs::write(
            &passwd,
            "LankeOS:x:1000:1000::/home/LankeOS:/usr/bin/bash\n",
        )
        .unwrap();
        assert_eq!(detect_shell(&passwd), "bash");
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn quotes_single_quotes() {
        assert_eq!(shell_quote("a'b"), "a'\\''b");
    }
}
