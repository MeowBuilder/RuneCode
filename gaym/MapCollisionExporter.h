#pragma once

#include <string>
#include <vector>
#include <cstddef>
#include "MapCollisionCore.h"

class MapCollisionExporter
{
public:
	void Clear();

	void SetMapName(const std::string& mapName);

	void AddWalkableRect(
		float centerX,
		float centerZ,
		float halfX,
		float halfZ,
		float yawRad = 0.0f
	);

	void AddWallRect(
		float centerX,
		float centerZ,
		float halfX,
		float halfZ,
		float yawRad = 0.0f
	);

	bool SaveToFile(const std::string& path) const;

	std::size_t GetWalkableCount() const { return _walkables.size(); }
	std::size_t GetWallCount() const { return _walls.size(); }

private:
	std::string _mapName;
	std::vector<MapCollisionRect> _walkables;
	std::vector<MapCollisionRect> _walls;
};