//! abidb.rs — farm 自己的 SONAME 数据库（ABI 传播基线）。
//!
//! 容器看到的 `out/<arch>/index.txt` 剥掉 `needed_so`（只留 provides + deps），lpkg 的
//! SONAME 一致性检查（check_forward_soname_integrity / check_needed_so_consistency）全部失效，
//! 容器构建不再被 ABI 断裂/bootstrap 环硬报错卡死。而 farm 的 ABI 传播（removed_sonames /
//! revmap / link_deps）从**这个库**读完整的 provides + needed_so。
//!
//! 每次 repack 时三处同步写：LankeBUILD.json、.lpkg metadata.json、本库（用户规则）。
//! 存储：`out/<arch>/.abi.json`，每包 `{version, provides, needed_so}`（seed 从远端索引全量灌入，
//! 构建后 update_pkg 单包刷新）。

use std::collections::HashMap;
use std::path::Path;

use crate::graph::{Index, PkgInfo};

fn db_path(out_dir: &Path, arch: &str) -> std::path::PathBuf {
    out_dir.join(arch).join(".abi.json")
}

fn arr(v: &serde_json::Value) -> Vec<String> {
    v.as_array()
        .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect())
        .unwrap_or_default()
}

/// 读原始数据库（不存在时返回空 Index，供 update_pkg 自举）。
fn read_db(out_dir: &Path, arch: &str) -> Result<Index, String> {
    let path = db_path(out_dir, arch);
    let Ok(text) = std::fs::read_to_string(&path) else {
        return Ok(Index::default());
    };
    let raw: serde_json::Value = serde_json::from_str(&text)
        .map_err(|e| format!("解析 ABI 数据库 {path:?} 失败: {e}"))?;
    let mut packages = HashMap::new();
    for (name, v) in raw.as_object().ok_or("ABI 数据库格式错误：顶层应为对象")? {
        packages.insert(
            name.clone(),
            PkgInfo {
                name: name.clone(),
                version: v["version"].as_str().unwrap_or("").to_string(),
                sha256: String::new(),
                deps: Vec::new(),
                provides: arr(&v["provides"]),
                needed_so: arr(&v["needed_so"]),
            },
        );
    }
    Ok(Index::from_packages(packages))
}

/// 读 ABI 数据库为 Index（ABI 传播的旧索引基线）。缺失/为空 → 报错（禁止无基线构建）。
pub fn load_index(out_dir: &Path, arch: &str) -> Result<Index, String> {
    let idx = read_db(out_dir, arch)?;
    if idx.packages.is_empty() {
        return Err(format!(
            "缺少 ABI 数据库 {}（空）——请先 `farm seed` 播种，禁止无基线构建",
            db_path(out_dir, arch).display()
        ));
    }
    Ok(idx)
}

/// seed：把完整索引（含 needed_so）全量写入 ABI 数据库。
pub fn write_all(out_dir: &Path, arch: &str, index: &Index) -> Result<(), String> {
    let mut obj = serde_json::Map::new();
    let mut names: Vec<&String> = index.packages.keys().collect();
    names.sort();
    for name in names {
        let info = &index.packages[name];
        let mut e = serde_json::Map::new();
        e.insert("version".into(), serde_json::Value::String(info.version.clone()));
        e.insert(
            "provides".into(),
            serde_json::Value::Array(
                info.provides.iter().map(|s| serde_json::Value::String(s.clone())).collect(),
            ),
        );
        e.insert(
            "needed_so".into(),
            serde_json::Value::Array(
                info.needed_so.iter().map(|s| serde_json::Value::String(s.clone())).collect(),
            ),
        );
        obj.insert(name.clone(), serde_json::Value::Object(e));
    }
    let path = db_path(out_dir, arch);
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent).map_err(|e| format!("创建 {parent:?} 失败: {e}"))?;
    }
    std::fs::write(&path, serde_json::to_string_pretty(&serde_json::Value::Object(obj)).unwrap())
        .map_err(|e| format!("写 ABI 数据库 {path:?} 失败: {e}"))
}

/// repack 后单包更新（读-改-写）。
pub fn update_pkg(
    out_dir: &Path,
    arch: &str,
    pkg: &str,
    version: &str,
    provides: &[String],
    needed_so: &[String],
) -> Result<(), String> {
    let mut idx = read_db(out_dir, arch)?;
    idx.packages.insert(
        pkg.to_string(),
        PkgInfo {
            name: pkg.to_string(),
            version: version.to_string(),
            sha256: String::new(),
            deps: Vec::new(),
            provides: provides.to_vec(),
            needed_so: needed_so.to_vec(),
        },
    );
    write_all(out_dir, arch, &idx)
}
