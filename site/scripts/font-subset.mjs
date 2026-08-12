// font-subset.mjs — 构建后自动子集化字体
//
// 参考 wtada233.top/scripts/font-subset.ts（subset-font 方案）。
// 从 VitePress 构建产物 (docs/.vitepress/dist) 的 HTML 中提取实际用到的字符，
// 用 subset-font 对字体做子集化并覆写 dist 内的副本（public 源字体保持完整）。
import fs from "node:fs";
import path from "node:path";
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

  // 2. 逐字体子集化并覆写 dist 副本
  for (const font of FONTS) {
    const fontPath = path.join(DIST_DIR, font.src.replace(/^\//, ""));
    if (!fs.existsSync(fontPath)) {
      console.error(`[font-subset] ✘ 字体不存在: ${fontPath}`);
      continue;
    }
    try {
      const buf = fs.readFileSync(fontPath);
      const subset = await subsetFont(buf, allChars, { targetFormat: "truetype" });
      fs.writeFileSync(fontPath, subset);
      const oldKB = (buf.length / 1024).toFixed(1);
      const newKB = (subset.length / 1024).toFixed(1);
      const pct = ((1 - subset.length / buf.length) * 100).toFixed(1);
      console.log(`[font-subset] ✔ ${font.name}: ${oldKB} KB → ${newKB} KB (-${pct}%)`);
    } catch (err) {
      console.error(`[font-subset] ✘ ${font.name} 子集化失败:`, err);
    }
  }
}

main().catch((err) => {
  console.error("[font-subset] 致命错误:", err);
  process.exit(1);
});
