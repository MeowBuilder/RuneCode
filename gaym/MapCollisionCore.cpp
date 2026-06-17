#include "stdafx.h"
#include "MapCollisionCore.h"

#include <fstream>
#include <sstream>
#include <cmath>
#include <cstdlib>
#include <algorithm>

namespace
{
	float ClampFloat(float v, float minV, float maxV)
	{
		if (v < minV) return minV;
		if (v > maxV) return maxV;
		return v;
	}

	float AbsFloat(float v)
	{
		return v < 0.0f ? -v : v;
	}

	float MaxFloat(float a, float b)
	{
		return a > b ? a : b;
	}

	bool ExtractArrayText(const std::string& text, const char* key, std::string& outArrayText)
	{
		std::string keyText = "\"";
		keyText += key;
		keyText += "\"";

		size_t keyPos = text.find(keyText);
		if (keyPos == std::string::npos)
			return false;

		size_t arrayStart = text.find('[', keyPos);
		if (arrayStart == std::string::npos)
			return false;

		int depth = 0;
		for (size_t i = arrayStart; i < text.size(); ++i)
		{
			if (text[i] == '[')
				depth++;
			else if (text[i] == ']')
			{
				depth--;
				if (depth == 0)
				{
					outArrayText = text.substr(arrayStart, i - arrayStart + 1);
					return true;
				}
			}
		}

		return false;
	}

	std::vector<std::string> ExtractObjectTexts(const std::string& arrayText)
	{
		std::vector<std::string> objects;

		int depth = 0;
		size_t objectStart = std::string::npos;

		for (size_t i = 0; i < arrayText.size(); ++i)
		{
			if (arrayText[i] == '{')
			{
				if (depth == 0)
					objectStart = i;
				depth++;
			}
			else if (arrayText[i] == '}')
			{
				depth--;
				if (depth == 0 && objectStart != std::string::npos)
				{
					objects.push_back(arrayText.substr(objectStart, i - objectStart + 1));
					objectStart = std::string::npos;
				}
			}
		}

		return objects;
	}

	bool ExtractFloatValue(const std::string& objectText, const char* key, float& outValue)
	{
		std::string keyText = "\"";
		keyText += key;
		keyText += "\"";

		size_t keyPos = objectText.find(keyText);
		if (keyPos == std::string::npos)
			return false;

		size_t colonPos = objectText.find(':', keyPos);
		if (colonPos == std::string::npos)
			return false;

		const char* start = objectText.c_str() + colonPos + 1;
		char* end = nullptr;

		float value = std::strtof(start, &end);
		if (end == start)
			return false;

		outValue = value;
		return true;
	}

	bool ParseRectObject(const std::string& objectText, MapCollisionRect& outRect)
	{
		if (!ExtractFloatValue(objectText, "centerX", outRect.centerX)) return false;
		if (!ExtractFloatValue(objectText, "centerZ", outRect.centerZ)) return false;
		if (!ExtractFloatValue(objectText, "halfX", outRect.halfX)) return false;
		if (!ExtractFloatValue(objectText, "halfZ", outRect.halfZ)) return false;

		// yawRad가 없으면 0으로 처리한다.
		float yaw = 0.0f;
		if (ExtractFloatValue(objectText, "yawRad", yaw))
			outRect.yawRad = yaw;
		else
			outRect.yawRad = 0.0f;

		outRect.halfX = AbsFloat(outRect.halfX);
		outRect.halfZ = AbsFloat(outRect.halfZ);

		return outRect.halfX > 0.0f && outRect.halfZ > 0.0f;
	}
}

void MapCollisionCore::Clear()
{
	_walkables.clear();
	_walls.clear();
}

bool MapCollisionCore::LoadFromFile(const std::string& path)
{
	std::ifstream file(path, std::ios::binary);
	if (!file.is_open())
		return false;

	std::stringstream ss;
	ss << file.rdbuf();

	return LoadFromJsonText(ss.str());
}

bool MapCollisionCore::LoadFromJsonText(const std::string& text)
{
	Clear();

	std::string walkableArrayText;
	if (ExtractArrayText(text, "walkables", walkableArrayText))
	{
		std::vector<std::string> objects = ExtractObjectTexts(walkableArrayText);

		for (const std::string& objectText : objects)
		{
			MapCollisionRect rect;
			if (ParseRectObject(objectText, rect))
				_walkables.push_back(rect);
		}
	}

	std::string wallArrayText;
	if (ExtractArrayText(text, "walls", wallArrayText))
	{
		std::vector<std::string> objects = ExtractObjectTexts(wallArrayText);

		for (const std::string& objectText : objects)
		{
			MapCollisionRect rect;
			if (ParseRectObject(objectText, rect))
				_walls.push_back(rect);
		}
	}

	// walkable이 하나도 없으면 이동 가능 영역 자체가 없으므로 실패 처리
	return !_walkables.empty();
}

void MapCollisionCore::AddWalkableRect(const MapCollisionRect& rect)
{
	if (rect.halfX <= 0.0f || rect.halfZ <= 0.0f)
		return;

	_walkables.push_back(rect);
}

void MapCollisionCore::AddWallRect(const MapCollisionRect& rect)
{
	if (rect.halfX <= 0.0f || rect.halfZ <= 0.0f)
		return;

	_walls.push_back(rect);
}

bool MapCollisionCore::IsPointInsideRect(float x, float z, const MapCollisionRect& rect, float margin) const
{
	float dx = x - rect.centerX;
	float dz = z - rect.centerZ;

	float c = std::cos(rect.yawRad);
	float s = std::sin(rect.yawRad);

	// world 좌표를 rect local 좌표로 변환
	float localX = dx * c + dz * s;
	float localZ = -dx * s + dz * c;

	return AbsFloat(localX) <= rect.halfX + margin &&
		AbsFloat(localZ) <= rect.halfZ + margin;
}

bool MapCollisionCore::IsCircleOnWalkable(float x, float z, float radius) const
{
	if (_walkables.empty())
		return false;

	// 중심 + 원 둘레 샘플이 전부 walkable 위에 있어야 함
	static const float sampleDirs[8][2] =
	{
		{ 1.0f,  0.0f },
		{-1.0f,  0.0f },
		{ 0.0f,  1.0f },
		{ 0.0f, -1.0f },
		{ 0.7071067f,  0.7071067f },
		{-0.7071067f,  0.7071067f },
		{ 0.7071067f, -0.7071067f },
		{-0.7071067f, -0.7071067f }
	};

	auto isPointOnAnyWalkable = [&](float px, float pz) -> bool
		{
			for (const MapCollisionRect& rect : _walkables)
			{
				if (IsPointInsideRect(px, pz, rect, 0.05f))
					return true;
			}
			return false;
		};

	if (!isPointOnAnyWalkable(x, z))
		return false;

	if (radius <= 0.0f)
		return true;

	for (int i = 0; i < 8; ++i)
	{
		float px = x + sampleDirs[i][0] * radius;
		float pz = z + sampleDirs[i][1] * radius;

		if (!isPointOnAnyWalkable(px, pz))
			return false;
	}

	return true;
}

bool MapCollisionCore::IsCircleIntersectWall(float x, float z, float radius) const
{
	float r = MaxFloat(radius, 0.0f);

	for (const MapCollisionRect& rect : _walls)
	{
		float dx = x - rect.centerX;
		float dz = z - rect.centerZ;

		float c = std::cos(rect.yawRad);
		float s = std::sin(rect.yawRad);

		// world 좌표를 wall local 좌표로 변환
		float localX = dx * c + dz * s;
		float localZ = -dx * s + dz * c;

		float closestX = ClampFloat(localX, -rect.halfX, rect.halfX);
		float closestZ = ClampFloat(localZ, -rect.halfZ, rect.halfZ);

		float diffX = localX - closestX;
		float diffZ = localZ - closestZ;

		if (diffX * diffX + diffZ * diffZ <= r * r)
			return true;
	}

	return false;
}

bool MapCollisionCore::CanStand(float x, float z, float radius) const
{
	if (!IsCircleOnWalkable(x, z, radius))
		return false;

	if (IsCircleIntersectWall(x, z, radius))
		return false;

	return true;
}

CollisionMoveResult MapCollisionCore::ResolveMove(
	float fromX,
	float fromZ,
	float toX,
	float toZ,
	float radius
) const
{
	CollisionMoveResult result;
	result.x = fromX;
	result.z = fromZ;
	result.blocked = false;

	if (_walkables.empty())
	{
		result.blocked = true;
		return result;
	}

	bool fromValid = CanStand(fromX, fromZ, radius);
	bool toValid = CanStand(toX, toZ, radius);

	// 시작 위치가 잘못되어 있는데 목표 위치는 정상이라면 목표 위치로 복구 허용
	if (!fromValid && toValid)
	{
		result.x = toX;
		result.z = toZ;
		result.blocked = true;
		return result;
	}

	// 시작도 목표도 잘못되어 있으면 현재 위치 유지
	if (!fromValid && !toValid)
	{
		result.blocked = true;
		return result;
	}

	float dx = toX - fromX;
	float dz = toZ - fromZ;
	float dist = std::sqrt(dx * dx + dz * dz);

	if (dist <= 0.0001f)
	{
		if (toValid)
		{
			result.x = toX;
			result.z = toZ;
		}
		else
		{
			result.blocked = true;
		}

		return result;
	}

	// 빠른 이동/대쉬가 벽을 통과하지 않도록 이동 경로를 잘게 쪼개서 검사
	float maxStep = MaxFloat(0.25f, radius * 0.35f);
	int steps = static_cast<int>(std::ceil(dist / maxStep));
	if (steps < 1) steps = 1;

	float lastX = fromX;
	float lastZ = fromZ;

	for (int i = 1; i <= steps; ++i)
	{
		float t = static_cast<float>(i) / static_cast<float>(steps);

		float stepX = fromX + dx * t;
		float stepZ = fromZ + dz * t;

		if (CanStand(stepX, stepZ, radius))
		{
			lastX = stepX;
			lastZ = stepZ;
			continue;
		}

		// X축만 이동 가능한 경우 벽을 따라 미끄러지게 함
		if (CanStand(stepX, lastZ, radius))
		{
			lastX = stepX;
			result.blocked = true;
			continue;
		}

		// Z축만 이동 가능한 경우 벽을 따라 미끄러지게 함
		if (CanStand(lastX, stepZ, radius))
		{
			lastZ = stepZ;
			result.blocked = true;
			continue;
		}

		// 둘 다 불가능하면 마지막 안전 위치에서 정지
		result.x = lastX;
		result.z = lastZ;
		result.blocked = true;
		return result;
	}

	result.x = lastX;
	result.z = lastZ;
	return result;
}