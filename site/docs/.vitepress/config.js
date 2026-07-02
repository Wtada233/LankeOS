import { defineConfig } from 'vitepress'

// ─── SEO 基础配置 ─────────────────────────────────────────────
const siteURL = 'https://lankeos.wtada233.top'
const siteTitle = 'LankeOS'
const siteDescription = 'A Linux From Scratch distribution built from the ground up — 从零构建的 Linux 发行版'
const siteImage = `${siteURL}/assets/preview.webp`

export default defineConfig({
  /* ─── 站点元信息 ───────────────────────────────────── */
  title: siteTitle,
  description: siteDescription,
  lang: 'zh-CN',
  cleanUrls: true,

  /* ─── Sitemap 自动生成 ─────────────────────────────── */
  sitemap: {
    hostname: siteURL,
    lastUpdated: true,
  },

  /* ─── <head> 全局标签 ─────────────────────────────── */
  head: [
    // ---------- 基础 ----------
    ['link', { rel: 'icon', href: '/favicon.svg', type: 'image/svg+xml' }],
    ['meta', { name: 'theme-color', content: '#2d8cf0' }],
    ['meta', {
      name: 'keywords',
      content: 'LankeOS, Linux, LFS, Linux From Scratch, 发行版, 操作系统, lpkg, Sway, Wayland, C++20, 包管理器',
    }],
    ['meta', { name: 'author', content: 'Wtada233' }],

    // ---------- 无障碍 ----------
    ['script', {}, `document.addEventListener('DOMContentLoaded',()=>{document.querySelector('.VPHome')?.setAttribute('role','main')})`],
    ['style', {}, `.VPButton.brand{background-color:#1557a0!important} .dark .VPButton.brand{background-color:#1a5a9e!important}`],
    ['meta', { name: 'robots', content: 'index, follow' }],

    // ---------- Open Graph ----------
    ['meta', { property: 'og:type', content: 'website' }],
    ['meta', { property: 'og:site_name', content: siteTitle }],
    ['meta', { property: 'og:locale', content: 'zh_CN' }],
    ['meta', { property: 'og:image', content: siteImage }],
    ['meta', { property: 'og:image:width', content: '1200' }],
    ['meta', { property: 'og:image:height', content: '630' }],

    // ---------- Twitter Card ----------
    ['meta', { name: 'twitter:card', content: 'summary_large_image' }],
    ['meta', { name: 'twitter:image', content: siteImage }],
  ],

  /* ─── 逐页动态 meta ───────────────────────────────── */
  transformHead({ pageData }) {
    // 生成当前页的 clean URL 路径
    const pagePath = pageData.relativePath
      .replace(/\/?index\.md$/, '')   // index.md → ''，保留其他 .md
      .replace(/\.md$/, '')
    const pageUrl = pagePath ? `${siteURL}/${pagePath}` : siteURL

    // 逐页标题 / 描述（fallback 到站点默认）
    const pageTitle = pageData.title
      ? `${pageData.title} | ${siteTitle}`
      : siteTitle
    const pageDesc = pageData.description || siteDescription
    const pageImage = pageData.frontmatter?.image || siteImage

    return [
      // ---------- 页面级 OG / Twitter ----------
      ['meta', { property: 'og:title', content: pageTitle }],
      ['meta', { property: 'og:description', content: pageDesc }],
      ['meta', { property: 'og:url', content: pageUrl }],
      ['meta', { property: 'og:image', content: pageImage }],
      ['meta', { name: 'twitter:title', content: pageTitle }],
      ['meta', { name: 'twitter:description', content: pageDesc }],
      ['meta', { name: 'twitter:image', content: pageImage }],

      // ---------- Canonical ----------
      ['link', { rel: 'canonical', href: pageUrl }],

      // ---------- JSON-LD 结构化数据 ----------
      [
        'script',
        { type: 'application/ld+json' },
        JSON.stringify({
          '@context': 'https://schema.org',
          '@type': pagePath === '' ? 'WebSite' : 'WebPage',
          name: pageTitle,
          url: pageUrl,
          description: pageDesc,
          inLanguage: 'zh-CN',
          publisher: {
            '@type': 'Person',
            name: 'Wtada233',
            url: 'https://github.com/Wtada233',
          },
          ...(pagePath !== ''
            ? { isPartOf: { '@id': siteURL } }
            : { image: siteImage }),
        }),
      ],
    ]
  },

  /* ─── VitePress 1.6+ 内置 sitemap 配置，上面已声明 ───── */
  /* ─── 主题配置 ──────────────────────────────────────── */
  ignoreDeadLinks: true,

  themeConfig: {
    logo: '/logo.svg',

    nav: [
      { text: '首页', link: '/' },
      { text: '下载', link: '/download' },
      { text: '发布历史', link: '/releases' },
      {
        text: '文档',
        items: [
          { text: '快速开始', link: '/guide/' },
          { text: '安装指南', link: '/guide/install' },
          { text: '从源码构建', link: '/guide/build' },
        ],
      },
      {
        text: 'lpkg 包管理器',
        items: [
          { text: '概述', link: '/lpkg/' },
        ],
      },
      { text: '截图', link: '/screenshots' },
    ],

    sidebar: {
      '/guide/': [
        {
          text: '指南',
          items: [
            { text: '快速开始', link: '/guide/' },
            { text: '安装指南', link: '/guide/install' },
            { text: '从源码构建', link: '/guide/build' },
          ],
        },
      ],
      '/lpkg/': [
        {
          text: 'lpkg 包管理器',
          items: [
            { text: '概述', link: '/lpkg/' },
          ],
        },
      ],
    },

    socialLinks: [
      { icon: 'github', link: 'https://github.com/Wtada233/LankeOS' },
    ],

    footer: {
      message: '基于 GPL-3.0 协议开源',
      copyright: 'Copyright © 2026 Wtada233',
    },

    editLink: {
      pattern: 'https://github.com/Wtada233/LankeOS/edit/main/site/docs/:path',
      text: '在 GitHub 上编辑此页',
    },

    lastUpdated: {
      text: '最后更新',
    },

    search: {
      provider: 'local',
    },
  },

  markdown: {
    lineNumbers: true,
  },
})
