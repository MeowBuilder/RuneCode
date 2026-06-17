#include "stdafx.h"
#include "MapCollisionExporter.h"

#include <fstream>
#include <iomanip>

void MapCollisionExporter::Clear()
{
	_mapName.clear();
	_walkables.clear();
	_walls.clear();
}

void MapCollisionExporter::SetMapName(const std::string& mapName)
{
	_mapName = mapName;
}

void MapCollisionExporter::AddWalkableRect(
	float centerX,
	float centerZ,
	float halfX,
	float halfZ,
	float yawRad
)
{
	if (halfX <= 0.0f || halfZ <= 0.0f)
		return;

	MapCollisionRect rect;
	rect.centerX = centerX;
	rect.centerZ = centerZ;
	rect.halfX = halfX;
	rect.halfZ = halfZ;
	rect.yawRad = yawRad;

	_walkables.push_back(rect);
}

void MapCollisionExporter::AddWallRect(
	float centerX,
	float centerZ,
	float halfX,
	float halfZ,
	float yawRad
)
{
	if (halfX <= 0.0f || halfZ <= 0.0f)
		return;

	MapCollisionRect rect;
	rect.centerX = centerX;
	rect.centerZ = centerZ;
	rect.halfX = halfX;
	rect.halfZ = halfZ;
	rect.yawRad = yawRad;

	_walls.push_back(rect);
}

bool MapCollisionExporter::SaveToFile(const std::string& path) const
{
	std::ofstream file(path, std::ios::binary);
	if (!file.is_open())
		return false;

	file << "{\n";
	file << "  \"mapName\": \"" << _mapName << "\",\n";

	file << "  \"walkables\": [\n";
	for (std::size_t i = 0; i < _walkables.size(); ++i)
	{
		const MapCollisionRect& r = _walkables[i];

		file << "    { ";
		file << "\"centerX\": " << std::fixed << std::setprecision(4) << r.centerX << ", ";
		file << "\"centerZ\": " << std::fixed << std::setprecision(4) << r.centerZ << ", ";
		file << "\"halfX\": " << std::fixed << std::setprecision(4) << r.halfX << ", ";
		file << "\"halfZ\": " << std::fixed << std::setprecision(4) << r.halfZ << ", ";
		file << "\"yawRad\": " << std::fixed << std::setprecision(6) << r.yawRad;
		file << " }";

		if (i + 1 < _walkables.size())
			file << ",";

		file << "\n";
	}
	file << "  ],\n";

	file << "  \"walls\": [\n";
	for (std::size_t i = 0; i < _walls.size(); ++i)
	{
		const MapCollisionRect& r = _walls[i];

		file << "    { ";
		file << "\"centerX\": " << std::fixed << std::setprecision(4) << r.centerX << ", ";
		file << "\"centerZ\": " << std::fixed << std::setprecision(4) << r.centerZ << ", ";
		file << "\"halfX\": " << std::fixed << std::setprecision(4) << r.halfX << ", ";
		file << "\"halfZ\": " << std::fixed << std::setprecision(4) << r.halfZ << ", ";
		file << "\"yawRad\": " << std::fixed << std::setprecision(6) << r.yawRad;
		file << " }";

		if (i + 1 < _walls.size())
			file << ",";

		file << "\n";
	}
	file << "  ]\n";

	file << "}\n";
	return true;
}