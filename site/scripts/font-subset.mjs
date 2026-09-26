// font-subset.mjs — 构建后自动子集化字体 + 内容哈希指纹
//
// 参考 wtada233.top/scripts/font-subset.ts（subset-font 方案）。
// 从 VitePress 构建产物 (docs/.vitepress/dist) 的 HTML 中提取实际用到的字符，
// 用 subset-font 对字体做子集化并覆写 dist 内的副本（public 源字体保持完整）。
//
// ⚠️ **为什么必须给字体文件名加内容哈希**（否则会踩一个很难定位的坑）：
// `docs/public/fonts/Unifont.ttf` 是 public 资源，Vite 原样拷贝、**不做指纹**，
// 于是 URL 永远是 `/fonts/Unifont.ttf`。而子集化的**内容每次构建都可能变**
// （新增字符 → 新字形）。浏览器按 URL 缓存字体，URL 不变就继续用旧副本 →
// 新加的字符在旧子集里没有字形 → 落到系统最后的兜底字体（LastResort 之类，
// 把每个码位画成带十六进制码的方框）→ 表现成"字符无法显示 / 显示成码位占位符"，
// 而服务端与源码其实都是对的。**已实际踩过一次**（首页图标换成 ↻/⊞ 后如此）。
// 解法：子集化后按内容哈希重命名（`Unifont.<hash>.ttf`）并改写 dist 内所有引用，
// 内容一变 URL 就变 → 浏览器必然重新下载。比加 `?v=` query 更稳（不受中间缓存
// 忽略 query 的影响）。
import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { fileURLToPath } from "node:url";
import subsetFont from "subset-font";

const SITE_DIR = path.dirname(path.dirname(fileURLToPath(import.meta.url)));
const DIST_DIR = path.join(SITE_DIR, "docs", ".vitepress", "dist");

// 需要子集化的字体（src 为相对 dist 根目录的构建产物路径）
const FONTS = [
  { name: "Unifont (BMP)", src: "/fonts/Unifont.ttf" },
  { name: "Unifont Upper (SMP)", src: "/fonts/Unifont_Upper.ttf" },
];

// 基础字符集：ASCII 可打印字符（U+0020..U+007E）+ 常用中英文标点。
// 保证 HTML 提取之外的字符（例如未来新增内容）也能正常渲染。
const BASE_CHARS =
  Array.from({ length: 95 }, (_, i) => String.fromCharCode(i + 32)).join("") +
  "，。、；：？！“”‘’（）《》〈〉【】〔〕…—·－";

/** 递归收集目录下指定扩展名的文件 */
function getFilesRecursive(dir, extensions) {
  const results = [];
  if (!fs.existsSync(dir)) return results;
  for (const entry of fs.readdirSync(dir, { withFileTypes: true })) {
    const full = path.join(dir, entry.name);
    if (entry.isDirectory()) {
      results.push(...getFilesRecursive(full, extensions));
    } else if (extensions.some((e) => entry.name.endsWith(e))) {
      results.push(full);
    }
  }
  return results;
}

/** 从 HTML 提取所有可见文本（去掉 script/style，解码实体） */
function extractText(html) {
  // 去掉 script/style 及其内容
  let text = html.replace(/<script[\s\S]*?<\/script>/gi, " ");
  text = text.replace(/<style[\s\S]*?<\/style>/gi, " ");
  // 补充常用属性里的文本（alt / title / placeholder）
  const attrs = [...html.matchAll(/\b(?:alt|title|placeholder)="([^"]*)"/gi)]
    .map((m) => m[1])
    .join(" ");
  // 剥离标签
  text = text.replace(/<[^>]+>/g, " ") + " " + attrs;
  // 解码 HTML 实体
  return text
    .replace(/&amp;/g, "&")
    .replace(/&lt;/g, "<")
    .replace(/&gt;/g, ">")
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&nbsp;/g, " ");
}

async function main() {
  const htmlFiles = getFilesRecursive(DIST_DIR, [".html"]);
  if (htmlFiles.length === 0) {
    console.warn(`[font-subset] 未找到构建产物: ${DIST_DIR}（请先运行 pnpm build）`);
    return;
  }

  // 1. 收集字符集
  const charSet = new Set(BASE_CHARS.split(""));
  for (const file of htmlFiles) {
    const text = extractText(fs.readFileSync(file, "utf-8"));
    for (const ch of text) {
      if (ch.trim() || ch === " " || ch === "\t" || ch === "\n") charSet.add(ch);
    }
  }
  const allChars = Array.from(charSet).sort().join("");
  console.log(`[font-subset] 从 ${htmlFiles.length} 个 HTML 提取 ${charSet.size} 个字符`);

  // 2. 逐字体子集化 → 按内容哈希改名（指纹）→ 记录待改写的引用映射
  //    renames: 旧路径 → 新路径，供第 3 步改写 dist 内所有引用
  const renames = new Map();
  for (const font of FONTS) {
    const fontPath = path.join(DIST_DIR, font.src.replace(/^\//, ""));
    if (!fs.existsSync(fontPath)) {
      console.error(`[font-subset] ✘ 字体不存在: ${fontPath}`);
      continue;
    }
    try {
      const buf = fs.readFileSync(fontPath);
      const subset = await subsetFont(buf, allChars, { targetFormat: "truetype" });

      // 内容哈希前 10 位做指纹；`.ttf` 前插入，保持扩展名可被静态服务器正确 Content-Type
      const hash = crypto.createHash("sha256").update(subset).digest("hex").slice(0, 10);
      const ext = path.extname(fontPath);
      const hashedName = `${path.basename(fontPath, ext)}.${hash}${ext}`;
      const hashedPath = path.join(path.dirname(fontPath), hashedName);

      fs.writeFileSync(hashedPath, subset);
      fs.rmSync(fontPath); // 删掉无指纹的那份：不留"旧名字仍可用"的退路
      renames.set(`/fonts/${path.basename(fontPath)}`, `/fonts/${hashedName}`);

      const oldKB = (buf.length / 1024).toFixed(1);
      const newKB = (subset.length / 1024).toFixed(1);
      const pct = ((1 - subset.length / buf.length) * 100).toFixed(1);
      console.log(
        `[font-subset] ✔ ${font.name}: ${oldKB} KB → ${newKB} KB (-${pct}%) → ${hashedName}`,
      );
    } catch (err) {
      console.error(`[font-subset] ✘ ${font.name} 子集化失败:`, err);
    }
  }

  // 3. 改写 dist 内所有对字体的引用为带指纹的名字。
  //    @font-face 的 src 在打包后的 CSS 里（VitePress 还会把关键 CSS 内联进 HTML），
  //    所以 .css 与 .html 都要扫；.js 一并扫是无害的兜底。
  if (renames.size === 0) return;
  let touched = 0;
  for (const file of getFilesRecursive(DIST_DIR, [".css", ".html", ".js"])) {
    let text = fs.readFileSync(file, "utf-8");
    const before = text;
    for (const [from, to] of renames) text = text.split(from).join(to);
    if (text !== before) {
      fs.writeFileSync(file, text);
      touched += 1;
    }
  }
  console.log(`[font-subset] 已改写 ${touched} 个文件中的字体引用`);
  for (const [from, to] of renames) console.log(`[font-subset]   ${from} → ${to}`);
}

main().catch((err) => {
  console.error("[font-subset] 致命错误:", err);
  process.exit(1);
});
