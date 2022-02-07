﻿using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Threading.Tasks;
using WorldsEngine;
using WorldsEngine.Math;
using WorldsEngine.Audio;

namespace Game.Combat
{
    [Component]
    [EditorFriendlyName("C# Damaging Projectile")]
    public class DamagingProjectile : Component, IStartListener, ICollisionHandler
    {
        public double Damage = 5.0;
        public double CreationTime = 0.0;
        public int BounceCount = 0;
        public Entity Attacker = Entity.Null;
        public AmmoType ProjectileType;

        public void Start()
        {
            CreationTime = Time.CurrentTime;
        }

        public void OnCollision(ref PhysicsContactInfo contactInfo)
        {
            Audio.PlayOneShotEvent("event:/Weapons/Laser Hit", contactInfo.AverageContactPoint);
            bool hitProjectile = Registry.HasComponent<DamagingProjectile>(contactInfo.OtherEntity);

            if (BounceCount > 0 || hitProjectile)
            {
                if (!hitProjectile)
                    BounceCount--;

                var dpa = Registry.GetComponent<DynamicPhysicsActor>(Entity);

                dpa.Velocity = Vector3.Reflect(dpa.Velocity, contactInfo.Normal);

                var pose = dpa.Pose;
                pose.Rotation = Quaternion.SafeLookAt(dpa.Velocity.Normalized);
                dpa.Pose = pose;
            }
            else
            {
                if (Registry.TryGetComponent<ProjectilePrism>(contactInfo.OtherEntity, out var projectilePrism))
                {
                    projectilePrism.RefractProjectile(Entity);
                }

                Registry.DestroyNext(Entity);
            }

            if (!Registry.HasComponent<HealthComponent>(contactInfo.OtherEntity)) return;

            var health = Registry.GetComponent<HealthComponent>(contactInfo.OtherEntity);

            health.Damage(Damage, Attacker);
        }
    }

    public class ProjectileCleanupSystem : ISystem
    {
        public void OnSimulate()
        {
            foreach (Entity projectileEntity in Registry.View<DamagingProjectile>())
            {
                var projectile = Registry.GetComponent<DamagingProjectile>(projectileEntity);

                if (Time.CurrentTime - projectile.CreationTime > 5.0)
                {
                    Registry.DestroyNext(projectileEntity);
                }
            }
        }
    }
}
