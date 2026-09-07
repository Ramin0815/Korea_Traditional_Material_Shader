using UnityEngine;

[ExecuteAlways]
[DisallowMultipleComponent]
public sealed class GatGeometryBinder : MonoBehaviour
{
    private static readonly int GatLocalYBoundsId = Shader.PropertyToID("_GatLocalYBounds");

    private Renderer cachedRenderer;
    private Mesh cachedMesh;
    private Bounds cachedBounds;
    private bool hasCachedBounds;
    private MaterialPropertyBlock propertyBlock;

    private void OnEnable()
    {
        RefreshBounds(force: true);
    }

    private void OnValidate()
    {
        RefreshBounds(force: true);
    }

    private void LateUpdate()
    {
        RefreshBounds(force: false);
    }

    private void RefreshBounds(bool force)
    {
        Renderer renderer = GetComponent<Renderer>();
        if (renderer == null || !TryGetLocalBounds(renderer, out Mesh mesh, out Bounds bounds))
            return;

        if (!force && renderer == cachedRenderer && mesh == cachedMesh
                   && hasCachedBounds && bounds.Equals(cachedBounds))
            return;

        if (propertyBlock == null)
            propertyBlock = new MaterialPropertyBlock();

        renderer.GetPropertyBlock(propertyBlock);
        propertyBlock.SetVector(GatLocalYBoundsId,
            new Vector4(bounds.min.y, bounds.max.y, 0.0f, 0.0f));
        renderer.SetPropertyBlock(propertyBlock);

        cachedRenderer = renderer;
        cachedMesh = mesh;
        cachedBounds = bounds;
        hasCachedBounds = true;
    }

    private static bool TryGetLocalBounds(Renderer renderer, out Mesh mesh, out Bounds bounds)
    {
        SkinnedMeshRenderer skinnedRenderer = renderer as SkinnedMeshRenderer;
        if (skinnedRenderer != null)
        {
            mesh = skinnedRenderer.sharedMesh;
            bounds = skinnedRenderer.localBounds;
            return mesh != null;
        }

        MeshFilter meshFilter = renderer.GetComponent<MeshFilter>();
        mesh = meshFilter != null ? meshFilter.sharedMesh : null;
        if (mesh == null)
        {
            bounds = default(Bounds);
            return false;
        }

        bounds = mesh.bounds;
        return true;
    }
}
