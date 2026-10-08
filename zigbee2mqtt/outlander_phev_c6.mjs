// Copier dans data/external_converters de Zigbee2MQTT.
// Firmware associe : PHEV-Remote-C6, 0.1.0-beta.1, 28 endpoints.
// Ne jamais publier une commande MQTT avec retain=true.
import * as exposes from 'zigbee-herdsman-converters/lib/exposes';

const e = exposes.presets;
const ea = exposes.access;

const binaryNames = {
    1: 'climate_on',
    6: 'phev_online',
    7: 'charging',
    8: 'plug_connected',
    10: 'doors_locked',
    11: 'headlights',
    12: 'parking_lights',
    13: 'command_ack',
    14: 'command_failed',
    15: 'wifi_connected',
    18: 'battery_valid',
    21: 'climate_terminated',
    23: 'ac_operating',
    24: 'hazard_lights',
    25: 'interior_lights',
};
const analogNames = {4: 'battery', 5: 'charge_remaining', 9: 'open_doors_mask',
    17: 'wifi_rssi', 19: 'battery_age', 20: 'uptime', 22: 'battery_warning_code', 26: 'registered_devices', 27: 'telemetry_valid_mask', 28: 'session_word'};
const modes = ['unknown', 'cool', 'heat', 'windscreen'];
const durations = ['unknown', '10', '20', '30'];
const gatewayModes = ['unknown', 'normal', 'ap', 'wifi-seb'];
const sessionStatuses = ['never', 'refreshing', 'climate_pending', 'refresh_completed',
    'command_acknowledged', 'timeout', 'transport_failed', 'command_failed'];
const doorNames = ['driver_door_open', 'passenger_door_open', 'rear_right_door_open',
    'rear_left_door_open', 'boot_open', 'bonnet_open'];
const unavailableVehicle = Object.fromEntries([
    'battery', 'battery_age', 'climate_on', 'climate_mode', 'climate_duration', 'climate_terminated',
    'charging', 'plug_connected', 'charge_remaining', 'doors_locked', 'open_doors_mask',
    'headlights', 'parking_lights', 'hazard_lights', 'interior_lights', 'ac_operating',
    'battery_warning_code', 'registered_devices', ...doorNames,
].map(name => [name, null]));
const groups = [
    ['battery', 'battery_age', 'parking_lights'], ['climate_on', 'climate_terminated'],
    ['climate_mode', 'climate_duration'], ['charging'], ['plug_connected'], ['charge_remaining'],
    ['doors_locked', 'open_doors_mask', 'headlights', ...doorNames], ['hazard_lights', 'interior_lights'],
    ['ac_operating'], ['battery_warning_code'], ['registered_devices'],
];
const fieldKnown = (name, meta) => {
    const group = groups.findIndex(fields => fields.includes(name));
    const mask = meta?.state?.telemetry_valid_mask;
    return group < 0 || !Number.isInteger(mask) || !!(mask & (1 << group));
};

const readBinary = {
    cluster: 'genBinaryInput', type: ['attributeReport', 'readResponse'],
    convert: (_model, msg, _publish, _options, meta) => {
        const name = binaryNames[msg.endpoint.ID];
        const value = msg.data.presentValue;
        if (!name || ![0, 1, false, true].includes(value)) return {};
        if (name === 'phev_online') return {phev_online: !!value}; // TCP closed intentionally; do not erase cached measurements
        if (name === 'battery_valid' && !value) return {battery_valid: false, battery: null, battery_age: null};
        if (!fieldKnown(name, meta)) return {};
        return {[name]: !!value};
    },
};
const readAnalog = {
    cluster: 'genAnalogInput', type: ['attributeReport', 'readResponse'],
    convert: (_model, msg, _publish, _options, meta) => {
        const name = analogNames[msg.endpoint.ID];
        const value = msg.data.presentValue;
        if (!name || !Number.isFinite(value)) return {};
        if (name === 'session_word') {
            // 21-bit nonzero identity + 3-bit status, exact in a ZCL float32.
            // HA scripts must compare this SINGLE raw sensor, not combine
            // separate asynchronously updated session_id/status entities.
            if (!Number.isInteger(value) || value < 0 || value > 0xffffff) return {};
            const sessionId = Math.floor(value / 8);
            const status = value % 8;
            if ((sessionId === 0) !== (status === 0)) return {};
            return {session_word: value, session_id: sessionId, session_status: sessionStatuses[status],
                session_active: status === 1 || status === 2};
        }
        if (name === 'telemetry_valid_mask') {
            if (!Number.isInteger(value) || value < 0 || value > 2047) return {};
            const unknown = groups.flatMap((fields, bit) => value & (1 << bit) ? [] : fields);
            return {telemetry_valid_mask: value, battery_valid: !!(value & 1),
                ...Object.fromEntries(unknown.map(field => [field, null]))};
        }
        if (!fieldKnown(name, meta)) return {};
        if (name === 'battery' && (value < 0 || value > 100 || meta?.state?.battery_valid === false)) return {};
        if (name === 'open_doors_mask') {
            if (!Number.isInteger(value) || value < 0 || value > 63) return {};
            return {open_doors_mask: value, ...Object.fromEntries(doorNames.map((door, i) => [door, !!(value & (1 << i))]))};
        }
        return {[name]: value};
    },
};
const readMultistate = {
    cluster: 'genMultistateInput', type: ['attributeReport', 'readResponse'],
    convert: (_model, msg, _publish, _options, meta) => {
        const value = msg.data.presentValue;
        if (!Number.isInteger(value) || value < 0 || value > 3) return {};
        if (msg.endpoint.ID === 2 && !fieldKnown('climate_mode', meta)) return {};
        if (msg.endpoint.ID === 3 && !fieldKnown('climate_duration', meta)) return {};
        if (msg.endpoint.ID === 2) return {climate_mode: modes[value] ?? 'unknown'};
        if (msg.endpoint.ID === 3) return {climate_duration: durations[value] ?? 'unknown'};
        if (msg.endpoint.ID === 16) return {gateway_mode: gatewayModes[value] ?? 'unknown'};
        return {};
    },
};

const writePhev = {
    key: ['climate_command', 'mode_command', 'duration_command'],
    convertSet: async (_entity, key, value, meta) => {
        const message = meta.message ?? {};
        if (message.maintenance_mode !== undefined || message.refresh !== undefined)
            throw new Error('Envoyer maintenance/refresh separement des commandes clim');
        const mode = key === 'mode_command' ? value : message.mode_command;
        const duration = key === 'duration_command' ? value : message.duration_command;
        const modeIndex = mode === undefined ? undefined : modes.indexOf(mode);
        const durationIndex = duration === undefined ? undefined : durations.indexOf(String(duration));
        // Valider le message complet AVANT toute ecriture. Pour une commande
        // groupee, envoyer les parametres puis seulement la marche/arret.
        if (modeIndex !== undefined && modeIndex < 1) throw new Error('mode_command: cool, heat ou windscreen attendu');
        if (durationIndex !== undefined && durationIndex < 1) throw new Error('duration_command: 10, 20 ou 30 attendu');
        // Z2M appelle ce convertisseur UNE fois par message, avec la premiere
        // cle rencontree. Traiter la demande groupee quel que soit cet ordre.
        const climateValue = key === 'climate_command' ? value : message.climate_command;
        const options = {sendPolicy: 'immediate', timeout: 3000, disableRecovery: true};
        if (climateValue !== undefined) {
            if (typeof climateValue !== 'boolean') throw new Error('climate_command: true ou false attendu');
            if (modeIndex !== undefined)
                await meta.device.getEndpoint(2).write('genMultistateOutput', {presentValue: modeIndex}, options);
            if (durationIndex !== undefined)
                await meta.device.getEndpoint(3).write('genMultistateOutput', {presentValue: durationIndex}, options);
            await meta.device.getEndpoint(1).write('genBinaryOutput', {presentValue: climateValue}, options);
        } else if (key === 'mode_command') {
            const index = modes.indexOf(value);
            if (index < 1) throw new Error('mode_command: cool, heat ou windscreen attendu');
            await meta.device.getEndpoint(2).write('genMultistateOutput', {presentValue: index}, options);
        } else {
            const index = durations.indexOf(String(value));
            if (index < 1) throw new Error('duration_command: 10, 20 ou 30 attendu');
            await meta.device.getEndpoint(3).write('genMultistateOutput', {presentValue: index}, options);
        }
        // L'etat confirme provient exclusivement du retour de la voiture.
        return {};
    },
};

const writeGateway = {
    key: ['maintenance_mode', 'refresh'],
    convertSet: async (_entity, key, value, meta) => {
        if (['climate_command', 'mode_command', 'duration_command'].some(name => meta.message?.[name] !== undefined))
            throw new Error('Envoyer maintenance/refresh separement de la clim');
        let index;
        if (key === 'refresh') {
            if (value !== 'refresh') throw new Error('refresh: refresh attendu');
            index = 4;
        } else {
            index = gatewayModes.indexOf(value);
            if (index < 1) throw new Error('maintenance_mode: normal, ap ou wifi-seb attendu');
        }
        await meta.device.getEndpoint(16).write('genMultistateOutput', {presentValue: index});
        return {}; // la transition coupe Zigbee; ce n'est pas une MAJ OTA Zigbee
    },
};

export default {
    zigbeeModel: ['Outlander-PHEV-Remote'],
    model: 'Outlander-PHEV-Remote',
    vendor: 'PHEV-C6',
    version: '0.1.0-beta.1',
    meta: {
        overrideHaDiscoveryPayload: payload => {
            // Z2M uses bracket notation in real discovery; dot notation is
            // also accepted for user overrides. Null/unknown is not "false".
            const match = /\bvalue_json(?:\.([a-z_]+)|\[\s*["']([a-z_]+)["']\s*\])/.exec(payload.value_template ?? '');
            const property = match?.[1] ?? match?.[2];
            if (['session_word', 'session_id'].includes(property)) payload.entity_category = 'diagnostic';
            const group = groups.findIndex(fields => fields.includes(property));
            if (group >= 0 && typeof payload.state_topic === 'string') {
                const availability = property === 'battery' ?
                    '{{ "online" if value_json.battery_valid | default(false) else "offline" }}' :
                    `{{ "online" if ((value_json.telemetry_valid_mask | default(0) | int) // ${1 << group}) % 2 == 1 else "offline" }}`;
                payload.availability = [...(payload.availability ?? []), {
                    topic: payload.state_topic,
                    value_template: availability,
                }];
                payload.availability_mode = 'all';
            }
        },
    },
    description: 'Passerelle Wi-Fi Mitsubishi Outlander PHEV 2020 / Zigbee',
    fromZigbee: [readBinary, readAnalog, readMultistate],
    toZigbee: [writePhev, writeGateway],
    exposes: [
        e.binary('climate_command', ea.SET, true, false).withDescription('Demander marche/arret de la preclimatisation'),
        e.enum('mode_command', ea.SET, ['cool', 'heat', 'windscreen']),
        e.enum('duration_command', ea.SET, ['10', '20', '30']).withDescription('Minutes'),
        e.enum('maintenance_mode', ea.SET, ['normal', 'ap', 'wifi-seb']).withDescription('Redemarrer pour maintenance OTA; Zigbee suspendu en AP/Wi-Fi maison'),
        e.enum('refresh', ea.SET, ['refresh']).withDescription('Demander une actualisation des registres PHEV'),
        e.enum('gateway_mode', ea.STATE, gatewayModes),
        e.binary('climate_on', ea.STATE, true, false).withDescription('Dernier etat confirme pendant une session; pas une surveillance continue'),
        e.enum('climate_mode', ea.STATE, modes),
        e.enum('climate_duration', ea.STATE, durations).withDescription('Minutes'),
        e.numeric('battery', ea.STATE).withUnit('%').withValueMin(0).withValueMax(100).withDescription('Batterie de traction PHEV, pas alimentation de la passerelle'),
        e.binary('battery_valid', ea.STATE, true, false).withDescription('Derniere mesure connue de moins de 25 h; independant de TCP'),
        e.numeric('battery_age', ea.STATE).withUnit('s').withDescription('Age de la derniere mesure batterie confirmee, y compris au repos'),
        e.binary('phev_online', ea.STATE, true, false).withDescription('Session protocole active; false au repos est normal'),
        e.binary('wifi_connected', ea.STATE, true, false),
        e.binary('charging', ea.STATE, true, false),
        e.binary('plug_connected', ea.STATE, true, false),
        e.numeric('charge_remaining', ea.STATE).withUnit('min'),
        e.binary('doors_locked', ea.STATE, true, false),
        e.numeric('open_doors_mask', ea.STATE).withDescription('Bits 0..5 : conducteur, passager, arriere droit, arriere gauche, coffre, capot'),
        e.binary('headlights', ea.STATE, true, false),
        e.binary('parking_lights', ea.STATE, true, false),
        e.binary('command_ack', ea.STATE, true, false),
        e.binary('command_failed', ea.STATE, true, false),
        ...doorNames.map(name => e.binary(name, ea.STATE, true, false)),
        e.binary('climate_terminated', ea.STATE, true, false),
        e.binary('ac_operating', ea.STATE, true, false),
        e.binary('hazard_lights', ea.STATE, true, false),
        e.binary('interior_lights', ea.STATE, true, false),
        e.numeric('battery_warning_code', ea.STATE).withDescription('Code brut avertissement batterie, interpretation a confirmer; pas tension 12 V'),
        e.numeric('registered_devices', ea.STATE),
        e.numeric('wifi_rssi', ea.STATE).withUnit('dBm'),
        e.numeric('uptime', ea.STATE).withUnit('s'),
        e.numeric('telemetry_valid_mask', ea.STATE).withDescription('Groupes de dernieres mesures connues et non perimees (masque de diagnostic)'),
        e.numeric('session_word', ea.STATE).withValueMin(0).withValueMax(0xffffff).withDescription('Mot atomique ID/resultat session; utiliser ce seul capteur pour attendre une nouvelle operation'),
        e.numeric('session_id', ea.STATE).withValueMin(0).withValueMax(0x1fffff).withDescription('Identite de session depuis demarrage; 0 = aucune, reboucle apres 2097151'),
        e.enum('session_status', ea.STATE, sessionStatuses).withDescription('Resultat protocole; command_acknowledged ne confirme pas le fonctionnement reel de la clim'),
        e.binary('session_active', ea.STATE, true, false).withDescription('Une session courte Wi-Fi/PHEV est en cours, meme avant association Wi-Fi'),
    ],
    configure: async (device, coordinatorEndpoint) => {
        const bindings = [
            [1, 'genBinaryInput'], [2, 'genMultistateInput'], [3, 'genMultistateInput'],
            [4, 'genAnalogInput'], [5, 'genAnalogInput'], [6, 'genBinaryInput'],
            [7, 'genBinaryInput'], [8, 'genBinaryInput'], [9, 'genAnalogInput'],
            [10, 'genBinaryInput'], [11, 'genBinaryInput'], [12, 'genBinaryInput'],
            [13, 'genBinaryInput'], [14, 'genBinaryInput'], [15, 'genBinaryInput'],
            [16, 'genMultistateInput'], [17, 'genAnalogInput'], [18, 'genBinaryInput'],
            [19, 'genAnalogInput'], [20, 'genAnalogInput'], [21, 'genBinaryInput'],
            [22, 'genAnalogInput'], [23, 'genBinaryInput'], [24, 'genBinaryInput'],
            [25, 'genBinaryInput'], [26, 'genAnalogInput'],
            [27, 'genAnalogInput'],
            [28, 'genAnalogInput'],
        ];
        for (const [id, cluster] of bindings) {
            const endpoint = device.getEndpoint(id);
            if (!endpoint) throw new Error(`Endpoint ${id} absent: reinterviewer le PHEV apres flash .19, sans supprimer l'association`);
            await endpoint.bind(cluster, coordinatorEndpoint);
            // Le firmware publie les rapports standards lui-meme. Ne pas
            // remplir la table de rapports automatiques limitee du SDK C6.
        }
    },
};
