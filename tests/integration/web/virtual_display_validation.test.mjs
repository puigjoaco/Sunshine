import test from 'node:test';
import assert from 'node:assert/strict';
import {invalidVirtualDisplaySetting} from '../../../src_assets/common/assets/web/configs/virtual-display-validation.js';
const config = {virtual_display_scale: 100, virtual_display_restore_delay: 5};
test('accepts integer limits and rejects out-of-range/fractional/blank values before save', () => {
  for (const scale of [100, 400, '150']) assert.equal(invalidVirtualDisplaySetting({...config,virtual_display_scale:scale},'linux'),null);
  for (const scale of [99,401,100.5,'',null,Infinity]) assert.equal(invalidVirtualDisplaySetting({...config,virtual_display_scale:scale},'linux'),'virtual_display_scale');
  for (const delay of [0,60,'5']) assert.equal(invalidVirtualDisplaySetting({...config,virtual_display_restore_delay:delay},'linux'),null);
  for (const delay of [-1,61,1.5,'',null]) assert.equal(invalidVirtualDisplaySetting({...config,virtual_display_restore_delay:delay},'linux'),'virtual_display_restore_delay');
  assert.equal(invalidVirtualDisplaySetting({},'windows'),null);
});
