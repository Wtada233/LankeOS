<script setup>
import DefaultTheme from 'vitepress/theme'
import DistroWatchBar from './components/DistroWatchBar.vue'

const { Layout } = DefaultTheme
</script>

<template>
  <Layout>
    <template #layout-bottom>
      <DistroWatchBar />
    </template>
  </Layout>
</template>
