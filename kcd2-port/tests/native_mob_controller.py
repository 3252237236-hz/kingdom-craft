"""Check the authored native mob controller and its table/brain contracts.

This does not launch KCD2 or claim that XGen accepts the profile in a save.
"""
from pathlib import Path
import argparse
import os
import re
import sys
import xml.etree.ElementTree as ET
import zipfile

sys.path.insert(0, os.environ.get('SKYCRAFT_TEST_PACKAGES', 'E:/SkyCraftKCD2Tools/python-test-packages'))
from lupa.lua51 import LuaRuntime

ROOT = Path(__file__).resolve().parents[1] / 'native-mobs'
SOURCE = ROOT / 'Scripts/Mods/kingdomcraft_mobs/NativeMobs.lua'
CHECKS = 0


def check(condition):
    global CHECKS
    assert condition, f'check {CHECKS+1} failed'
    CHECKS += 1


def fixture():
    lua = LuaRuntime()
    lua.globals().newHandle = object
    lua.execute('''
        now=100;entities={};spawned={};spawnCalls=0;rejectSpawn=false
        System={GetCurrTime=function() return now end,
            GetEntity=function(id) return entities[id] end}
        XGenAIModule={SpawnEntity=function(params)
            spawnCalls=spawnCalls+1
            if rejectSpawn then return nil end
            local id=newHandle()
            local entity={id=id,params=params,pos=params.Pos,
                soul={__ThisWUID=newHandle(),hp=100}}
            function entity.soul:GetState(name) assert(name=='health');return self.hp end
            function entity.soul:SetState(name,value) assert(name=='health');self.hp=value end
            function entity:DrawSlot(slot,visible) assert(slot==0 and visible==0);self.drawn=visible end
            function entity:GetWorldPos() return self.pos end
            function entity:SetWorldPos(pos) self.pos=pos end
            function entity:GetName() return self.params.Name end
            entities[id]=entity;spawned[#spawned+1]=entity
            return id
        end}
    ''')
    controller = lua.execute(SOURCE.read_text(encoding='utf-8'))
    return lua, controller


SESSION = 'a'*32
UUID = 'b'*32


def row(lua, entity=7, uuid=UUID):
    return lua.table_from(dict(entityId=entity, uuid=uuid, x=5, y=10, z=3,
                               yaw=45, width=.6, height=1.8, health=20, maxHealth=20))


def test_lifecycle_and_damage():
    lua, m = fixture()
    rows = lua.table_from([row(lua)])
    result = m.sync(9, SESSION, rows)
    check(result['live'] == 1 and result['spawned'] == 1 and result['failed'] == 0)
    native = m.resolve(7, UUID)
    check(native is not None and native['id'] != 7 and m.keeps(native))
    check(m.owns(native) and m.resolveAttack(9, SESSION, 7, UUID) is not None)
    check(m.resolveAttack(8, SESSION, 7, UUID) is None)
    check(m.resolveAttack(9, 'c'*32, 7, UUID) is None)
    check(m.resolveAttack(9, SESSION, 7, 'c'*32) is None)
    check(m.resolveAttack(9, SESSION, 7, None) is None)
    check(m.resolveAttack(9, SESSION, 7, '') is None)
    check(native['params']['ClassName'] == 'NPC')
    check(native['params']['NoAI'] is False and native['params']['SuspendedAI'] is False)
    check(native['params']['PerceptibleObjectAI'] is True and native['params']['PerceptorObjectAI'] is False)
    check(native['drawn'] == 0 and (native['pos']['x'], native['pos']['y'], native['pos']['z']) == (5, -3, 10))
    old_id, old_soul = native['id'], native['soul']['__ThisWUID']
    check(m.resolve(7, 'c'*32) is None)
    native['soul']['hp'] = 85
    reports = m.damageReports()
    check(len(reports) == 1 and reports[1]['damage'] == 3)
    receipt = reports[1]
    check(receipt['entityId'] == 7 and receipt['uuid'] == UUID and receipt['epoch'] == 9)
    check(native['soul']['hp'] == 100 and native['soul']['__ThisWUID'] is old_soul)
    check(len(m.damageReports()) == 1 and m.damageReports()[1]['serial'] == receipt['serial'])
    check(not m.acknowledge(8, SESSION, receipt['serial'], 7, UUID))
    check(not m.acknowledge(9, 'c'*32, receipt['serial'], 7, UUID))
    check(not m.acknowledge(9, SESSION, receipt['serial'], 8, UUID))
    check(not m.acknowledge(9, SESSION, receipt['serial'], 7, 'c'*32))
    check(m.acknowledge(9, SESSION, receipt['serial'], 7, UUID))
    check(not m.acknowledge(9, SESSION, receipt['serial'], 7, UUID) and len(m.damageReports()) == 0)
    native['soul']['hp'] = 80
    later = m.damageReports()[1]
    check(later['serial'] > receipt['serial'] and later['damage'] == 4)
    # Removing/reusing a Minecraft numerical ID cannot retain its old native identity.
    m.sync(9, SESSION, lua.table_from([row(lua, 7, 'c'*32)]))
    replacement = m.resolve(7, 'c'*32)
    check(replacement is not None and replacement['id'] is not old_id and not m.keeps(native))
    check(m.owns(native))  # old double is hidden from native NPC publication until despawn
    check(m.resolve(7, UUID) is None)
    check(len(m.damageReports()) == 1)  # unacknowledged hurt survives an ordinary range removal
    m.sync(10, SESSION, lua.table_from([row(lua)]))
    check(len(m.damageReports()) == 0 and not m.keeps(replacement))
    after_epoch = m.resolve(7, UUID)
    check(after_epoch is not None and after_epoch['id'] is not replacement['id'])
    m.sync(10, 'd'*32, lua.table_from([row(lua)]))
    check(not m.keeps(after_epoch) and m.resolve(7, UUID) is not None)
    live = m.resolve(7, UUID)
    lua.globals().now = 103
    check(not m.keeps(live) and m.resolve(7, UUID) is None)
    m.sync(10, 'd'*32, lua.table_from([row(lua)]))
    check(not m.keeps(live) and m.resolve(7, UUID)['id'] is not live['id'])
    lua.globals().now = 90
    check(m.resolve(7, UUID) is None)  # time reversal is not a valid lease
    m.reset()
    check(len(m.damageReports()) == 0 and m.resolve(7, UUID) is None)


def test_failure_paths_and_bounds():
    lua, m = fixture()
    m.sync(1, SESSION, lua.table_from([row(lua)]))
    native = m.resolve(7, UUID)
    native_id = native['id']
    for bad in (float('nan'), float('inf'), -1, 0, '7'):
        broken = row(lua)
        broken['entityId'] = bad
        check(m.sync(1, SESSION, lua.table_from([broken]))['status'] == 'invalid snapshot')
        check(m.resolve(7, UUID)['id'] is native_id)
    for key, value in (('uuid', 'not-a-uuid'), ('x', float('nan')), ('health', 21),
                       ('maxHealth', 0), ('width', -1), ('height', float('inf'))):
        broken = row(lua);broken[key] = value
        check(m.sync(1, SESSION, lua.table_from([broken]))['status'] == 'invalid snapshot')
    check(m.sync(1, SESSION, lua.table_from([row(lua), row(lua)]))['status'] == 'invalid snapshot')
    check(m.sync(1, SESSION, lua.table_from([row(lua), row(lua, 8)]))['status'] == 'invalid snapshot')
    check(m.sync(1, SESSION, lua.table_from([row(lua, 8, 'c'*32)]))['live'] == 1)
    check(not m.keeps(native))
    native = m.resolve(8, 'c'*32)
    native['soul']['__ThisWUID'] = 8
    check(m.resolve(8, 'c'*32) is None and not m.keeps(native))
    m.sync(1, SESSION, lua.table_from([row(lua)]))
    native = m.resolve(7, UUID)
    native['soul']['hp'] = 0
    check(m.damageReports()[1]['damage'] == 20 and not m.keeps(native))
    m.sync(1, SESSION, lua.table_from([row(lua)]))
    replacement = m.resolve(7, UUID)
    check(replacement is not None and replacement['id'] is not native['id'])
    # Failed restoration must not repeatedly count the same native loss.
    lua.execute('spawned[#spawned].soul.SetState=function() return false end')
    replacement['soul']['hp'] = 90
    check(len(m.damageReports()) == 2 and not m.keeps(replacement))
    check(len(m.damageReports()) == 2)
    # A reused Cry handle with a different Soul must fail identity validation.
    m.sync(1, SESSION, lua.table_from([row(lua)]))
    replacement = m.resolve(7, UUID)
    replacement['soul']['__ThisWUID'] = object()
    check(m.resolve(7, UUID) is None)
    m.reset()
    lua.globals().rejectSpawn = True
    many = lua.table_from([row(lua, i, f'{i:032x}') for i in range(1, 65)])
    before = lua.globals().spawnCalls
    check(m.sync(1, SESSION, many)['failed'] == 4 and lua.globals().spawnCalls == before+4)
    check(m.sync(1, SESSION, lua.table_from([row(lua, i, f'{i:032x}') for i in range(1, 66)]))['status'] == 'invalid snapshot')
    lua.execute('System.GetCurrTime=nil')
    check(m.sync(1, SESSION, many)['status'] == 'native clock/entity API unavailable')


def test_profiles(game_tables=None):
    manifest = ET.parse(ROOT / 'mod.manifest').getroot()
    modid = manifest.findtext('info/modid')
    check(modid == 'kingdomcraft_mobs')
    tables = {}
    for path in sorted((ROOT / 'Libs/Tables').rglob('*.xml')):
        name = path.name.split('__')[0]
        root = ET.parse(path).getroot()
        check(path.stem.endswith('__'+modid) and root.tag == 'database')
        tables[name] = list(list(root)[0])
        if game_tables:
            with zipfile.ZipFile(game_tables) as archive:
                base = ET.fromstring(archive.read('Libs/Tables/'+path.parent.name+'/'+name+'.xml'))
                check(list(root)[0].tag == list(base)[0].tag and list(root)[0].get('version') == list(base)[0].get('version'))
                primary = {'soul':'soul_id','brain':'brain_id','subbrain':'subbrain_id'}.get(name)
                if primary:
                    for original in archive.namelist():
                        if original.startswith('Libs/Tables/'+path.parent.name+'/'+name) and original.endswith('.xml'):
                            check(all(e.get(primary) != tables[name][0].get(primary)
                                      for e in ET.fromstring(archive.read(original)).iter(name)))
    check(len(tables) == 5)
    soul, brain, sub = tables['soul'][0], tables['brain'][0], tables['subbrain'][0]
    check(soul.get('brain_id') == brain.get('brain_id'))
    link, tree = tables['brain2subbrain'][0], tables['subbrain_behaviour_tree'][0]
    check(link.get('brain_id') == brain.get('brain_id') and link.get('subbrain_id') == sub.get('subbrain_id'))
    check(tree.get('subbrain_id') == sub.get('subbrain_id') and sub.get('always_active') == 'true')
    check(re.search(r'local PROFILE = "([^"]+)"', SOURCE.read_text())[1] == soul.get('soul_id'))
    behavior = ET.parse(ROOT / 'AI' / tree.get('file_name')).getroot().find('BehaviorTree')
    check(behavior.get('name') == tree.get('tree_name'))
    check(behavior.find('Root').get('OneTimeOnly') == 'false')
    check(behavior.find('.//Despawn').get('DeSpawnedEntity') == '$this.id')
    check('SkyCraftNativeMobs.keeps(entity)' in behavior.find('.//ExecuteLua').get('code'))
    check(not any(e.tag in ('Move','PathFind','Throw','Spawn','DealDamage') for e in behavior.iter()))
    check('System.RemoveEntity(' not in SOURCE.read_text())


def test_delivery_backpressure():
    lua, m = fixture()
    m.sync(1, SESSION, lua.table_from([row(lua)]))
    native = m.resolve(7, UUID)
    for _ in range(513):
        native['soul']['hp'] = 95
        receipts = m.damageReports()
    check(len(receipts) == 512 and native['soul']['hp'] == 100)
    # Range removal under pressure must not lose the 513th observed native hit.
    m.sync(1, SESSION, lua.table_from([row(lua, 8, 'c'*32)]))
    check(not m.keeps(native) and m.resolve(8, 'c'*32) is None)
    oldest = receipts[1]
    check(m.acknowledge(1, SESSION, oldest['serial'], 7, UUID))
    receipts = m.damageReports()
    check(len(receipts) == 512 and sum(r['damage'] for _, r in receipts.items())+oldest['damage'] == 513)
    check(m.sync(1, SESSION, lua.table_from([row(lua, 8, 'c'*32)]))['spawned'] == 0)
    for _, receipt in receipts.items():
        m.acknowledge(1, SESSION, receipt['serial'], receipt['entityId'], receipt['uuid'])
    check(len(m.damageReports()) == 0)
    check(m.sync(1, SESSION, lua.table_from([row(lua, 8, 'c'*32)]))['spawned'] == 1)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--game-tables', type=Path)
    args = parser.parse_args()
    test_lifecycle_and_damage()
    test_failure_paths_and_bounds()
    test_delivery_backpressure()
    test_profiles(args.game_tables)
    print(f'native mob controller contracts passed ({CHECKS} checks); KCD2 profile loading and Minecraft transport unverified')
