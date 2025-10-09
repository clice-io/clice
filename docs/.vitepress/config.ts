import { defineConfig } from 'vitepress'
import { genSidebar } from './theme/sidebar'

// https://vitepress.dev/reference/site-config
export default defineConfig({
  title: "clice",
  description: "a powerful and modern C++ language server",
  cleanUrls: true,
  base: '/',
  rewrites: {
    'en/:rest*': ':rest*',
  },
  locales: {
    root: { label: 'English' },
    zh: { label: '简体中文' },
  },
  themeConfig: {
    // https://vitepress.dev/reference/default-theme-config
    nav: [
      { text: 'Home', link: '/' },
    ],
    sidebar: {
      "/zh/": [
        genSidebar('zh', 'design', { title: 'Design' }),
        genSidebar('zh', 'dev', { title: 'Development' }),
        genSidebar('zh', 'guide', { title: 'Guide' }),
      ],
      "/": [
        genSidebar('en', 'design', { title: 'Design' }),
        genSidebar('en', 'dev', { title: 'Development' }),
        genSidebar('en', 'guide', { title: 'Guide' }),
      ],
    },
    socialLinks: [
      { icon: 'discord', link: 'https://discord.gg/PA3UxW2VA3' },
      { icon: 'github', link: 'https://github.com/clice-io/clice' },
    ],
    outline: 'deep',
  },
})
