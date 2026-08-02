//! serve.rs — 本地 repo 静态 HTTP 服务器（§12.5）。
//!
//! `farm build` 期间内嵌于 farm 进程、结束后关停；`farm serve --daemon` 可独立常驻。
//! 静态服务器**无状态**直接 serve 目录——新发布包写入后无需服务器感知（lpkg 会自动
//! 附加架构路径 `<repo>/<arch>/...`）。用 std TcpListener，不引入 axum/warp 依赖。

use std::fs;
use std::io::{Read, Write};
use std::net::{TcpListener, TcpStream};
use std::path::Path;
use std::thread;

/// 启动静态文件服务器（阻塞直到被关停）。
/// `bind`：独立 `farm serve` 用 `0.0.0.0`（局域网可访问）；build 内嵌用 `127.0.0.1`（容器经 host 网络访问）。
pub fn serve(bind: &str, root: &Path, port: u16) -> Result<(), String> {
    let root_abs = root
        .canonicalize()
        .map_err(|e| format!("root {root:?} 不可访问: {e}"))?;
    let listener = TcpListener::bind((bind, port))
        .map_err(|e| format!("绑定 {bind}:{port} 失败: {e}"))?;
    println!("[serve] 本地 repo 服务器 http://{bind}:{port}（root={root_abs:?}）");
    for stream in listener.incoming() {
        let Ok(s) = stream else { continue };
        let root = root_abs.clone();
        thread::spawn(move || handle_conn(s, &root));
    }
    Ok(())
}

fn handle_conn(mut stream: TcpStream, root: &Path) {
    let mut buf = [0u8; 8192];
    let n = match stream.read(&mut buf) {
        Ok(n) => n,
        Err(_) => return,
    };
    let req = String::from_utf8_lossy(&buf[..n]);
    let (status, body) = match parse_request_path(&req) {
        None => (Status::BadRequest, b"400 bad request".to_vec()),
        Some(p) if p == "/" => (Status::Ok, b"LankeOS local repo".to_vec()),
        Some(p) => serve_file(root, &p),
    };
    let _ = stream.write_all(&response(status, &body));
    let _ = stream.flush();
}

/// 解析文件路径，防路径穿越后 serve；不存在/越界 → 404。
fn serve_file(root: &Path, url_path: &str) -> (Status, Vec<u8>) {
    let rel = url_path.trim_start_matches('/');
    // 拒绝含 ".." 的段（防御；canonicalize 兜底）
    if rel.split('/').any(|seg| seg == "..") {
        return (Status::NotFound, b"404 not found".to_vec());
    }
    let file = root.join(rel);
    let canonical = match file.canonicalize() {
        Ok(c) => c,
        Err(_) => return (Status::NotFound, b"404 not found".to_vec()),
    };
    if !canonical.starts_with(root) {
        return (Status::NotFound, b"404 not found".to_vec());
    }
    match fs::read(&canonical) {
        Ok(bytes) => (Status::Ok, bytes),
        Err(_) => (Status::NotFound, b"404 not found".to_vec()),
    }
}

enum Status {
    Ok,
    NotFound,
    BadRequest,
}

impl Status {
    fn code(&self) -> u16 {
        match self {
            Status::Ok => 200,
            Status::NotFound => 404,
            Status::BadRequest => 400,
        }
    }
    fn reason(&self) -> &'static str {
        match self {
            Status::Ok => "OK",
            Status::NotFound => "Not Found",
            Status::BadRequest => "Bad Request",
        }
    }
}

fn response(status: Status, body: &[u8]) -> Vec<u8> {
    let head = format!(
        "HTTP/1.1 {} {}\r\nContent-Length: {}\r\nContent-Type: application/octet-stream\r\nConnection: close\r\n\r\n",
        status.code(),
        status.reason(),
        body.len()
    );
    let mut v = head.into_bytes();
    v.extend_from_slice(body);
    v
}

/// 取请求行：`GET <path> HTTP/1.1`。非 GET/HEAD 返回 None。
fn parse_request_path(req: &str) -> Option<String> {
    let line = req.lines().next()?;
    let mut parts = line.split_whitespace();
    let method = parts.next()?;
    if method != "GET" && method != "HEAD" {
        return None;
    }
    parts.next().map(|p| p.to_string())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_get_path() {
        assert_eq!(
            parse_request_path("GET /x86_64/index.txt HTTP/1.1").as_deref(),
            Some("/x86_64/index.txt")
        );
        assert_eq!(
            parse_request_path("POST / HTTP/1.1"),
            None,
            "非 GET/HEAD 拒绝"
        );
    }

    #[test]
    fn path_traversal_blocked() {
        let root = std::env::temp_dir().join("farm-serve-root");
        fs::create_dir_all(&root).unwrap();
        fs::write(root.join("ok.txt"), b"hello").unwrap();
        let (s1, b1) = serve_file(&root, "/ok.txt");
        assert_eq!(s1.code(), 200);
        assert_eq!(b1, b"hello");
        // ".." 段直接拒绝
        let (s2, _) = serve_file(&root, "/../../etc/passwd");
        assert_eq!(s2.code(), 404);
        // 不存在 → 404
        let (s3, _) = serve_file(&root, "/nope.txt");
        assert_eq!(s3.code(), 404);
        fs::remove_dir_all(&root).ok();
    }
}
