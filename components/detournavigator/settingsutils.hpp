﻿#ifndef OPENMW_COMPONENTS_DETOURNAVIGATOR_SETTINGSUTILS_H
#define OPENMW_COMPONENTS_DETOURNAVIGATOR_SETTINGSUTILS_H

#include "settings.hpp"
#include "tilebounds.hpp"
#include "tileposition.hpp"

#include <osg/Vec2f>
#include <osg/Vec3f>

#include <algorithm>
#include <cmath>

namespace DetourNavigator
{
    inline float toNavMeshCoordinates(const RecastSettings& settings, float value)
    {
        return value * settings.mRecastScaleFactor;
    }

    inline osg::Vec2f toNavMeshCoordinates(const RecastSettings& settings, osg::Vec2f position)
    {
        return position * settings.mRecastScaleFactor;
    }

    inline osg::Vec3f toNavMeshCoordinates(const RecastSettings& settings, osg::Vec3f position)
    {
        std::swap(position.y(), position.z());
        return position * settings.mRecastScaleFactor;
    }

    inline TileBounds toNavMeshCoordinates(const RecastSettings& settings, const TileBounds& value)
    {
        return TileBounds {
            toNavMeshCoordinates(settings, value.mMin),
            toNavMeshCoordinates(settings, value.mMax)
        };
    }

    inline osg::Vec3f fromNavMeshCoordinates(const RecastSettings& settings, osg::Vec3f position)
    {
        const auto factor = 1.0f / settings.mRecastScaleFactor;
        position *= factor;
        std::swap(position.y(), position.z());
        return position;
    }

    inline float getTileSize(const RecastSettings& settings)
    {
        return static_cast<float>(settings.mTileSize) * settings.mCellSize;
    }

    inline TilePosition getTilePosition(const RecastSettings& settings, const osg::Vec3f& position)
    {
        return TilePosition(
            static_cast<int>(std::floor(position.x() / getTileSize(settings))),
            static_cast<int>(std::floor(position.z() / getTileSize(settings)))
        );
    }

    inline TileBounds makeTileBounds(const RecastSettings& settings, const TilePosition& tilePosition)
    {
        return TileBounds {
            osg::Vec2f(tilePosition.x(), tilePosition.y()) * getTileSize(settings),
            osg::Vec2f(tilePosition.x() + 1, tilePosition.y() + 1) * getTileSize(settings),
        };
    }

    inline float getBorderSize(const RecastSettings& settings)
    {
        return static_cast<float>(settings.mBorderSize) * settings.mCellSize;
    }

    inline float getRealTileSize(const RecastSettings& settings)
    {
        return settings.mTileSize * settings.mCellSize / settings.mRecastScaleFactor;
    }

    inline float getMaxNavmeshAreaRadius(const Settings& settings)
    {
        return std::floor(std::sqrt(settings.mMaxTilesNumber / osg::PI)) - 1;
    }

    inline TileBounds makeRealTileBoundsWithBorder(const RecastSettings& settings, const TilePosition& tilePosition)
    {
        TileBounds result = makeTileBounds(settings, tilePosition);
        const float border = getBorderSize(settings);
        result.mMin -= osg::Vec2f(border, border);
        result.mMax += osg::Vec2f(border, border);
        result.mMin /= settings.mRecastScaleFactor;
        result.mMax /= settings.mRecastScaleFactor;
        return result;
    }
}

#endif
