#pragma once

#include <string>
#include <vector>
#include <cstddef>

struct MapCollisionRect
{
	float centerX = 0.0f;
	float centerZ = 0.0f;

	float halfX = 0.0f;
	float halfZ = 0.0f;

	// y축 회전값. 단위는 radian.
	float yawRad = 0.0f;
};

struct CollisionMoveResult
{
	float x = 0.0f;
	float z = 0.0f;

	// 중간에 막혔거나 보정되었으면 true
	bool blocked = false;
};

class MapCollisionCore
{
public:
	void Clear();

	bool LoadFromFile(const std::string& path);
	bool LoadFromJsonText(const std::string& text);

	void AddWalkableRect(const MapCollisionRect& rect);
	void AddWallRect(const MapCollisionRect& rect);

	bool CanStand(float x, float z, float radius) const;

	CollisionMoveResult ResolveMove(
		float fromX,
		float fromZ,
		float toX,
		float toZ,
		float radius
	) const;

	std::size_t GetWalkableCount() const { return _walkables.size(); }
	std::size_t GetWallCount() const { return _walls.size(); }

private:
	bool IsPointInsideRect(float x, float z, const MapCollisionRect& rect, float margin = 0.0f) const;
	bool IsCircleOnWalkable(float x, float z, float radius) const;
	bool IsCircleIntersectWall(float x, float z, float radius) const;

private:
	std::vector<MapCollisionRect> _walkables;
	std::vector<MapCollisionRect> _walls;
};