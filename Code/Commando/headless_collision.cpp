// SPDX-License-Identifier: GPL-3.0-or-later
#include "headless_collision.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "aabox.h"
#include "bounded_chunk_reader.h"
#include "colmath.h"
#include "coltype.h"
#include "lineseg.h"
#include "obbox.h"
#include "quat.h"
#include "tri.h"
#include "w3d_file.h"

namespace {

static_assert(sizeof(W3dMeshHeader3Struct) == 116 && sizeof(W3dTriStruct) == 32);
static_assert(sizeof(W3dVectorStruct) == 12 && sizeof(W3dBoxStruct) == 68);
static_assert(sizeof(W3dHierarchyStruct) == 36 && sizeof(W3dPivotStruct) == 60);
static_assert(sizeof(W3dHModelHeaderStruct) == 40 && sizeof(W3dHModelNodeStruct) == 18);
static_assert(sizeof(W3dHLodHeaderStruct) == 40 && sizeof(W3dHLodSubObjectStruct) == 36);
static_assert(sizeof(W3dEmitterHeaderStruct) == 20);

using ChunkReader = BoundedChunkReader;

std::string Fold_Name(std::string name)
{
    for (char &letter : name) {
        if (letter >= 'A' && letter <= 'Z') {
            letter += 'a' - 'A';
        }
    }
    return name;
}

template<std::size_t N> std::string Name(const char (&field)[N])
{
    return Fold_Name(std::string(field, std::find(field, field + N, '\0')));
}

Vector3 To_Vector(const W3dVectorStruct &value)
{
    return Vector3(value.X, value.Y, value.Z);
}

bool Finite(const Vector3 &value)
{
    return std::isfinite(value.X) && std::isfinite(value.Y) && std::isfinite(value.Z);
}

struct ModelPart {
    std::string Name;
    uint32 Bone;
};

std::vector<ModelPart> Read_Subobjects(ChunkReader &reader)
{
    std::optional<W3dHLodArrayHeaderStruct> header;
    std::vector<ModelPart> objects;
    reader.Each_Chunk([&](uint32 id) {
        switch (id) {
        case W3D_CHUNK_HLOD_SUB_OBJECT_ARRAY_HEADER:
            reader.Require(!header, "Duplicate HLOD array header");
            header = reader.Value<W3dHLodArrayHeaderStruct>();
            break;
        case W3D_CHUNK_HLOD_SUB_OBJECT: {
            const auto object = reader.Value<W3dHLodSubObjectStruct>();
            objects.push_back({Name(object.Name), object.BoneIndex});
            break;
        }
        }
    });
    reader.Require(header && header->ModelCount == objects.size(), "Invalid HLOD subobject count");
    return objects;
}

Vector3 Sweep_Extent(const OBBoxClass &box)
{
    Vector3 extent;
    box.Compute_Axis_Aligned_Extent(&extent);
    // Retain the culling margin used by OBBoxCollisionTestClass.
    return extent + Vector3(0.01f, 0.01f, 0.01f);
}

bool Prefer_Hit(const CastResultStruct &hit, const CastResultStruct &best, const Vector3 &move)
{
    return hit.Fraction < best.Fraction || (hit.Fraction == best.Fraction &&
        Vector3::Dot_Product(hit.Normal, move) < Vector3::Dot_Product(best.Normal, move));
}

} // namespace

struct HeadlessCollision::Geometry {
    struct Triangle {
        uint32 Vertices[3];
        Vector3 Normal;
        uint8 Surface;
        bool HasArea;
    };

    enum class Type { Mesh, AlignedBox, OrientedBox };
    Type Kind = Type::Mesh;
    unsigned Mask = COLLISION_TYPE_ALL;
    AABoxClass Bounds;
    std::vector<Vector3> Vertices;
    std::vector<Triangle> Triangles;

    TriClass Get_Triangle(const Triangle &triangle) const;
    bool Sweep_Mesh(const OBBoxClass &box, const Vector3 &move, CastResultStruct &best) const;
};

TriClass HeadlessCollision::Geometry::Get_Triangle(const Triangle &triangle) const
{
    TriClass tri;
    tri.V[0] = &Vertices[triangle.Vertices[0]];
    tri.V[1] = &Vertices[triangle.Vertices[1]];
    tri.V[2] = &Vertices[triangle.Vertices[2]];
    tri.N = &triangle.Normal;
    return tri;
}

bool HeadlessCollision::Geometry::Sweep_Mesh(const OBBoxClass &box, const Vector3 &move,
    CastResultStruct &best) const
{
    CastResultStruct bounds_hit;
    if (!CollisionMath::Collide(LineSegClass(box.Center, box.Center + move),
            AABoxClass(Bounds.Center, Bounds.Extent + Sweep_Extent(box)), &bounds_hit)) {
        return false;
    }

    const Vector3 stationary(0, 0, 0);
    for (const auto &triangle : Triangles) {
        if (!triangle.HasArea) {
            continue;
        }
        CastResultStruct hit;
        hit.ComputeContactPoint = true;
        if (CollisionMath::Collide(box, move, Get_Triangle(triangle), stationary, &hit)) {
            hit.SurfaceType = triangle.Surface;
            if (hit.StartBad) {
                best = hit;
                return true;
            }
            if (Prefer_Hit(hit, best, move)) {
                best = hit;
            }
        }
    }
    return best.Fraction < 1.0f;
}

AABoxClass HeadlessCollision::Collider::Aligned_Box() const
{
    // AABoxRenderObjClass keeps the center offset and extents world-aligned.
    return AABoxClass(Transform.Get_Translation() + Shape->Bounds.Center, Shape->Bounds.Extent);
}

struct HeadlessCollision::Loader {
    struct Model {
        std::shared_ptr<Geometry> Shape;
        std::string Hierarchy;
        std::vector<ModelPart> Parts;
        bool Supported = true;
    };

    const FindAsset &Find;
    const std::optional<GameDataFile> &Dependencies;
    std::vector<std::string> Ancestors;
    std::unordered_set<std::string> LoadedFiles;
    std::unordered_map<std::string, Model> Models;
    std::unordered_map<std::string, std::vector<Matrix3D>> Hierarchies;
    bool LoadedDependencies = false;

    Loader(const FindAsset &find, const std::optional<GameDataFile> &dependencies)
        : Find(find), Dependencies(dependencies) {}

    void Read_Mesh(ChunkReader &reader)
    {
        std::optional<W3dMeshHeader3Struct> header;
        std::optional<std::vector<W3dVectorStruct>> vertices;
        std::optional<std::vector<W3dTriStruct>> triangles;
        reader.Each_Chunk([&](uint32 id) {
            switch (id) {
            case W3D_CHUNK_MESH_HEADER3:
                reader.Require(!header, "Duplicate W3D mesh header");
                header = reader.Value<W3dMeshHeader3Struct>();
                break;
            case W3D_CHUNK_VERTICES:
                reader.Require(!vertices, "Duplicate W3D vertex array");
                vertices = reader.Array<W3dVectorStruct>();
                break;
            case W3D_CHUNK_TRIANGLES:
                reader.Require(!triangles, "Duplicate W3D triangle array");
                triangles = reader.Array<W3dTriStruct>();
                break;
            }
        });
        if (!header) {
            return;
        }
        const auto container = Name(header->ContainerName);
        const auto mesh = Name(header->MeshName);
        const auto name = container.empty() ? mesh : container + "." + mesh;
        const auto attributes = header->Attributes;
        const auto saved_mask = attributes & W3D_MESH_FLAG_COLLISION_TYPE_MASK;
        const bool normal_mesh = (attributes & (W3D_MESH_FLAG_GEOMETRY_TYPE_MASK |
            W3D_MESH_FLAG_SKIN | W3D_MESH_FLAG_ALIGNED)) == 0;
        if (!normal_mesh) {
            Model model;
            model.Supported = saved_mask == 0;
            Models.try_emplace(name, std::move(model));
            return;
        }

        auto shape = std::make_shared<Geometry>();
        shape->Mask |= (saved_mask >> W3D_MESH_FLAG_COLLISION_TYPE_SHIFT) << 1;
        reader.Require((vertices ? vertices->size() : 0) == header->NumVertices &&
            (triangles ? triangles->size() : 0) == header->NumTris &&
            (header->NumTris == 0 || header->NumVertices >= 3), "Invalid W3D mesh counts");
        if (header->NumTris == 0) {
            Models.try_emplace(name);
            return;
        }
        shape->Vertices.reserve(vertices->size());
        Vector3 minimum = To_Vector(vertices->front());
        Vector3 maximum = minimum;
        for (const auto &value : *vertices) {
            const auto vertex = To_Vector(value);
            reader.Require(Finite(vertex), "Invalid W3D vertex");
            shape->Vertices.push_back(vertex);
            minimum.Update_Min(vertex);
            maximum.Update_Max(vertex);
        }
        shape->Bounds = AABoxClass((maximum + minimum) * 0.5f, (maximum - minimum) * 0.5f);
        shape->Triangles.reserve(triangles->size());
        for (const auto &tri : *triangles) {
            reader.Require(tri.Vindex[0] < shape->Vertices.size() && tri.Vindex[1] < shape->Vertices.size() &&
                tri.Vindex[2] < shape->Vertices.size() && Finite(To_Vector(tri.Normal)),
                "Invalid W3D triangle");
            const auto &origin = shape->Vertices[tri.Vindex[0]];
            const auto edge1 = shape->Vertices[tri.Vindex[1]] - origin;
            const auto edge2 = shape->Vertices[tri.Vindex[2]] - origin;
            const bool has_area = Vector3::Cross_Product(edge1, edge2).Length2() > 0.0f;
            shape->Triangles.push_back({{tri.Vindex[0], tri.Vindex[1], tri.Vindex[2]},
                To_Vector(tri.Normal), static_cast<uint8>(tri.Attributes), has_area});
        }
        Model model;
        model.Shape = std::move(shape);
        Models.try_emplace(name, std::move(model));
    }

    void Read_Box(ChunkReader &reader)
    {
        const auto box = reader.Value<W3dBoxStruct>();
        auto shape = std::make_shared<Geometry>();
        shape->Kind = box.Attributes & W3D_BOX_ATTRIBUTE_ORIENTED ?
            Geometry::Type::OrientedBox : Geometry::Type::AlignedBox;
        shape->Mask |= ((box.Attributes & W3D_BOX_ATTRIBUTE_COLLISION_TYPE_MASK) >>
            W3D_BOX_ATTRIBUTE_COLLISION_TYPE_SHIFT) << 1;
        shape->Bounds = AABoxClass(To_Vector(box.Center), To_Vector(box.Extent));
        reader.Require(Finite(shape->Bounds.Center) && Finite(shape->Bounds.Extent) &&
            box.Extent.X >= 0 && box.Extent.Y >= 0 && box.Extent.Z >= 0, "Invalid W3D collision box");
        Model model;
        model.Shape = std::move(shape);
        Models.try_emplace(Name(box.Name), std::move(model));
    }

    void Read_Hierarchy(ChunkReader &reader)
    {
        std::optional<W3dHierarchyStruct> header;
        std::optional<std::vector<W3dPivotStruct>> pivots;
        reader.Each_Chunk([&](uint32 id) {
            switch (id) {
            case W3D_CHUNK_HIERARCHY_HEADER:
                reader.Require(!header, "Duplicate W3D hierarchy header");
                header = reader.Value<W3dHierarchyStruct>();
                break;
            case W3D_CHUNK_PIVOTS:
                reader.Require(!pivots, "Duplicate W3D pivot array");
                pivots = reader.Array<W3dPivotStruct>();
                break;
            }
        });
        reader.Require(header && pivots && header->NumPivots == pivots->size() && !pivots->empty(),
            "Invalid W3D hierarchy counts");
        const bool add_root = header->Version < W3D_MAKE_VERSION(3, 0);
        // HTreeClass::Base_Update uses the instance transform for pivot zero.
        std::vector<Matrix3D> pose{Matrix3D(true)};
        pose.reserve(pivots->size() + add_root);
        for (std::size_t i = add_root ? 0 : 1; i < pivots->size(); ++i) {
            const auto &pivot = (*pivots)[i];
            const auto parent = pivot.ParentIdx + (add_root ? 1u : 0u);
            const auto &rot = pivot.Rotation.Q;
            const auto position = To_Vector(pivot.Translation);
            reader.Require(parent < pose.size() && Finite(position) && std::isfinite(rot[0]) &&
                std::isfinite(rot[1]) && std::isfinite(rot[2]) && std::isfinite(rot[3]), "Invalid W3D pivot");
            pose.push_back(pose[parent] * Matrix3D(Quaternion(rot[0], rot[1], rot[2], rot[3]), position));
        }
        Hierarchies.try_emplace(Name(header->Name), std::move(pose));
    }

    void Read_Hmodel(ChunkReader &reader)
    {
        std::optional<W3dHModelHeaderStruct> header;
        std::vector<W3dHModelNodeStruct> nodes;
        reader.Each_Chunk([&](uint32 id) {
            switch (id) {
            case W3D_CHUNK_HMODEL_HEADER:
                reader.Require(!header, "Duplicate HModel header");
                header = reader.Value<W3dHModelHeaderStruct>();
                break;
            case W3D_CHUNK_NODE:
            case W3D_CHUNK_COLLISION_NODE:
            case W3D_CHUNK_SKIN_NODE:
                nodes.push_back(reader.Value<W3dHModelNodeStruct>());
                break;
            }
        });
        reader.Require(header && header->NumConnections == nodes.size(), "Invalid HModel node count");
        const auto name = Name(header->Name);
        const bool add_root = header->Version < W3D_MAKE_VERSION(3, 0);
        Model model;
        model.Hierarchy = Name(header->HierarchyName);
        model.Parts.reserve(nodes.size());
        for (const auto &node : nodes) {
            uint32 bone = node.PivotIdx;
            if (add_root) {
                bone = bone == 0xffffu ? 0 : bone + 1;
            }
            model.Parts.push_back({name + "." + Name(node.RenderObjName), bone});
        }
        Models.try_emplace(name, std::move(model));
    }

    void Read_Hlod(ChunkReader &reader)
    {
        std::optional<W3dHLodHeaderStruct> header;
        unsigned lod_count = 0;
        std::vector<ModelPart> top_lod;
        std::optional<std::vector<ModelPart>> aggregates;
        reader.Each_Chunk([&](uint32 id) {
            switch (id) {
            case W3D_CHUNK_HLOD_HEADER:
                reader.Require(!header, "Duplicate HLOD header");
                header = reader.Value<W3dHLodHeaderStruct>();
                break;
            case W3D_CHUNK_HLOD_LOD_ARRAY:
                ++lod_count;
                top_lod = Read_Subobjects(reader);
                break;
            case W3D_CHUNK_HLOD_AGGREGATE_ARRAY:
                reader.Require(!aggregates, "Duplicate HLOD aggregate array");
                aggregates = Read_Subobjects(reader);
                break;
            }
        });
        reader.Require(header && header->LodCount == lod_count && lod_count > 0, "Invalid HLOD count");
        if (aggregates) {
            top_lod.insert(top_lod.end(), aggregates->begin(), aggregates->end());
        }
        Model model;
        model.Hierarchy = Name(header->HierarchyName);
        model.Parts = std::move(top_lod);
        Models.try_emplace(Name(header->Name), std::move(model));
    }

    void Load_File(const std::string &name)
    {
        if (name == ".w3d" || name.find_first_of("/\\:") != std::string::npos ||
            !LoadedFiles.insert(name).second) {
            return;
        }
        if (const auto file = Find(name)) {
            ChunkReader reader(*file);
            reader.Each_Chunk([&](uint32 id) {
                switch (id) {
                case W3D_CHUNK_MESH: Read_Mesh(reader); break;
                case W3D_CHUNK_BOX: Read_Box(reader); break;
                case W3D_CHUNK_HIERARCHY: Read_Hierarchy(reader); break;
                case W3D_CHUNK_HMODEL: Read_Hmodel(reader); break;
                case W3D_CHUNK_HLOD: Read_Hlod(reader); break;
                case W3D_CHUNK_EMITTER:
                    // Particle emitters can be HLOD parts and have no ray geometry.
                    reader.In_Chunk(W3D_CHUNK_EMITTER_HEADER, [&] {
                        Models.try_emplace(Name(reader.Value<W3dEmitterHeaderStruct>().Name));
                    });
                    break;
                }
            });
        }
    }

    void Load_Dependencies()
    {
        if (LoadedDependencies) {
            return;
        }
        LoadedDependencies = true;
        if (Dependencies) {
            // AssetDependencyManager's CHUNKID_FILE_LIST and VARID_ASSET_FILENAME.
            ChunkReader reader(*Dependencies);
            reader.In_Chunk(0x04020527, [&] {
                reader.Each_Micro([&](uint32 id) {
                    if (id == 1) {
                        const auto path = Fold_Name(reader.Read_String());
                        const auto name = path.substr(path.find_last_of("/\\") + 1);
                        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".w3d") == 0) {
                            Load_File(name);
                        }
                    }
                });
            });
        }
    }

    bool Add_Model(const std::string &name, const Matrix3D &transform, std::vector<Collider> &colliders)
    {
        // A W3D can reference another HLOD, including a broken cyclic reference.
        if (Ancestors.size() == 32 || std::find(Ancestors.begin(), Ancestors.end(), name) != Ancestors.end()) {
            return false;
        }
        if (name.empty() || name == "null") {
            return true;
        }
        if (Models.find(name) == Models.end()) {
            Load_File(name.substr(0, name.find('.')) + ".w3d");
            if (Models.find(name) == Models.end()) {
                Load_Dependencies();
            }
        }
        const auto found = Models.find(name);
        if (found == Models.end() || !found->second.Supported) {
            return false;
        }

        // References to unordered_map values remain valid as child loading adds more assets.
        const auto &model = found->second;
        if (model.Shape) {
            Matrix3D inverse;
            transform.Get_Orthogonal_Inverse(inverse);
            colliders.push_back({model.Shape, transform, inverse});
        }
        if (model.Parts.empty()) {
            return true;
        }

        const std::vector<Matrix3D> *pose = nullptr;
        if (!model.Hierarchy.empty()) {
            if (Hierarchies.find(model.Hierarchy) == Hierarchies.end()) {
                Load_File(model.Hierarchy + ".w3d");
                if (Hierarchies.find(model.Hierarchy) == Hierarchies.end()) {
                    Load_Dependencies();
                }
            }
            const auto hierarchy = Hierarchies.find(model.Hierarchy);
            if (hierarchy != Hierarchies.end()) {
                pose = &hierarchy->second;
            }
        }
        Ancestors.push_back(name);
        bool complete = true;
        for (const auto &part : model.Parts) {
            if (part.Bone == 0) {
                complete &= Add_Model(part.Name, transform, colliders);
            } else if (pose && part.Bone < pose->size()) {
                complete &= Add_Model(part.Name, transform * (*pose)[part.Bone], colliders);
            } else {
                complete = false;
            }
        }
        Ancestors.pop_back();
        return complete;
    }
};

HeadlessCollision::HeadlessCollision(HeadlessLevel level)
    : Scene(std::move(level)), Objects(Scene.Static_Objects().size()), BuildingOwners(Objects.size())
{
}

HeadlessCollision HeadlessCollision::Load(HeadlessLevel level, const FindAsset &find_asset,
    const std::optional<GameDataFile> &map_dependencies)
{
    HeadlessCollision collision(std::move(level));
    Loader loader(find_asset, map_dependencies);
    const auto &objects = collision.Scene.Static_Objects();
    const auto &buildings = collision.Scene.Buildings();
    std::vector<std::string> prefixes;
    for (const auto &building : buildings) {
        prefixes.push_back(Fold_Name(building.MeshPrefix));
    }
    for (std::size_t i = 0; i < objects.size(); ++i) {
        const auto &object = objects[i];
        const auto name = Fold_Name(object.AssetName);
        const auto &colliders = collision.Objects[i];
        if (!loader.Add_Model(name, object.Transform, collision.Objects[i])) {
            ++collision.IncompleteObjects;
        }
        collision.ColliderCount += colliders.size();
        if (colliders.empty() || (!object.BuildingAggregate && name.find_first_of("#^") == std::string::npos)) {
            continue;
        }
        // The original collects named parts by their world-space bounds, including meshes at the origin.
        const auto world_box = [](const Collider &collider) {
            if (collider.Shape->Kind == Geometry::Type::AlignedBox) {
                return collider.Aligned_Box();
            }
            auto bounds = collider.Shape->Bounds;
            bounds.Transform(collider.Transform);
            return bounds;
        };
        auto bounds = world_box(colliders.front());
        for (std::size_t j = 1; j < colliders.size(); ++j) {
            bounds.Add_Box(world_box(colliders[j]));
        }
        const auto separator = name.find('.');
        const auto part = std::string_view(name).substr(separator == std::string::npos ? 0 : separator + 1);
        float closest = std::numeric_limits<float>::infinity();
        for (std::size_t j = 0; j < buildings.size(); ++j) {
            const auto &building = buildings[j];
            if (prefixes[j].empty() || part.compare(0, prefixes[j].size(), prefixes[j]) != 0) {
                continue;
            }
            const float distance = (bounds.Center - building.CollectionCenter).Length2();
            if (distance <= building.CollectionRadius * building.CollectionRadius && distance < closest) {
                closest = distance;
                collision.BuildingOwners[i] = j;
            }
        }
    }
    return collision;
}

std::optional<HeadlessCollision::RayHit> HeadlessCollision::Cast_Ray(const Vector3 &start, const Vector3 &end,
    unsigned mask) const
{
    const LineSegClass world_ray(start, end);
    float nearest = 1.0f;
    std::optional<RayHit> hit;
    for (const auto index : Scene.Find_Static_Candidates(start, end)) {
        for (const auto &collider : Objects[index]) {
            const auto &shape = *collider.Shape;
            if (!(shape.Mask & mask)) {
                continue;
            }
            const bool aligned = shape.Kind == Geometry::Type::AlignedBox;
            const LineSegClass ray = aligned ? world_ray :
                LineSegClass(collider.Inverse * start, collider.Inverse * end);
            CastResultStruct result;
            result.Fraction = nearest;
            if (shape.Kind == Geometry::Type::Mesh) {
                if (!CollisionMath::Collide(ray, shape.Bounds, &result)) {
                    continue;
                }
                result.Reset();
                result.Fraction = nearest;
                for (const auto &triangle : shape.Triangles) {
                    if (CollisionMath::Collide(ray, shape.Get_Triangle(triangle), &result)) {
                        result.SurfaceType = triangle.Surface;
                    }
                }
                if (result.Fraction >= nearest) {
                    continue;
                }
            } else {
                const auto box = aligned ? collider.Aligned_Box() : shape.Bounds;
                if (!CollisionMath::Collide(ray, box, &result)) {
                    continue;
                }
            }
            if (result.StartBad) {
                return RayHit{index, 0, start, Vector3(0, 0, 0), result.SurfaceType, true};
            }
            nearest = result.Fraction;
            Vector3 normal = result.Normal;
            if (!aligned) {
                Matrix3D::Rotate_Vector(collider.Transform, result.Normal, &normal);
            }
            hit = RayHit{index, result.Fraction, start + result.Fraction * world_ray.Get_DP(),
                normal, result.SurfaceType, false};
        }
    }
    return hit;
}

std::optional<HeadlessCollision::BoxHit> HeadlessCollision::Sweep_Box(const AABoxClass &box, const Vector3 &move,
    unsigned mask) const
{
    return Sweep_Box(OBBoxClass(box.Center, box.Extent), move, mask);
}

std::optional<HeadlessCollision::BoxHit> HeadlessCollision::Sweep_Box(const OBBoxClass &box, const Vector3 &move,
    unsigned mask) const
{
    const Vector3 stationary(0, 0, 0);
    CastResultStruct best;
    std::optional<BoxHit> hit;
    for (const auto index : Scene.Find_Static_Candidates(box.Center, box.Center + move, Sweep_Extent(box))) {
        for (const auto &collider : Objects[index]) {
            const auto &shape = *collider.Shape;
            if (!(shape.Mask & mask)) {
                continue;
            }
            CastResultStruct result;
            result.ComputeContactPoint = true;
            if (shape.Kind == Geometry::Type::AlignedBox) {
                if (!CollisionMath::Collide(box, move, collider.Aligned_Box(), stationary, &result)) {
                    continue;
                }
            } else {
                OBBoxClass local_box;
                OBBoxClass::Transform(collider.Inverse, box, &local_box);
                Vector3 local_move;
                Matrix3D::Rotate_Vector(collider.Inverse, move, &local_move);
                const bool collided = shape.Kind == Geometry::Type::Mesh ?
                    shape.Sweep_Mesh(local_box, local_move, result) :
                    CollisionMath::Collide(local_box, local_move, shape.Bounds, stationary, &result);
                if (!collided) {
                    continue;
                }
                Matrix3D::Rotate_Vector(collider.Transform, result.Normal, &result.Normal);
                result.ContactPoint = collider.Transform * result.ContactPoint;
            }
            if (result.StartBad) {
                return BoxHit{index, 0, result.Normal, std::nullopt, result.SurfaceType, true};
            }
            if (Prefer_Hit(result, best, move)) {
                best = result;
                hit = BoxHit{index, result.Fraction, result.Normal, result.ContactPoint, result.SurfaceType, false};
            }
        }
    }
    return hit;
}
