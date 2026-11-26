/** Return the invalid Linux-only setting, or null before submitting configuration. */
export function invalidVirtualDisplaySetting(config, platform) {
  if (platform !== 'linux') return null;
  for (const [key, min, max] of [['virtual_display_scale', 100, 400], ['virtual_display_restore_delay', 0, 60]]) {
    const raw = config[key];
    const value = Number(raw);
    if (!['number', 'string'].includes(typeof raw) || String(raw).trim() === '' || !Number.isInteger(value) || value < min || value > max) return key;
  }
  return null;
}
