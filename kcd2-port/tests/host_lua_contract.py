"""Exercise the Lua closures embedded in the KCD2 host without launching the game."""

import os
from pathlib import Path
import sys

sys.path.insert(
    0,
    os.environ.get(
        "SKYCRAFT_TEST_PACKAGES", r"E:\SkyCraftKCD2Tools\python-test-packages"
    ),
)

from lupa.lua51 import LuaRuntime


HOST = Path(__file__).resolve().parents[1] / "host"


def embedded_lua(filename: str, function_name: str) -> str:
    source = (HOST / filename).read_text(encoding="utf-8")
    signature = next(
        text for text in (f"bool {function_name}(", f"void {function_name}(")
        if text in source
    )
    owner = source.index(signature)
    start = source.index('R"lua(', owner) + len('R"lua(')
    end = source.index(')lua"', start)
    # MSVC has a 16 KB limit per string token; adjacent raw literals form one closure.
    while source.startswith(')lua" R"lua(', end):
        end = source.index(')lua"', end + len(')lua"'))
    return source[start:end].replace(')lua" R"lua(', '')


def test_puppet() -> None:
    lua = LuaRuntime()
    lua.execute(
        """
        player = { id = 1, pos = { x = 0, y = 0, z = 0 } }
        function player:GetWorldPos()
            return { x = self.pos.x, y = self.pos.y, z = self.pos.z }
        end
        function player:SetWorldPos(position) self.pos = position end
        """
    )
    command = lua.execute(embedded_lua("skycraft_host.cpp", "PreparePuppet"))
    assert command("move", 0, 0, 0)["status"] == "steady"
    assert command("move", 1, 2, 3)["status"] == "moved"
    position = lua.globals().player.pos
    assert (position.x, position.y, position.z) == (1, -3, 2)
    lua.execute("player.pos = { x = 10, y = 0, z = 0 }")
    assert command("move", 2, 2, 3)["status"] == "native teleport"
    assert command("reset", 0, 0, 0)["status"] == "reset"
    assert command("move", 2, 2, 3)["status"] == "moved"


def test_snapshot_level() -> None:
    lua = LuaRuntime()
    lua.execute(
        """
        player = { GetWorldPos = function()
            return { x = 1, y = 2, z = 3 }
        end }
        System = {
            GetViewCameraDir = function()
                return { x = 0, y = 1, z = 0 }
            end,
            GetCVar = function(name)
                if name == "sv_map" then return "trosecko" end
            end
        }
        """
    )
    snapshot = lua.execute(embedded_lua("skycraft_host.cpp", "PrepareSnapshot"))
    assert snapshot()["levelName"] == "trosecko"
    lua.execute(
        """
        System.GetCVar = function() return nil end
        Game = { GetLevelName = function() return "kuttenberg" end }
        """
    )
    assert snapshot()["levelName"] == "kuttenberg"
    lua.execute("Game = nil")
    assert snapshot()["levelName"] is None
    assert snapshot()["alive"] is True
    lua.execute('''
        System.GetViewCameraPos = function() return { x=10, y=20, z=30 } end
        System.GetViewCameraFov = function() return math.pi / 3 end
        System.ProjectToScreen = function(point)
            assert(point.x == 10 and point.z == 30)
            local distance = point.y - 20
            return { x=50, y=50, z=(.15*2000/distance-.15)/(2000-.15) }
        end
    ''')
    camera = snapshot()
    assert abs(camera["fov"] - 60) < 1e-5
    assert abs(camera["depth1"] - (.15*2000-.15)/(2000-.15)) < 1e-8
    assert abs(camera["depth8"] - (.15*2000/8-.15)/(2000-.15)) < 1e-8
    lua.execute('System.ProjectToScreen = function() error("unavailable") end')
    assert snapshot()["depth1"] is None and abs(snapshot()["fov"] - 60) < 1e-5
    lua.execute('System.GetViewCameraFov = function() return 4.23297e21 end')
    assert snapshot()["fov"] == 70
    lua.execute(
        """
        player.id = 1
        player.soul = { hp = 100 }
        function player.soul:GetState(name) return self.hp end
        resets, loads = 0, 0
        Player = {
            OnReset = function(self, fromInit, isReload)
                assert(self == player and not fromInit and isReload)
                resets = resets + 1
                return "reset result", 42
            end,
            OnLoadAI = function(self, saved)
                assert(self == player and saved.token == 123)
                loads = loads + 1
                return "load result"
            end
        }
        setmetatable(player, { __index = Player })
        """
    )
    initial = snapshot()
    revision = initial["loadRevision"]
    assert lua.execute("return player:OnReset(false, true)") == ("reset result", 42)
    assert snapshot()["loadRevision"] > revision
    revision = snapshot()["loadRevision"]
    assert lua.execute("return player:OnLoadAI({ token = 123 })") == "load result"
    assert snapshot()["loadRevision"] > revision
    assert lua.globals().loads == 1 and lua.globals().resets == 1
    # Repeated snapshots must not wrap existing callbacks again.
    revision = snapshot()["loadRevision"]
    assert snapshot()["loadRevision"] == revision
    lua.execute("player.soul.hp = 0")
    assert snapshot()["alive"] is False
    lua.execute("player.soul.hp = 100")
    assert snapshot()["alive"] is True
    lua.execute("player.soul.GetState = function() error('not loaded') end")
    assert snapshot() is None


def test_native_attack_routing() -> None:
    lua = LuaRuntime()
    lua.execute(
        """
        maps = { combat = true, combat_base = true, stone_throwing = false }
        ActionMapManager = {
            IsActionMapEnabled = function(name) return maps[name] end,
            EnableActionMap = function(name, enabled) maps[name] = enabled end
        }
        """
    )
    route = lua.execute(embedded_lua("skycraft_host.cpp", "UpdateNativeActionRouting"))
    assert route(True)["attackMapsChanged"] == 3
    maps = lua.globals().maps
    assert not maps.combat and not maps.combat_base and not maps.stone_throwing
    assert route(False)["attackMapsChanged"] == 3
    assert maps.combat and maps.combat_base and not maps.stone_throwing
    # Missing state queries must never activate an unknown map on handoff.
    lua.execute("ActionMapManager.IsActionMapEnabled = nil")
    assert route(True)["unknownMapStates"] == 3
    assert maps.combat and maps.combat_base and not maps.stone_throwing
    assert route(False)["attackMapsChanged"] == 0
    assert maps.combat and maps.combat_base and not maps.stone_throwing


def test_combat() -> None:
    lua = LuaRuntime()
    lua.execute(
        """
        System = {}
        player = { id = 1, actor = { hp = 100 },
            pos = { x = 0, y = 0, z = 0 } }
        function player.actor:GetHealth() return self.hp end
        function player.actor:SetHealth(value) self.hp = value end
        function player:GetWorldPos() return self.pos end
        """
    )
    command = lua.execute(embedded_lua("skycraft_combat.cpp", "PrepareCommand"))
    assert command("health", 0, 0)["status"] == "primed"
    lua.execute("player.actor.hp = 90")
    hurt = command("health", 0, 0)
    assert (hurt["status"], hurt["damage"]) == ("hurt", 10)
    assert lua.globals().player.actor.hp == 100
    assert command("reset", 0, 0)["status"] == "reset"
    lua.execute("player.actor.hp = 80")
    assert command("health", 0, 0)["status"] == "primed"
    lua.execute("player.actor.GetHealth = function() error('unavailable') end")
    assert command("death", 0, 0)["status"] == "death health read failed"
    lua.execute(
        """
        player.actor.GetHealth = function(self) return self.hp end
        target = { id = 2, actor = { hp = 50 },
            pos = { x = 2, y = 0, z = 0 } }
        function target.actor:GetHealth() return self.hp end
        function target:GetWorldPos() return self.pos end
        System.GetEntities = function() return { target } end
        System.GetEntity = function(id) if id == target.id then return target end end
        g_gameRules = { CreateHit = function() end }
        """
    )
    scan = command("scan", 0, 0)
    assert scan["status"] == "ok"
    target_id = scan["actors"][1]["id"]
    hit = command("hit", target_id, 5, "normal", 0, 0, 0)
    assert hit["status"] == "hit submitted"
    assert (hit["healthBefore"], hit["healthAfter"]) == (50, 50)
    lua.execute(
        """
        target.soul = { hp = 60 }
        function target.soul:GetState(name) assert(name == "health"); return self.hp end
        function target.soul:DealDamage(stamina, health, attacker, noReaction, part)
            assert(stamina == 0 and attacker == player.id and noReaction and part == 'torso')
            self.hp = self.hp - health
        end
        function target.soul:SetState(name, value) self.hp = value end
        g_gameRules.CreateHit = function() error('must not deal twice') end
        target.GetMaxHealth = function() return 1 end
        """
    )
    scan = command("scan", 0, 0)
    assert scan["actors"][1]["health"] == 60
    assert scan["actors"][1]["healthFrac"] == 0.6
    hit = command("hit", target_id, 5, "normal", 0, 0, 0)
    assert hit["status"] == "health reduced"
    assert hit["damageApi"] == "soul.DealDamage(stamina,health,attacker,noReaction,part)"
    assert (hit["healthBefore"], hit["healthAfter"], hit["healthSource"]) == (60, 55, "soul")
    assert lua.globals().target.actor.hp == 50
    healed = command("heal", target_id, 10, "normal", 0, 0, 0)
    assert healed["status"] == "health restored" and healed["healthAfter"] == 65
    healed = command("heal", target_id, 100, "normal", 0, 0, 0)
    assert healed["healthAfter"] == 100
    # Read failures must not be presented as successful damage to generic actor HP.
    lua.execute(
        """
        target.soul.GetState = function() error('unavailable') end
        g_gameRules.CreateHit = function() end
        """
    )
    hit = command("hit", target_id, 5, "normal", 0, 0, 0)
    assert hit["status"] == "hit failed"
    assert (hit["healthBefore"], hit["healthAfter"], hit["healthSource"]) == (-1, -1, "soul")
    # A rejected native call must never fall back to a second damage API.
    lua.execute("target.soul.GetState = function(self) return self.hp end; target.soul.DealDamage = function() return false end")
    assert command("hit", target_id, 5, "normal", 0, 0, 0)["status"] == "hit failed"
    lua.execute("target.soul.DealDamage = nil")
    assert command("hit", target_id, 5, "normal", 0, 0, 0)["status"] == "hit failed"
    command("reset", 0, 0)
    assert command("hit", target_id, 5, "normal", 0, 0, 0)["status"] == "target gone"
    lua.execute(
        """
        player.soul = { hp = 40 }
        function player.soul:GetState(name) assert(name == "health"); return self.hp end
        function player.soul:SetState(name, value) assert(name == "health"); self.hp = value end
        """
    )
    assert command("health", 0, 0)["status"] == "primed"
    lua.execute("player.soul.hp = 35")
    hurt = command("health", 0, 0)
    assert (hurt["status"], hurt["damage"]) == ("hurt", 5)
    assert lua.globals().player.soul.hp == 40
    assert lua.globals().player.actor.hp == 80
    lua.execute("player.soul.hp = 0")
    assert command("health", 0, 0)["status"] == "hurt"
    assert lua.globals().player.soul.hp == 0
    command("reset", 0, 0)
    lua.execute("player.soul.hp = 40")
    assert command("health", 0, 0)["status"] == "primed"
    lua.execute(
        """
        function player.soul:DealDamage(stamina, amount, source, direct)
            assert(stamina == 0)
            self.hp = math.max(0, self.hp - amount)
        end
        """
    )
    assert command("death", 0, 0)["status"] == "death observed"
    assert command("death", 0, 0)["status"] == "already dead"


def test_npc_block_separation() -> None:
    lua = LuaRuntime()
    lua.execute(
        """
        player = { id = 1 }
        npc = { id = 2, actor = { GetHealth = function() return 100 end },
            pos = { x = 1.5, y = -0.5, z = 0 } }
        function npc:GetWorldPos() return self.pos end
        function npc:SetWorldPos(position) self.pos = position end
        atop = { id = 3, human = {}, pos = { x = 1.5, y = -0.5, z = 1 } }
        function atop:GetWorldPos() return self.pos end
        function atop:SetWorldPos(position) self.pos = position end
        distant = { id = 4, human = {}, pos = { x = 20, y = -0.5, z = 0 } }
        function distant:GetWorldPos() return self.pos end
        function distant:SetWorldPos(position) self.pos = position end
        scans = 0
        System = {
            GetEntities = function()
                scans = scans + 1
                return { player, npc, atop, distant }
            end,
            GetEntity = function(id)
                for _, entity in ipairs({ player, npc, atop, distant }) do
                    if entity.id == id then return entity end
                end
            end
        }
        """
    )
    command = lua.execute(
        embedded_lua("skycraft_physics.cpp", "PrepareNpcCommand")
    )
    blocks = lua.table_from([lua.table_from([1, 0, 0])])
    first = command(blocks, 0, 0, 0, 1)
    assert (first["status"], first["moved"]) == ("ok", 1)
    assert lua.globals().npc.pos.x == 1.0
    assert lua.globals().atop.pos.x == 1.5
    assert lua.globals().distant.pos.x == 20
    second = command(blocks, 0, 0, 0, 1)
    assert second["moved"] == 1
    assert abs(lua.globals().npc.pos.x - 0.65) < 1e-6
    assert lua.globals().scans == 1
    assert command(lua.table_from([]), 0, 0, 0, 1)["moved"] == 0
    assert lua.globals().scans == 1
    assert command(blocks, 0, 0, 0, 1)["status"] == "ok"
    assert lua.globals().scans == 2
    assert command(blocks, 0, 0, 0, 2)["status"] == "ok"
    assert lua.globals().scans == 3
    # A low slab does not block an NPC standing on its surface.
    lua.execute("npc.pos = { x = 1.5, y = -0.5, z = 0.5 }")
    slab = lua.table_from([lua.table_from([1, 0, 0, 0, 0, 0, 1, 0.5, 1])])
    assert command(slab, 0, 0, 0, 2)["moved"] == 0
    # Fence geometry extends above its owning cell.
    lua.execute("npc.pos = { x = 1.5, y = -0.5, z = 1 }")
    fence = lua.table_from([lua.table_from([1, 0, 0, 0.375, 0, 0.375, 0.625, 1.5, 0.625])])
    assert command(fence, 0, 0, 0, 2)["moved"] > 0


def test_lethal_combat() -> None:
    lua = LuaRuntime()
    lua.execute('''
        player = { id=1, pos={x=0,y=0,z=0} }
        function player:GetWorldPos() return self.pos end
        target = { id=2, pos={x=1,y=0,z=0}, soul={hp=20,calls=0}, actor={hp=100} }
        function target:GetWorldPos() return self.pos end
        function target:GetLocalBBox() return {x=-0.8,y=-0.3,z=0}, {x=0.8,y=0.3,z=1} end
        function target.actor:GetHealth() return self.hp end
        function target.soul:GetState(name) assert(name == 'health'); return self.hp end
        function target.soul:SetState(name,value) assert(value > 0); self.hp=value end
        function target.actor:IsDead() return self.hp <= 0 end
        function target.actor:IsUnconscious() return self.unconscious == true end
        function target.soul:DealDamage(stamina, hp, attacker, noReaction, part)
            assert(stamina == 0 and attacker == 1 and noReaction and part == 'torso')
            self.calls=self.calls+1
            if hp < self.hp then self.hp = self.hp - hp end
        end
        System = { GetEntities=function() return {target} end,
            GetEntity=function(id) if id==2 then return target end end }
    ''')
    command = lua.execute(embedded_lua("skycraft_combat.cpp", "PrepareCommand"))
    def scan(now=0): return command("scan", 0, 0, "normal", 0, 0, 0, now)
    key = scan()["actors"][1]["id"]
    actor = scan()["actors"][1]
    assert actor["width"] == 1.6 and actor["height"] == 1 and not actor["dead"]
    def hit(amount, now=0): return command("hit", key, amount, "normal", 0, 0, 0, now)
    assert hit(5)["status"] == "health reduced"
    assert hit(15, 100)["status"] == "death submitted"
    assert hit(15, 200)["status"] == "death submitted"
    assert lua.globals().target.soul.calls == 2  # one nonlethal and one lethal hit
    assert not scan(500)["actors"][1]["dead"]  # submission is not confirmation
    lua.execute("target.actor.hp=0")  # engine death may precede the Soul readback
    result = scan(600)
    assert result["actors"][1]["dead"] and result["deaths"][1]["status"] == "death observed"
    assert len(scan(700)["deaths"]) == 0
    assert hit(1, 800)["status"] == "target dead"
    command("reset", 0, 0)
    lua.execute("target.actor.hp=100; target.actor.unconscious=true; target.soul.hp=20")
    key = scan(1000)["actors"][1]["id"]
    assert hit(19.9995, 1000)["status"] == "death submitted"  # float transport rounding
    result = scan(4000)
    assert result["deaths"][1]["status"] == "death unconfirmed"
    assert not result["actors"][1]["dead"]
    assert lua.globals().target.soul.calls == 3  # polling never deals again
    assert hit(20, 4001)["status"] == "death submitted"
    assert lua.globals().target.soul.calls == 4  # a later real attack is allowed
    command("reset", 0, 0)
    key = scan(5000)["actors"][1]["id"]
    lua.execute("target.soul.DealDamage=function() return false end")
    assert hit(20, 5000)["status"] == "hit failed"
    assert not scan(9000)["actors"][1]["dead"]
    lua.execute("target.soul.DealDamage=function(self,stamina,hp) assert(stamina==0); self.hp=0; target.actor.hp=0 end")
    assert hit(20, 9100)["status"] == "death observed"


def test_native_knockback() -> None:
    lua = LuaRuntime()
    lua.execute('''
        player = {id=1, GetWorldPos=function() return {x=0,y=0,z=0} end}
        target = {id=2, pos={x=1,y=0,z=0}, actor={GetHealth=function() return 100 end},
            soul={hp=100}, calls=0}
        function target:GetWorldPos() return {x=self.pos.x,y=self.pos.y,z=self.pos.z} end
        function target:SetWorldPos(pos) self.pos=pos end
        function target.soul:GetState() return self.hp end
        function target.soul:DealDamage(stamina,hp) assert(stamina==0); self.hp=self.hp-hp end
        System={GetEntities=function() return {target} end, GetEntity=function(id) return id==2 and target end}
        ent_all=15
        wall=false
        Physics={RayWorldIntersection=function(origin,ray,max,mask,skip1,skip2,hits)
            assert(mask==15 and skip1==2 and skip2==1)
        target.calls=target.calls+1
            if wall then hits[1]={dist=0.35}; return 1 end
            return 0
        end}
    ''')
    command = lua.execute(embedded_lua("skycraft_combat.cpp", "PrepareCommand"))
    key = command("scan", 0, 0, "normal", 0, 0, 0, 0)["actors"][1]["id"]
    hit = command("hit", key, 5, "normal", 1, 0, .4, 100)
    assert hit["pushApi"] == "collision-swept velocity" and hit["impulse"]
    moved = command("tick", 0, 0, "normal", 0, 0, 0, 150)
    assert moved["moved"] == 1 and moved["failed"] == 0
    assert abs(lua.globals().target.pos.x - 1.4) < 1e-6
    assert lua.globals().target.calls == 9
    current = command("scan", 0, 0, "normal", 0, 0, 0, 150)["actors"][1]
    assert abs(current["x"] - 1.4) < 1e-6  # cache does not freeze actor position
    lua.execute("wall=true")
    blocked = command("tick", 0, 0, "normal", 0, 0, 0, 200)
    assert blocked["blocked"] == 1 and blocked["failed"] == 0
    assert lua.globals().target.pos.x <= 1.41
    x = lua.globals().target.pos.x
    command("tick", 0, 0, "normal", 0, 0, 0, 250)
    assert lua.globals().target.pos.x == x
    lua.execute("wall=false; Physics.RayWorldIntersection=function() error('failed') end")
    command("hit", key, 5, "normal", 1, 0, .4, 300)
    failed = command("tick", 0, 0, "normal", 0, 0, 0, 350)
    assert failed["failed"] > 0 and lua.globals().target.pos.x == x


def test_native_collision_proxy() -> None:
    lua = LuaRuntime()
    lua.execute(
        """
        entities, nextId, removed = {}, 0, 0
        PHYSICPARAM_COLLISION_CLASS, PHYSICPARAM_PART_FLAGS = 1, 2
        geom_colltype_player, geom_colltype_ray = 4, 8
        gcc_interactive, gcc_rigid, gcc_npc_avoiding_types = 16, 32, 16
        System = {
            GetEntity = function(id) return entities[id] end,
            RemoveEntity = function(id) entities[id] = nil; removed = removed + 1 end,
            SpawnEntity = function(params)
                nextId = nextId + 1
                local entity = { id = nextId, params = params }
                function entity:GetName() return self.params.name end
                function entity:GetLocalBBox()
                    return { x = -1, y = -1, z = 0 }, { x = 1, y = 1, z = 2 }
                end
                function entity:DrawSlot(slot, visible) self.hidden = visible == 0 end
                function entity:SetPhysicParams(kind, params)
                    if kind == PHYSICPARAM_COLLISION_CLASS then self.filter = params end
                    if kind == PHYSICPARAM_PART_FLAGS then self.geometry = params end
                end
                entities[entity.id] = entity
                return entity
            end
        }
        """
    )
    command = lua.execute(embedded_lua("skycraft_physics.cpp", "PrepareCommand"))
    result = command("spawn", 0, 10, 20, 30, 0, 0, 0, 0, 1, 0.5, 1)
    assert result["ok"] and result["geometryApplied"]
    assert result["obstacleClass"] == 48  # bitwise union, no overlap counted twice
    entity = lua.globals().entities[2]
    assert entity.hidden and entity.filter.collisionClassIgnore == 0
    assert entity.geometry.flags == 12
    assert (entity.params.scale.x, entity.params.scale.y, entity.params.scale.z) == (0.5, 0.5, 0.25)
    assert (entity.params.position.x, entity.params.position.y, entity.params.position.z) == (10.5, -30.5, 20)
    # Parts in one cell are independent and the calibration is reused.
    assert command("spawn", 0, 10, 20, 30, 1, 0, 0, 0, 0.5, 1, 1)["ok"]
    assert lua.globals().nextId == 3
    assert command("remove", 0, 10, 20, 30, 0)["ok"]
    assert lua.globals().entities[2] is None and lua.globals().entities[3] is not None


if __name__ == "__main__":
    test_snapshot_level()
    test_puppet()
    test_native_attack_routing()
    test_combat()
    test_lethal_combat()
    test_native_knockback()
    test_npc_block_separation()
    test_native_collision_proxy()
    print("host Lua contracts passed")
