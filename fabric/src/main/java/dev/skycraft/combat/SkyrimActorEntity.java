package dev.skycraft.combat;

import dev.skycraft.link.Proto;
import dev.skycraft.link.Kcd2Protocol;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.network.syncher.EntityDataSerializers;
import net.minecraft.network.syncher.SynchedEntityData;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.sounds.SoundEvent;
import net.minecraft.world.damagesource.DamageSource;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityDimensions;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.HumanoidArm;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.Pose;
import net.minecraft.tags.ItemTags;
import net.minecraft.world.entity.projectile.Projectile;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.item.Items;
import net.minecraft.world.level.Level;
import net.minecraft.world.phys.Vec3;
import org.jspecify.annotations.Nullable;

/**
 * An invisible stand-in for one Skyrim actor, so Minecraft's own combat (swords, crits, sweeps,
 * enchantments, attack cooldown, bows, tridents) can target and hit Skyrim NPCs. What it receives is
 * collected into one hit per tick and forwarded to the real actor; its own health never drops.
 */
public class SkyrimActorEntity extends LivingEntity {
	private static final EntityDataAccessor<Integer> FORM_ID = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.INT);
	private static final EntityDataAccessor<Float> WIDTH = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.FLOAT);
	private static final EntityDataAccessor<Float> HEIGHT = SynchedEntityData.defineId(SkyrimActorEntity.class, EntityDataSerializers.FLOAT);

	// This tick's hit, flushed to Skyrim by SkyCombat after all attacks for the tick have landed
	// (Player.attack adds its sprint/enchantment knockback after hurtServer returns).
	private float pendingDamage;
	private float pendingHealing;
	private int pendingFlags;
	private int pendingWeapon;
	private double pushX, pushZ;
	private float pushStrength;
	private boolean hitThisTick;
	private final java.util.ArrayDeque<NativeAttackTrace> nativeHits = new java.util.ArrayDeque<>();
	private @Nullable Vec3 lastEnvironmentPosition;
	private boolean capturingNativeDamage;
	private float damageHealthBefore;
	private float effectiveNativeDamage;

	public SkyrimActorEntity(EntityType<? extends SkyrimActorEntity> type, Level level) {
		super(type, level);
		this.setNoGravity(true);
		this.noPhysics = true;
		this.setInvisible(true);
		this.setSilent(true);
	}

	public int formId() {
		return this.entityData.get(FORM_ID);
	}

	public void setFormId(int formId) {
		this.entityData.set(FORM_ID, formId);
	}

	@Override
	protected void defineSynchedData(SynchedEntityData.Builder builder) {
		super.defineSynchedData(builder);
		builder.define(FORM_ID, 0);
		builder.define(WIDTH, 0.6F);
		builder.define(HEIGHT, 1.8F);
	}

	public void setSize(float width, float height) {
		if (Math.abs(this.entityData.get(WIDTH) - width) > 0.01F || Math.abs(this.entityData.get(HEIGHT) - height) > 0.01F) {
			this.entityData.set(WIDTH, width);
			this.entityData.set(HEIGHT, height);
			this.refreshDimensions();
		}
	}

	@Override
	public void onSyncedDataUpdated(EntityDataAccessor<?> accessor) {
		super.onSyncedDataUpdated(accessor);
		if (WIDTH.equals(accessor) || HEIGHT.equals(accessor)) {
			this.refreshDimensions();
		}
	}

	@Override
	protected EntityDimensions getDefaultDimensions(Pose pose) {
		return EntityDimensions.scalable(this.entityData.get(WIDTH), this.entityData.get(HEIGHT));
	}

	@Override
	public void setHealth(float health) {
		// Capture vanilla's final damage after armor, effects and absorption,
		// before LivingEntity clamps HP to zero. Otherwise a nearly dead native
		// actor can never receive the full damage of a lethal sword/arrow hit.
		if (this.capturingNativeDamage && Float.isFinite(health)) {
			this.effectiveNativeDamage = Math.max(0.0F, this.damageHealthBefore - health);
		}
		super.setHealth(health);
	}

	@Override
	protected void actuallyHurt(ServerLevel level, DamageSource source, float dmg) {
		if (this.isInvulnerableTo(level, source) || dmg <= 0.0F) {
			return;
		}
		if (Kcd2Protocol.ENABLED) {
			// Vanilla actuallyHurt applies armor, Resistance, absorption, stats
			// and combat tracking. Preserve those and restore only the proxy HP:
			// the native Soul owns health and the real character's death.
			float before = this.getHealth();
			this.damageHealthBefore = before;
			this.effectiveNativeDamage = 0.0F;
			this.capturingNativeDamage = true;
			try {
				super.actuallyHurt(level, source, dmg);
				dmg = this.effectiveNativeDamage;
			} finally {
				this.capturingNativeDamage = false;
				this.setHealth(before);
			}
			if (dmg <= 0.0F) return;
		}
		this.pendingDamage += dmg;
		if (Kcd2Protocol.ENABLED && source.getEntity() instanceof net.minecraft.world.entity.Mob) {
			this.pendingFlags |= Kcd2Protocol.HIT_MOB_SOURCE;
		}
		if (source.getDirectEntity() instanceof Projectile) {
			this.pendingFlags |= Proto.HIT_PROJECTILE;
		}
		this.pendingWeapon = weaponClass(source);
		if (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE)) {
			this.pendingFlags |= Proto.HIT_FIRE;
		}
		if (Kcd2Protocol.ENABLED) {
			int flags = (source.getDirectEntity() instanceof Projectile ? Proto.HIT_PROJECTILE : 0)
				| (source.getEntity() instanceof net.minecraft.world.entity.Mob ? Kcd2Protocol.HIT_MOB_SOURCE : 0)
				| (source.is(net.minecraft.tags.DamageTypeTags.IS_FIRE) ? Proto.HIT_FIRE : 0);
			this.nativeHits.addLast(NativeAttackTrace.capture(this, source, dmg, flags, this.pendingWeapon));
		}
		this.hitThisTick = true;
		if (!Kcd2Protocol.ENABLED) this.getCombatTracker().recordDamage(source, dmg);
	}

	@Override
	public void heal(float amount) {
		if (!Kcd2Protocol.ENABLED || this.formId() == 0 || this.level().isClientSide()) {
			super.heal(amount);
			return;
		}
		float before = this.getHealth();
		super.heal(amount);
		this.pendingHealing += Math.max(0.0F, this.getHealth() - before);
		this.setHealth(before);
	}

	public float pendingHealing() { return this.pendingHealing; }
	public void clearHealing() { this.pendingHealing = 0.0F; }

	public void syncHostHealth(float fraction) {
		if (Kcd2Protocol.ENABLED && Float.isFinite(fraction)) {
			this.setHealth(Math.max(0.001F, Math.min(1.0F, fraction) * this.getMaxHealth()));
		}
	}

	@Override
	public void knockback(double power, double xd, double zd, DamageSource source, float damage, boolean comesFromEffect) {
		if (Kcd2Protocol.ENABLED) {
			// Let vanilla apply resistance and sprint/enchantment strength. Keep
			// the proxy at the native position and send the resulting velocity.
			this.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
			super.knockback(power, xd, zd, source, damage, comesFromEffect);
			var velocity = this.getDeltaMovement();
			double speed = Math.hypot(velocity.x, velocity.z);
			if (speed > this.pushStrength) {
				this.pushStrength = (float) speed;
				this.pushX = velocity.x / speed;
				this.pushZ = velocity.z / speed;
			}
			this.setDeltaMovement(net.minecraft.world.phys.Vec3.ZERO);
			this.hitThisTick = true;
			return;
		}
		// Skyrim owns this actor's position. Remember the strongest push for Skyrim's stagger:
		// Minecraft pushes towards -(xd, zd).
		double len = Math.sqrt(xd * xd + zd * zd);
		if (len > 1e-6 && power > this.pushStrength) {
			this.pushStrength = (float) power;
			this.pushX = -xd / len;
			this.pushZ = -zd / len;
		}
		this.hitThisTick = true;
	}

	/** Player.crit() was called on us this tick. */
	public void markCritical() {
		this.pendingFlags |= Proto.HIT_CRITICAL;
		if (!this.nativeHits.isEmpty() && this.nativeHits.peekLast().sourceKind() == 1) {
			this.nativeHits.addLast(this.nativeHits.removeLast().critical());
		}
	}

	public java.util.ArrayDeque<NativeAttackTrace> nativeHits() { return this.nativeHits; }

	/** Which kind of Skyrim weapon impact this hit should look and sound like. */
	private static int weaponClass(DamageSource source) {
		if (source.getDirectEntity() instanceof net.minecraft.world.entity.projectile.arrow.ThrownTrident) {
			return Proto.WEAPON_PIERCE;
		}
		if (source.getDirectEntity() instanceof Projectile) {
			return Proto.WEAPON_ARROW;
		}
		ItemStack weapon = source.getWeaponItem();
		if (weapon == null && source.getEntity() instanceof LivingEntity attacker) {
			weapon = attacker.getMainHandItem();
		}
		if (weapon == null || weapon.isEmpty()) {
			return Proto.WEAPON_UNARMED;
		}
		if (weapon.is(ItemTags.SWORDS)) {
			return Proto.WEAPON_BLADE;
		}
		if (weapon.is(ItemTags.AXES)) {
			return Proto.WEAPON_AXE;
		}
		if (weapon.is(Items.TRIDENT)) {
			return Proto.WEAPON_PIERCE;
		}
		return Proto.WEAPON_BLUNT;
	}

	/** Returns this tick's hit (damage, flags, push, weapon) and clears it; null if nothing hit us. */
	public float[] peekHit() {
		if (!this.hitThisTick) {
			return null;
		}
		float[] hit = { this.pendingDamage, (float) this.pushX, (float) this.pushZ, this.pushStrength, Float.intBitsToFloat(this.pendingFlags),
			Float.intBitsToFloat(this.pendingWeapon) };
		return hit;
	}

	public void clearHit() {
		this.nativeHits.clear();
		this.pendingDamage = 0.0F;
		this.pendingFlags = 0;
		this.pushX = this.pushZ = 0.0;
		this.pushStrength = 0.0F;
		this.hitThisTick = false;
	}

	@Override
	public void tick() {
		// Position and rotation come from Skyrim (SkyCombat); keep hurt timers and fire ticking.
		if (Kcd2Protocol.ENABLED) this.setDeltaMovement(Vec3.ZERO);
		this.baseTick();
		if (!Kcd2Protocol.ENABLED) {
			this.setHealth(this.getMaxHealth());
			return;
		}
		if (this.level().isClientSide() || !this.isAlive() || this.isRemoved()) return;
		// Native actors are positioned by their engine, so LivingEntity.aiStep/travel
		// is deliberately skipped. Run its vanilla contact path explicitly instead.
		// That includes the inside-effect collector (fire/lava/cactus/plates), not
		// just the old pressure-plate callback with a NOOP effect applier.
		Vec3 current = this.position();
		Vec3 previous = this.lastEnvironmentPosition;
		if (previous == null || previous.distanceToSqr(current) > 16) previous = current;
		this.lastEnvironmentPosition = current;
		boolean supported = this.level().getBlockCollisions(this,
			this.getBoundingBox().move(0, -0.06, 0)).iterator().hasNext();
		this.setOnGround(supported);
		this.applyEffectsFromBlocks(previous, current);
		// Fluid currents are calculated by Entity.baseTick. Export their horizontal
		// velocity through the same swept native push used by vanilla knockback.
		Vec3 velocity = this.getDeltaMovement();
		double speed = velocity.horizontalDistance();
		if (Double.isFinite(speed) && speed > 0.0001 && speed > this.pushStrength) {
			this.pushStrength = (float) speed;
			this.pushX = velocity.x / speed;
			this.pushZ = velocity.z / speed;
			this.hitThisTick = true;
		}
		this.setDeltaMovement(Vec3.ZERO);
	}

	@Override
	protected boolean isAffectedByBlocks() {
		// noPhysics prevents Minecraft travel from fighting native AI. It must
		// not make the corresponding real NPC immune to Minecraft hazards.
		return Kcd2Protocol.ENABLED ? !this.isRemoved() : super.isAffectedByBlocks();
	}

	@Override
	public boolean isPushable() {
		return false;
	}

	@Override
	protected void doPush(Entity entity) {
	}

	@Override
    public boolean canBeCollidedWith(@Nullable Entity other) {
        return Kcd2Protocol.ENABLED && this.isAlive() && !this.isRemoved()
            && !(other instanceof SkyrimActorEntity);
	}

	@Override
	public boolean shouldShowName() {
		return false;
	}

	@Override
	public boolean shouldBeSaved() {
		return false;
	}

	@Override
	protected @Nullable SoundEvent getHurtSound(DamageSource source) {
		return null; // Skyrim plays the NPC's own pain sounds
	}

	@Override
	protected @Nullable SoundEvent getDeathSound() {
		return null;
	}

	@Override
	public HumanoidArm getMainArm() {
		return HumanoidArm.RIGHT;
	}
}
