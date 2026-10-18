import fs from 'node:fs';
import assert from 'node:assert/strict';
import {runGuest} from '../../../local/pxa-apps/openlara/tools/guest-host.mjs';
import {checkpoint,telemetry,assertRestored} from '../../../local/pxa-apps/openlara/tools/classic-save-probe.mjs';
const [baseline,candidate,assets,output]=process.argv.slice(2),files=new Map();
const save='data/classic-save.bin',manifest=JSON.parse(fs.readFileSync(assets+'/game/data-manifest.json')),
    lara=manifest.levels.find(level=>level.id===3).lara_entities[0];
const a=await runGuest(baseline,assets,files);a.menu(4);a.buttons(1);a.tick(35);a.buttons(0);a.tick(30);a.buttons(8);a.tick(29);a.buttons(0);a.tick(30);a.menu(1);
assert(a.logs.includes('OpenLara save committed'));const initial=checkpoint(files.get(save),lara);
assert.notEqual(initial.rotation,lara.rotation,'initial rotation did not change');assert.notDeepEqual(initial.position,lara.position,'Lara did not move before saving');a.stop();
const b=await runGuest(candidate,assets,files);b.menu(2);b.tick(180);
assert(b.logs.includes('OpenLara save restored'));assertRestored(telemetry(b.logs),initial);b.menu(1);
const updated=checkpoint(files.get(save),lara);assert.deepEqual(updated.inventory,initial.inventory);b.stop();
const c=await runGuest(baseline,assets,files);c.menu(2);c.tick(180);
assert(c.logs.includes('OpenLara save restored'));assertRestored(telemetry(c.logs),updated);c.menu(1);c.stop();
const result={lara_entity_index:lara.index,bidirectional_restore:true,inventory_identical:true,
    saved_position:initial.position,saved_rotation:initial.rotation,roundtrip_rotation:updated.rotation,
    yaw_tolerance_per_load_units:1};
fs.writeFileSync(output,JSON.stringify(result,null,2)+'\n');console.log(result);
