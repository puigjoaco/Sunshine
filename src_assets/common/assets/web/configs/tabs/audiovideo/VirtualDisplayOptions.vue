<script setup>
import {ref} from 'vue'
import PlatformLayout from '../../../PlatformLayout.vue'
import Checkbox from '../../../Checkbox.vue'

const props = defineProps(['platform', 'config'])
const config = ref(props.config)
</script>

<template>
  <PlatformLayout :platform="platform">
    <template #linux>
      <Checkbox class="mb-3"
                id="virtual_display"
                locale-prefix="config"
                v-model="config.virtual_display"
                default="false"
      ></Checkbox>

      <div class="mb-3">
        <label for="virtual_display_mode" class="form-label">{{ $t('config.virtual_display_mode') }}</label>
        <select id="virtual_display_mode" class="form-select" v-model="config.virtual_display_mode">
          <option value="extend">{{ $t('config.virtual_display_mode_extend') }}</option>
          <option value="exclusive">{{ $t('config.virtual_display_mode_exclusive') }}</option>
        </select>
        <div class="form-text">{{ $t('config.virtual_display_mode_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="virtual_display_restore_delay" class="form-label">{{ $t('config.virtual_display_restore_delay') }}</label>
        <input id="virtual_display_restore_delay" class="form-control" type="number" min="0" max="60" step="1"
               v-model.number="config.virtual_display_restore_delay" />
        <div class="form-text">{{ $t('config.virtual_display_restore_delay_desc') }}</div>
      </div>

      <div class="mb-3">
        <label for="virtual_display_scale" class="form-label">{{ $t('config.virtual_display_scale') }}</label>
        <input id="virtual_display_scale" class="form-control" type="number" min="100" max="400" step="1"
               v-model.number="config.virtual_display_scale" />
        <div class="form-text">{{ $t('config.virtual_display_scale_desc') }}</div>
      </div>
    </template>
  </PlatformLayout>
</template>
