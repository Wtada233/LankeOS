/**
 * 构建时从 GitHub 获取 lpkg README，生成 docs/lpkg/index.md
 *
 * 用法：node scripts/fetch-lpkg-docs.mjs
 *
 * 环境变量：
 *   GITHUB_OWNER   — 默认为 Wtada233
 *   GITHUB_REPO    — 默认为 LankeOS
 */

const OWNER = process.env.GITHUB_OWNER || 'Wtada233'
const REPO = process.env.GITHUB_REPO || 'LankeOS'
const BRANCH = 'main'
const OUTPUT = 'docs/lpkg/index.md'

const headers = { 'User-Agent': 'lankeos-website-builder' }
if (process.env.GITHUB_TOKEN) {
  headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`
}

async function fetchRaw(path) {
  const url = `https://raw.githubusercontent.com/${OWNER}/${REPO}/${BRANCH}/${path}`
  const res = await fetch(url, { headers })
  if (!res.ok) throw new Error(`fetch ${path} failed: ${res.status}`)
  return res.text()
}

function githubMdToVitepress(md) {
  md = md.replace(/^\[.*?\]\(.*?\).*?\n/, '')

  md = md
    .replace(/^#### /gm, '##### ')
    .replace(/^### /gm, '#### ')
    .replace(/^## /gm, '### ')
    .replace(/^# /gm, '## ')

  md = md.replace(
    /\((\.[^)]*)\)/g,
    `(https://github.com/${OWNER}/${REPO}/blob/${BRANCH}/lpkg/$1)`
  )

  md = md.replace(
    /\]\(([^)]+\.png)\)/g,
    `](https://raw.githubusercontent.com/${OWNER}/${REPO}/${BRANCH}/lpkg/$1)`
  )

  return md
}

async function main() {
  console.log(`Fetching lpkg docs from ${OWNER}/${REPO}...`)

  const readme = await fetchRaw('lpkg/README.md')
  console.log(`  → README.md fetched (${readme.length} chars)`)

  let body = githubMdToVitepress(readme)

  const frontmatter = `---
title: LPKG
editLink: false
---

# lpkg 包管理器

> ⚡ 此页面由 \`scripts/fetch-lpkg-docs.mjs\` 在构建时自动生成，从 GitHub 仓库的 \`lpkg/README.md\` 拉取。
`

  const fs = await import('fs')
  fs.writeFileSync(OUTPUT, frontmatter + '\n' + body + '\n')
  console.log(`  → 已写入 ${OUTPUT} (${((frontmatter.length + body.length) / 1024).toFixed(1)} KB)`)
}

main().catch(err => {
  console.error('fetch-lpkg-docs.mjs 失败:', err.message)
  process.exit(1)
})
