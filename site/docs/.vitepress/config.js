import { defineConfig } from 'vitepress'

export default defineConfig({
  title: 'LankeOS',
  description: 'A Linux From Scratch distribution built from the ground up',
  lang: 'zh-CN',

  head: [
    ['link', { rel: 'icon', href: '/favicon.svg', type: 'image/svg+xml' }],
    ['meta', { name: 'theme-color', content: '#2d8cf0' }],
  ],

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
      pattern: 'https://github.com/Wtada233/LankeOS/edit/main/docs/:path',
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
