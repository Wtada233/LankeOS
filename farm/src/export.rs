//! export.rs — 把构建仓库（`out/<arch>/<pkg>/<ver>.lpkg`）扁平化为发行布局
//! `<pkg>-<ver>.lpkg`，输出到指定目录。**纯文件复制，不重打包**。
//!
//! build/validate 成功已把每个 .lpkg 归一化为 zstd level 22 + mtime 1970（§4 无条件归一化重打），
//! export 只按 `<pkg>-<ver>` 重命名复制 → 得到扁平、带版本号、可直传的单文件集合。

use crate::error::FarmError;
use std::fs;
use std::path::{Path, PathBuf};

/// export 报告。
#[derive(Debug, Default)]
pub struct ExportReport {
    pub exported: Vec<String>,
    pub failed: Vec<String>,
}

/// 名字安全校验（纵深防御）：包名/版本会被拼进输出文件名。虽 `file_name()`/`file_stem()` 取的是单个
/// 路径分量（正常路径不可能越界），但挂载了不可信卷/畸形条目时仍须拒绝——含路径分隔符或 `..`
/// 一律不导出并记 failed（曾无校验直拼）。
fn safe_component(s: &str) -> bool {
    !s.is_empty() && !s.contains('/') && !s.contains('\\') && !s.contains("..")
}

/// 遍历 `input/<arch>/<pkg>/*.lpkg`，逐个**复制**为 `<pkg>-<ver>.lpkg` 扁平输出（字节即仓库产物，
/// 已 level 22 + mtime 1970 归一化，无需再解压/重压）。
pub fn export(input: &Path, output: &Path, arch: &str) -> Result<ExportReport, FarmError> {
    fs::create_dir_all(output).map_err(|e| format!("创建输出目录 {output:?} 失败: {e}"))?;
    let repo_root = input.join(arch);
    let mut report = ExportReport::default();

    let entries = fs::read_dir(&repo_root).map_err(|e| format!("读取 {repo_root:?} 失败: {e}"))?;
    let mut pkgs: Vec<PathBuf> = entries
        .filter_map(|e| e.ok().map(|e| e.path()))
        .filter(|p| p.is_dir())
        .collect();
    pkgs.sort();

    for pkgdir in pkgs {
        let Some(pkg) = pkgdir
            .file_name()
            .and_then(|n| n.to_str())
            .map(String::from)
        else {
            continue;
        };
        if !safe_component(&pkg) {
            report.failed.push(format!("{pkg}: 包名含非法字符，跳过"));
            continue;
        }
        let mut lpkg_files: Vec<PathBuf> = fs::read_dir(&pkgdir)
            .ok()
            .map(|it| {
                it.filter_map(|e| e.ok().map(|e| e.path()))
                    .filter(|p| p.extension().is_some_and(|x| x == "lpkg"))
                    .collect()
            })
            .unwrap_or_default();
        lpkg_files.sort();
        if lpkg_files.is_empty() {
            report.failed.push(format!("{pkg}: 无 .lpkg"));
            continue;
        }

        for lpkg in lpkg_files {
            let Some(ver) = lpkg.file_stem().and_then(|s| s.to_str()).map(String::from) else {
                report
                    .failed
                    .push(format!("{pkg}: 无法解析 .lpkg 文件名版本"));
                continue;
            };
            if !safe_component(&ver) {
                report
                    .failed
                    .push(format!("{pkg}: 版本名 {ver:?} 含非法字符，跳过"));
                continue;
            }
            let out_path = output.join(format!("{pkg}-{ver}.lpkg"));
            match fs::copy(&lpkg, &out_path) {
                Ok(_) => report.exported.push(format!("{pkg}-{ver}")),
                Err(e) => report.failed.push(format!("{pkg}-{ver}: {e}")),
            }
        }
    }
    Ok(report)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::path::PathBuf;

    /// 合成一个最小 .lpkg（zstd tar：metadata.json + content/libfoo.so 假 ELF）。
    fn make_lpkg(out_path: &Path, name: &str, version: &str) -> PathBuf {
        let src = out_path.with_extension("src");
        let _ = fs::remove_dir_all(&src);
        fs::create_dir_all(src.join("content")).unwrap();
        fs::write(
            src.join("content/libfoo.so"),
            [0x7f, b'E', b'L', b'F', 2, 1, 1],
        )
        .unwrap();
        let meta = serde_json::json!({
            "name": name, "version": version,
            "deps": [], "provides": [], "needed_so": [],
        });
        fs::write(
            src.join("metadata.json"),
            serde_json::to_string(&meta).unwrap(),
        )
        .unwrap();
        let f = fs::File::create(out_path).unwrap();
        let enc = zstd::stream::write::Encoder::new(f, 3).unwrap();
        let mut b = tar::Builder::new(enc);
        b.append_dir_all(".", &src).unwrap();
        let enc = b.into_inner().unwrap();
        enc.finish().unwrap();
        let _ = fs::remove_dir_all(&src);
        out_path.to_path_buf()
    }

    #[test]
    fn export_rejects_unsafe_pkg_or_version_names() {
        let tmp = std::env::temp_dir().join(format!("farm-export-unsafe-{}", std::process::id()));
        let _ = fs::remove_dir_all(&tmp);
        let input = tmp.join("out");
        let output = tmp.join("export");
        // 合法包
        let good = input.join("x86_64").join("ok");
        fs::create_dir_all(&good).unwrap();
        make_lpkg(&good.join("1.0.lpkg"), "ok", "1.0");
        // 非法版本名（含 ".."）
        let bad = input.join("x86_64").join("evil");
        fs::create_dir_all(&bad).unwrap();
        make_lpkg(&bad.join("1.0..2.lpkg"), "evil", "1.0..2");

        let report = export(&input, &output, "x86_64").unwrap();
        assert!(report.exported.contains(&"ok-1.0".to_string()));
        assert!(
            report.failed.iter().any(|f| f.contains("evil")),
            "含 .. 的版本名应记 failed: {:?}",
            report.failed
        );
        assert!(
            !output.join("evil-1.0..2.lpkg").exists(),
            "非法名不得产生输出"
        );
        let _ = fs::remove_dir_all(&tmp);
    }

    #[test]
    fn export_flattens_and_renames() {
        let tmp = std::env::temp_dir().join(format!("farm-export-test-{}", std::process::id()));
        let input = tmp.join("out");
        let output = tmp.join("export");
        let pkgdir = input.join("x86_64").join("demo");
        fs::create_dir_all(&pkgdir).unwrap();
        make_lpkg(&pkgdir.join("1.0+1.lpkg"), "demo", "1.0+1");

        let report = export(&input, &output, "x86_64").unwrap();
        assert_eq!(report.exported, vec!["demo-1.0+1"]);
        assert!(report.failed.is_empty());

        // 输出 = 仓库 .lpkg 的**字节副本**（只扁平化，不重打包）
        let out = output.join("demo-1.0+1.lpkg");
        assert!(out.exists());
        assert_eq!(
            fs::read(&out).unwrap(),
            fs::read(pkgdir.join("1.0+1.lpkg")).unwrap(),
            "export 应原样复制，不改字节"
        );
        // 仍是合法 .lpkg，可正常解包（round-trip）
        let extract = tmp.join("extract");
        crate::scan::extract_lpkg(&out, &extract).unwrap();
        let meta = crate::scan::read_metadata_json(&extract.join("metadata.json")).unwrap();
        assert_eq!(meta["name"], "demo");
        assert!(extract.join("content/libfoo.so").exists());

        let _ = fs::remove_dir_all(&tmp);
    }
}
