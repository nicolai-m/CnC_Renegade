// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef HEADLESS_COLLISION_H
#define HEADLESS_COLLISION_H

#include <functional>
#include <memory>

#include "headless_level.h"

class AABoxClass;
class OBBoxClass;

class HeadlessCollision {
public:
    using FindAsset = std::function<std::optional<GameDataFile>(const std::string &)>;

    struct RayHit {
        std::size_t ObjectIndex;
        float Fraction;
        Vector3 Point;
        Vector3 Normal;
        std::uint32_t Surface;
        bool StartsInside;
    };

    struct BoxHit {
        std::size_t ObjectIndex;
        float Fraction;
        Vector3 Normal;
        std::optional<Vector3> ContactPoint;
        std::uint32_t Surface;
        bool StartsInside;
    };

    static HeadlessCollision Load(HeadlessLevel level, const FindAsset &find_asset,
        const std::optional<GameDataFile> &map_dependencies);

    const HeadlessLevel &Level() const { return Scene; }
    std::size_t Collider_Count() const { return ColliderCount; }
    std::size_t Incomplete_Object_Count() const { return IncompleteObjects; }

    // Cast against static W3D geometry, using a mask from coltype.h.
    std::optional<RayHit> Cast_Ray(const Vector3 &start, const Vector3 &end, unsigned mask) const;

    // Translate a box with its orthonormal basis held fixed, using a mask from coltype.h.
    // Zero displacement checks for penetration, which has no unique contact point.
    std::optional<BoxHit> Sweep_Box(const AABoxClass &box, const Vector3 &move, unsigned mask) const;
    std::optional<BoxHit> Sweep_Box(const OBBoxClass &box, const Vector3 &move, unsigned mask) const;

private:
    struct Geometry;
    struct Loader;

    struct Collider {
        std::shared_ptr<const Geometry> Shape;
        Matrix3D Transform;
        Matrix3D Inverse;

        AABoxClass Aligned_Box() const;
    };

    explicit HeadlessCollision(HeadlessLevel level);

    HeadlessLevel Scene;
    std::vector<std::vector<Collider>> Objects;
    std::size_t ColliderCount = 0;
    std::size_t IncompleteObjects = 0;
};

#endif // HEADLESS_COLLISION_H
