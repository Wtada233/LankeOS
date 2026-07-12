<template>
  <div class="distrowatch-bar">
    <a
      href="https://distrowatch.com/dwres.php?waitingdistro=1104&amp;resource=links"
      target="_blank"
      rel="noopener noreferrer"
      class="distrowatch-link"
    >
      <span class="dw-star">&#9733;</span>
      <span class="dw-text">{{ text }}</span>
      <span class="dw-arrow">&rarr;</span>
    </a>
  </div>
</template>

<script setup>
import { computed } from 'vue'
import { useData } from 'vitepress'

const { lang } = useData()

const text = computed(() =>
  lang.value === 'en'
    ? 'Click to vote for LankeOS on DistroWatch!'
    : '点击链接为 LankeOS 加入 DistroWatch 投一票！'
)
</script>

<style>
/* 为固定底栏留出空间 */
body {
  padding-bottom: 41px !important;
}
</style>

<style scoped>
.distrowatch-bar {
  position: fixed;
  bottom: 0;
  z-index: 55;
  width: 100%;
  text-align: center;
  padding: 10px 16px;
  background: linear-gradient(135deg, var(--vp-c-brand-3) 0%, color-mix(in srgb, var(--vp-c-brand-2) 80%, #000) 100%);
}

.distrowatch-link {
  display: inline-flex;
  align-items: center;
  gap: 8px;
  color: #fff;
  font-size: 14px;
  font-weight: 500;
  text-decoration: none;
  transition: opacity 0.2s ease;
}

.distrowatch-link:hover {
  opacity: 0.85;
  color: #fff;
}

.dw-star {
  font-size: 16px;
  animation: dw-pulse 2s ease-in-out infinite;
}

.dw-arrow {
  font-size: 16px;
  transition: transform 0.2s ease;
}

.distrowatch-link:hover .dw-arrow {
  transform: translateX(4px);
}

@keyframes dw-pulse {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.5; }
}
</style>
