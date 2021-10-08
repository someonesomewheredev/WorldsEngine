using WorldsEngine;
using WorldsEngine.Math;
using Game.Combat;
using System.Collections.Generic;

namespace Game
{
    [Component]
    public class ExplodeOnDeath : IStartListener
    {

        public void Start(Entity e)
        {
            var hc = Registry.GetComponent<HealthComponent>(e);
            hc.OnDeath += OnDeath;
        }

        public Entity SpawnCube(Transform transform)
        {
            var entity = Registry.Create();
            Registry.SetTransform(entity, transform);

            var worldObject = Registry.AddComponent<WorldObject>(entity);
            worldObject.Mesh = AssetDB.PathToId("Models/cube.wmdl");

            var dpa = Registry.AddComponent<DynamicPhysicsActor>(entity);
            dpa.Mass = 0.25f;

            List<PhysicsShape> physicsShapes = new() {
                new BoxPhysicsShape(Vector3.One)
            };
            dpa.SetPhysicsShapes(physicsShapes);

            return entity;
        }

        private void OnDeath(Entity e)
        {
            var transform = Registry.GetTransform(e);
            transform.Scale = new Vector3(0.1f);

            async void DestroyAfter(int ms, Entity entity)
            {
                await System.Threading.Tasks.Task.Delay(ms);
                Registry.Destroy(entity);
            }

            for (int x = -2; x < 2; x++) {
            for (int y = -2; y < 2; y++) {
                var spawnTransform = transform;
                var pos = spawnTransform.Position;
                pos += new Vector3(x * 0.1f, y * 0.1f, 0.0f);
                spawnTransform.Position = pos;
                DestroyAfter(5000, SpawnCube(spawnTransform));
            }
            }

            Registry.Destroy(e);
        }
    }
}