import { defineConfig } from 'vitepress'

// ─── SEO 基础配置 ─────────────────────────────────────────────
const siteURL = 'https://lankeos.wtada233.top'
const siteTitle = 'LankeOS'
const siteImage = `${siteURL}/assets/preview.webp`

export default defineConfig({
  /* ─── 站点元信息 ───────────────────────────────────── */
  title: siteTitle,
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

    // ---------- 全局 Open Graph ----------
    ['meta', { property: 'og:type', content: 'website' }],
    ['meta', { property: 'og:site_name', content: siteTitle }],
    ['meta', { property: 'og:image', content: siteImage }],
    ['meta', { property: 'og:image:width', content: '1200' }],
    ['meta', { property: 'og:image:height', content: '630' }],

    // ---------- 全局 Twitter Card ----------
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

    // 判断当前语言
    const isEn = pageData.lang === 'en'

    // 逐页标题 / 描述（fallback 到站点默认）
    const pageTitle = pageData.title
      ? `${pageData.title} | ${siteTitle}`
      : siteTitle
    const pageDesc = pageData.description || (isEn
      ? 'A Linux from Scratch distribution built from the ground up'
      : 'A Linux From Scratch distribution built from the ground up — 从零构建的 Linux 发行版')
    const pageImage = pageData.frontmatter?.image || siteImage

    return [
      // ---------- 页面级 OG locale ----------
      ['meta', { property: 'og:locale', content: isEn ? 'en_US' : 'zh_CN' }],

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
          '@type': !pagePath || pagePath === 'en' ? 'WebSite' : 'WebPage',
          name: pageTitle,
          url: pageUrl,
          description: pageDesc,
          inLanguage: isEn ? 'en-US' : 'zh-CN',
          publisher: {
            '@type': 'Person',
            name: 'Wtada233',
            url: 'https://github.com/Wtada233',
          },
          ...(!pagePath || pagePath === 'en'
            ? { image: pageImage }
            : { isPartOf: { '@id': siteURL } }),
        }),
      ],
    ]
  },

  /* ─── 国际化 / 多语言 ──────────────────────────────── */
  locales: {
    // 简体中文（默认）
    root: {
      label: '简体中文',
      lang: 'zh-CN',
      description: 'A Linux From Scratch distribution built from the ground up — 从零构建的 Linux 发行版',
      themeConfig: {
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
        footer: {
          message: '基于 GPL-3.0 协议开源',
          copyright: 'Copyright © 2026 Wtada233',
        },
      },
    },

    // English
    en: {
      label: 'English',
      lang: 'en',
      description: 'A Linux from Scratch distribution built from the ground up',
      themeConfig: {
        nav: [
          { text: 'Home', link: '/en/' },
          { text: 'Download', link: '/en/download' },
          { text: 'Releases', link: '/en/releases' },
          {
            text: 'Docs',
            items: [
              { text: 'Quick Start', link: '/en/guide/' },
              { text: 'Installation Guide', link: '/en/guide/install' },
              { text: 'Build from Source', link: '/en/guide/build' },
            ],
          },
          {
            text: 'lpkg Package Manager',
            items: [
              { text: 'Overview', link: '/en/lpkg/' },
            ],
          },
          { text: 'Screenshots', link: '/en/screenshots' },
        ],
        sidebar: {
          '/en/guide/': [
            {
              text: 'Guide',
              items: [
                { text: 'Quick Start', link: '/en/guide/' },
                { text: 'Installation Guide', link: '/en/guide/install' },
                { text: 'Build from Source', link: '/en/guide/build' },
              ],
            },
          ],
          '/en/lpkg/': [
            {
              text: 'lpkg Package Manager',
              items: [
                { text: 'Overview', link: '/en/lpkg/' },
              ],
            },
          ],
        },
        editLink: {
          pattern: 'https://github.com/Wtada233/LankeOS/edit/main/site/docs/:path',
          text: 'Edit this page on GitHub',
        },
        lastUpdated: {
          text: 'Last updated',
        },
        search: {
          provider: 'local',
        },
        footer: {
          message: 'Open source under GPL-3.0',
          copyright: 'Copyright © 2026 Wtada233',
        },
      },
    },
  },

  /* ─── 共享主题配置 ──────────────────────────────────── */
  themeConfig: {
    logo: '/logo.svg',
    socialLinks: [
      { icon: 'github', link: 'https://github.com/Wtada233/LankeOS' },
    ],
  },

  markdown: {
    lineNumbers: true,
  },
})
