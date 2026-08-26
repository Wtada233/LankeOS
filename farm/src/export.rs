//! export.rs — 把构建仓库（`out/<arch>/<pkg>/<ver>.lpkg`）扁平化重打包为发行格式
//! `<pkg>-<ver>.lpkg`（zstd level 22，ultra 最高压缩档），输出到指定目录。
//!
//! 用途：构建仓库（out/）内部用 level 3 快速压缩、频繁重打包；发行/分发用 level 22
//! 一次重压，得到扁平、带版本号、可直传的单文件集合。

use std::fs;
use std::path::{Path, PathBuf};

use crate::{repack, scan};

/// export 报告。
#[derive(Debug, Default)]
pub struct ExportReport {
    pub exported: Vec<String>,
    pub failed: Vec<String>,
}

/// 遍历 `input/<arch>/<pkg>/*.lpkg`，逐个解包 → zstd level 22 重打 → `<pkg>-<ver>.lpkg` 扁平输出。
pub fn export(input: &Path, output: &Path, arch: &str) -> Result<ExportReport, String> {
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
            let out_path = output.join(format!("{pkg}-{ver}.lpkg"));

            // 解包到临时目录 → 重打 level 22；无论成败都清掉临时目录（解出的是 root 属主树，
            // 必须用 sudo 感知的删除，否则 /etc、/var 等目录残留——曾留下 14G 的 .export-extract）
            let extract_dir = output.join(".export-extract").join(&pkg);
            let res = (|| {
                let _ = scan::remove_dir_tree(&extract_dir);
                fs::create_dir_all(&extract_dir).map_err(|e| format!("创建解包目录失败: {e}"))?;
                scan::extract_lpkg(&lpkg, &extract_dir)?;
                repack::export_lpkg(&extract_dir, &out_path)
            })();
            let _ = scan::remove_dir_tree(&extract_dir);

            match res {
                Ok(()) => report.exported.push(format!("{pkg}-{ver}")),
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

        // 输出扁平化命名存在，且 level 22 重打包可解包（round-trip）
        let out = output.join("demo-1.0+1.lpkg");
        assert!(out.exists());
        let extract = tmp.join("extract");
        scan::extract_lpkg(&out, &extract).unwrap();
        let meta = scan::read_metadata_json(&extract.join("metadata.json")).unwrap();
        assert_eq!(meta["name"], "demo");
        assert!(extract.join("content/libfoo.so").exists());

        let _ = fs::remove_dir_all(&tmp);
    }
}
