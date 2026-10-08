// Execute the real converter. Only the expose-building library is mocked;
// no Zigbee/MQTT traffic leaves this test. Run with --experimental-vm-modules.
import {readFile} from 'node:fs/promises';
import vm from 'node:vm';
import assert from 'node:assert/strict';
const fluent = () => new Proxy({}, {get: () => () => fluent()});
const exposes = {presets: {binary: fluent, enum: fluent, numeric: fluent}, access: {SET: 2, STATE: 1}};
const dep = new vm.SyntheticModule(['presets', 'access'], function () {
    this.setExport('presets', exposes.presets); this.setExport('access', exposes.access);
});
const mod = new vm.SourceTextModule(await readFile(new URL('../zigbee2mqtt/outlander_phev_c6.mjs', import.meta.url), 'utf8'));
await mod.link(name => {assert.equal(name, 'zigbee-herdsman-converters/lib/exposes'); return dep;});
await mod.evaluate();
const d = mod.namespace.default;
const writes=[];
const device={getEndpoint: id => ({write: async (cluster,data) => writes.push({id,cluster,data}), bind: async cluster=>writes.push({id,cluster}), configureReporting: async ()=>{assert.fail('C6 reports are sent manually; automatic report table must not be allocated');}})};
const set=async (key,value,message={})=>d.toZigbee[0].convertSet(null,key,value,{device,message});
const msg=(id,value)=>({endpoint:{ID:id},data:value===undefined?{}:{presentValue:value}});
assert.deepEqual(d.fromZigbee[0].convert(null,msg(6,1)),{phev_online:true});
assert.deepEqual(d.fromZigbee[0].convert(null,msg(6,2)),{});
assert.deepEqual(d.fromZigbee[0].convert(null,msg(7,0),null,null,{state:{phev_online:false,telemetry_valid_mask:8}}),{charging:false});
assert.deepEqual(d.fromZigbee[1].convert(null,msg(22,0),null,null,{state:{telemetry_valid_mask:0}}),{});
assert.deepEqual(d.fromZigbee[0].convert(null,msg(6,0)),{phev_online:false});
assert.equal(d.fromZigbee[1].convert(null,msg(27,0)).battery,null);
assert.equal(d.fromZigbee[1].convert(null,msg(27,0)).climate_on,null);
assert.equal(d.fromZigbee[1].convert(null,msg(27,1)).battery_valid,true);
assert.equal(d.fromZigbee[1].convert(null,msg(27,1)).doors_locked,null);
assert.deepEqual(d.fromZigbee[1].convert(null,msg(27,2048)),{});
const sessionNames=['never','refreshing','climate_pending','refresh_completed','command_acknowledged','timeout','transport_failed','command_failed'];
for (let status=0;status<8;status++) {
    const word=status === 0 ? 0 : 8+status;
    assert.deepEqual(d.fromZigbee[1].convert(null,msg(28,word)), {
        session_word:word,session_id:status === 0 ? 0 : 1,
        session_status:sessionNames[status],session_active:status===1 || status===2,
    });
}
assert.deepEqual(d.fromZigbee[1].convert(null,msg(28,0xffffff)), {
    session_word:0xffffff,session_id:0x1fffff,session_status:'command_failed',session_active:false,
});
for(const invalid of [-1,1,2,3,4,5,6,7,8,16,24,1.5,0x1000000,NaN,Infinity,undefined])
    assert.deepEqual(d.fromZigbee[1].convert(null,msg(28,invalid)),{});
assert.equal(d.fromZigbee[0].convert(null,msg(18,0)).battery_valid,false);
assert.deepEqual(d.fromZigbee[1].convert(null,msg(4,94)),{battery:94});
assert.deepEqual(d.fromZigbee[1].convert(null,msg(4,NaN)),{});
assert.deepEqual(d.fromZigbee[1].convert(null,msg(4,101)),{});
assert.deepEqual(d.fromZigbee[1].convert(null,msg(4,94),null,null,{state:{battery_valid:false}}),{});
assert.equal(d.fromZigbee[1].convert(null,msg(9,33)).driver_door_open,true);
assert.equal(d.fromZigbee[1].convert(null,msg(9,33)).bonnet_open,true);
assert.equal(d.fromZigbee[1].convert(null,msg(9,33)).boot_open,false);
assert.deepEqual(d.fromZigbee[2].convert(null,msg(2,2)),{climate_mode:'heat'});
assert.deepEqual(d.fromZigbee[2].convert(null,msg(3,3)),{climate_duration:'30'});
assert.deepEqual(d.fromZigbee[2].convert(null,msg(2,undefined)),{});
assert.deepEqual(d.fromZigbee[2].convert(null,msg(3,10)),{});
await set('climate_command',true,{climate_command:true,mode_command:'heat',duration_command:'10'});
assert.deepEqual(writes.map(w=>[w.id,w.data.presentValue]),[[2,2],[3,1],[1,true]]);
assert.deepEqual(await set('climate_command',false),{}); // no optimistic state
assert.equal(writes.at(-1).data.presentValue,false);
for (const key of ['mode_command', 'duration_command', 'climate_command']) {
    for (const on of [true, false]) {
        const grouped={mode_command:'heat',duration_command:'10',climate_command:on};
        const start=writes.length;
        await set(key,grouped[key],grouped);
        assert.deepEqual(writes.slice(start).map(w=>[w.id,w.data.presentValue]),[[2,2],[3,1],[1,on]]);
    }
}
for(const [k,v,m] of [['climate_command','ON',{}],['mode_command','unknown',{}],['duration_command',5,{}],['climate_command',true,{duration_command:'5'}]]) {
    const n=writes.length; await assert.rejects(set(k,v,m)); assert.equal(writes.length,n);
}
const gateway=async (key,value,message={})=>d.toZigbee[1].convertSet(null,key,value,{device,message});
for (const [value,index] of [['normal',1],['ap',2],['wifi-seb',3]]) {
    assert.deepEqual(await gateway('maintenance_mode',value),{});
    assert.equal(writes.at(-1).id,16); assert.equal(writes.at(-1).data.presentValue,index);
}
await gateway('refresh','refresh'); assert.equal(writes.at(-1).data.presentValue,4);
await assert.rejects(gateway('maintenance_mode','invalid'));
await assert.rejects(gateway('maintenance_mode','ap',{climate_command:true}));
await assert.rejects(set('climate_command',true,{maintenance_mode:'ap'}));
writes.length=0; await d.configure(device,{}); assert.equal(writes.length,28);
assert.deepEqual(writes.map(w=>w.id).sort((a,b)=>a-b),Array.from({length:28},(_,i)=>i+1));
for (const template of ['{{ value_json.battery }}','{{ value_json["battery"] }}',"{{ value_json['battery'] }}"]) {
    const discovery={value_template:template,state_topic:'zigbee2mqtt/phev',availability:[{topic:'zigbee2mqtt/bridge/state'}]};
    d.meta.overrideHaDiscoveryPayload(discovery);
    assert.equal(discovery.availability.length,2); assert.equal(discovery.availability_mode,'all');
    assert.match(discovery.availability[1].value_template,/battery_valid/);
    assert.equal(discovery.availability[0].topic,'zigbee2mqtt/bridge/state');
}
for (const [field,divisor] of [['battery_age',1],['parking_lights',1],['climate_on',2],['climate_mode',4],
    ['charging',8],['plug_connected',16],['charge_remaining',32],['doors_locked',64],['boot_open',64],
    ['hazard_lights',128],['ac_operating',256],['battery_warning_code',512],['registered_devices',1024]]) {
    const discovery={value_template:`{{ value_json["${field}"] }}`,state_topic:'zigbee2mqtt/phev'};
    d.meta.overrideHaDiscoveryPayload(discovery);
    assert.equal(discovery.availability.length,1); assert.equal(discovery.availability_mode,'all');
    assert.match(discovery.availability[0].value_template,/telemetry_valid_mask/);
    assert.ok(discovery.availability[0].value_template.includes(`// ${divisor}) % 2 == 1`));
}
for (const field of ['phev_online','command_ack','session_word','session_id','session_status','climate_command']) {
    const discovery={value_template:`{{ value_json["${field}"] }}`,state_topic:'zigbee2mqtt/phev'};
    d.meta.overrideHaDiscoveryPayload(discovery);
    assert.equal(discovery.availability,undefined); // controls never gated on cached vehicle availability
    if (['session_word','session_id'].includes(field)) assert.equal(discovery.entity_category,'diagnostic');
}
console.log('PASS converter: exact atomic session word/status, invalid values, real bracket/dot HA discovery, group validity, ordered commands, 28 manual bindings');
