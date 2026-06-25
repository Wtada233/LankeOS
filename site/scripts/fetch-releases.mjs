/**
 * 构建时从 GitHub API 获取 Release Notes，生成 docs/releases.md
 *
 * 用法：node scripts/fetch-releases.mjs
 *
 * 环境变量：
 *   GITHUB_TOKEN   — 提高 API 速率限制（可选）
 *   GITHUB_OWNER   — 默认为 Wtada233
 *   GITHUB_REPO    — 默认为 LankeOS
 */

const OWNER = process.env.GITHUB_OWNER || 'Wtada233'
const REPO = process.env.GITHUB_REPO || 'LankeOS'
const OUTPUT = 'docs/releases.md'

const headers = {
  Accept: 'application/vnd.github+json',
  'User-Agent': 'lankeos-website-builder',
}
if (process.env.GITHUB_TOKEN) {
  headers.Authorization = `Bearer ${process.env.GITHUB_TOKEN}`
}

async function fetchReleases() {
  const perPage = 30
  const url = `https://api.github.com/repos/${OWNER}/${REPO}/releases?per_page=${perPage}`
  const res = await fetch(url, { headers })

  if (!res.ok) {
    throw new Error(`GitHub API ${res.status}: ${res.statusText}`)
  }

  const releases = await res.json()
  return releases.map(r => ({
    tag: r.tag_name,
    name: r.name || r.tag_name,
    body: r.body || '',
    published: r.published_at,
    prerelease: r.prerelease,
    htmlUrl: r.html_url,
  }))
}

function releaseToMarkdown(release) {
  const date = release.published
    ? new Date(release.published).toLocaleDateString('zh-CN')
    : '—'

  const tag = release.prerelease
    ? `${release.name} ⚠️ 预发布`
    : release.name

  const body = release.body
    .replace(/^### /gm, '#### ')
    .replace(/^## /gm, '### ')
    .replace(/^# /gm, '## ')
    .replace(/@(\w+)/g, '[$1](https://github.com/$1)')

  return `
## ${tag}

**${release.name}** · ${date} · [在 GitHub 上查看](${release.htmlUrl})

${body}
---`
}

async function main() {
  console.log(`Fetching releases from ${OWNER}/${REPO}...`)
  const releases = await fetchReleases()
  console.log(`  → ${releases.length} releases found`)

  const header = `---
title: 发布历史
editLink: false
---

# 发布历史

> ⚡ 此页面由 \`scripts/fetch-releases.mjs\` 在构建时自动生成，从 GitHub Releases API 拉取。

${releases.map(releaseToMarkdown).join('\n')}

---

*最后更新：${new Date().toISOString()}*
`

  const fs = await import('fs')
  fs.writeFileSync(OUTPUT, header.trimStart() + '\n')
  console.log(`  → 已写入 ${OUTPUT} (${(Buffer.byteLength(header) / 1024).toFixed(1)} KB)`)
}

main().catch(err => {
  console.error('fetch-releases.mjs 失败:', err.message)
  process.exit(1)
})
